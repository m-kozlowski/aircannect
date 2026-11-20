#include "storage_oximetry_csv.h"
#include "oximetry_csv_capture.h"

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

#include "debug_log.h"
#include "large_byte_buffer.h"
#include "storage_internal.h"
#include "storage_service.h"
#include "storage_space_policy.h"

namespace aircannect::OximetryCsv {
namespace {
std::shared_ptr<const ReportOximetryCatalog> pending, current;
size_t index = 0, offset = 0;
uint32_t sample = 0, retry_at = 0;
bool replacing = false;
std::string final, part;
std::unique_ptr<LargeByteBuffer> chunk;
OperationTicket ticket;

void next() {
    ++index;
    sample = offset = 0;
    replacing = false;
    chunk.reset();
}

bool retry(uint32_t now, const char *error) {
    Log::logf(CAT_OXI, LOG_WARN, "ring CSV %s file=%s\n", error, final.c_str());
    retry_at = now + 60000;
    next();
    return true;
}

bool step(uint32_t now) {
    auto &publisher = StorageService::atomic_write_port();
    if (ticket.valid()) {
        StorageAtomicWriteCompletion done;
        if (!publisher.take_completion(ticket, done)) return false;
        ticket = {};
        if (done.outcome.disposition != OperationDisposition::Succeeded)
            return retry(now, done.error);
        const auto &r = (*current)[index];
        StorageService::notify_oximetry_publication(ReportOximetryChange::file(
            OximetryArchiveFile::RingCsv, r.source, std::string_view(r.id, 8)));
        next();
        return true;
    }

    const auto latest = std::atomic_load(&pending);
    if (current != latest) {
        current = latest;
        next();
        index = 0;
        retry_at = 0;
    }
    if (current && index >= current->size() && retry_at &&
        int32_t(now - retry_at) >= 0) {
        index = retry_at = 0;
    }
    if (!current || index >= current->size() || !publisher.recovery_complete()) return false;
    const auto &r = (*current)[index];
    if (r.csv_bytes || r.start_ms < r.noon_s * 1000) {
        next(); // Only the first slice of a vendor BIN needs conversion.
        return true;
    }

    if (sample == r.samples) {
        StorageAtomicWriteCommand command;
        command.path = final;
        command.staged_path = part;
        command.staged_size = offset;
        command.replace_existing = replacing;
        command.generation = 1;
        const auto submit = publisher.request_write(command);
        if (submit.accepted()) ticket = submit.ticket;
        else if (submit.admission == OperationAdmission::Rejected)
            return retry(now, "publication rejected");
        return submit.accepted();
    }

    const auto bin = r.path();
    constexpr size_t Count = 32;
    uint8_t records[Count * RingBinLayout::MaxStride];
    const size_t count = std::min<size_t>(Count, r.samples - sample);
    const size_t length = count * r.bin.stride();
    if (!sample) {
        final = bin.substr(0, bin.size()-3) + "csv";
        part = final + ".part";
    }
    const int fd = Storage::open_descriptor(bin.c_str(), O_RDONLY);
    if (fd < 0) return retry(now, "BIN open failed");
    const off_t pos = r.bin.offset(sample);
    const bool read_ok = lseek(fd, pos, SEEK_SET) == pos &&
                         read(fd, records, length) == ssize_t(length);
    if (Storage::close_descriptor(bin.c_str(), fd) != 0 || !read_ok)
        return retry(now, "BIN read failed");

    if (!chunk) chunk = LargeByteBuffer::allocate(4096);
    if (!chunk) return retry(now, "buffer unavailable");
    size_t used = 0;
    if (!sample) {
        char header[MaxHeader];
        used = format_header(header, r.source, r.id, r.offset_minutes, r.peer, r.serial);
        if (!used) return retry(now, "header too long");
        memcpy(chunk->data(), header, used);
    }
    for (size_t i = 0; i < count; ++i) {
        const auto value = r.bin.decode(records + i * r.bin.stride());
        char row[MaxRow];
        const size_t n = format_row(row, r.start_ms + int64_t(sample + i) * r.interval_s * 1000,
                                    r.offset_minutes, value.oxygen, value.pulse, value.motion, -1);
        if (!n || used + n > chunk->size()) return retry(now, "invalid row");
        memcpy(chunk->data() + used, row, n);
        used += n;

        if (!sample && !i) {
            struct stat info{};
            replacing = Storage::file_stat(final.c_str(), info);
            if (!replacing && errno != ENOENT) return retry(now, "CSV stat failed");
            if (!replacing) continue;

            // A later clock observation can change the first timestamp. Never
            // replace an unrelated file delivered by a user over USB.
            char first[MaxHeader + MaxRow];
            if (used > sizeof(first)) return retry(now, "CSV prefix too long");
            const int csv = Storage::open_descriptor(final.c_str(), O_RDONLY);
            if (csv < 0) return retry(now, "CSV open failed");
            const ssize_t got = read(csv, first, used);
            if (Storage::close_descriptor(final.c_str(), csv) != 0 || got < 0)
                return retry(now, "CSV read failed");
            if (size_t(got) == used && !memcmp(first, chunk->data(), used)) {
                next();
                return true;
            }
            const std::string identity = std::string("# Source=") + r.source + "/" + r.id;
            const std::string_view existing(first, got);
            if (existing.substr(0, identity.size()) != identity ||
                (existing.substr(identity.size(), 12) != " Version=2 O" &&
                 existing.substr(identity.size(), sizeof(Columns)+1) != std::string("\r\n") + Columns)) {
                Log::logf(CAT_OXI, LOG_WARN, "ring CSV name occupied: %s\n", final.c_str());
                next();
                return true;
            }
        }
    }
    const auto space = Storage::space_snapshot();
    if (space.known && !space.fits(used, StorageSpacePolicy::ReclaimBelow))
        return retry(now, "insufficient space");
    if (!write_chunk(part.c_str(), offset, chunk->data(), used, true))
        return retry(now, "write failed");
    offset += used;
    sample += count;
    return true;
}
}  // namespace

void publish_catalog(std::shared_ptr<const ReportOximetryCatalog> catalog) {
    std::atomic_store(&pending, std::move(catalog));
}

void reset_archive() {
    if (ticket.valid()) StorageService::atomic_write_port().abandon(ticket);
    ticket = {};
    current.reset();
    publish_catalog({});
    next();
    index = retry_at = 0;
}

bool archive_step(uint32_t now) try {
    return step(now);
} catch (const std::bad_alloc &) {
    return retry(now, "allocation failed");
}

}  // namespace aircannect::OximetryCsv
