#include "storage_oximetry_csv.h"
#include "oximetry_csv_capture.h"

#include <algorithm>
#include <fcntl.h>
#include <unistd.h>

#include "debug_log.h"
#include "large_byte_buffer.h"
#include "memory_manager.h"
#include "storage_internal.h"
#include "storage_service.h"
#include "storage_space_policy.h"

namespace aircannect::OximetryCsv {
namespace {
std::shared_ptr<const ReportOximetryCatalog> pending, current;
std::vector<uint8_t, LargeAllocator<uint8_t>> checked;
size_t index = 0, offset = 0, used = 0;
size_t input_pos = 0, input_end = 0;
uint32_t sample = 0, retry_at = 0;
bool replacing = false, rewind_after_file = false;
std::string final, part;
std::unique_ptr<LargeByteBuffer> chunk, input;
OperationTicket ticket;

void reset_file() {
    sample = offset = used = input_pos = input_end = 0;
    replacing = false;
    input.reset();
    chunk.reset();
}

void next(bool complete = true) {
    if (complete && index < checked.size()) checked[index] = 1;
    index = rewind_after_file ? 0 : index + 1;
    rewind_after_file = false;
    reset_file();
}

void adopt(std::shared_ptr<const ReportOximetryCatalog> latest) {
    decltype(checked) seen(latest ? latest->size() : 0, 0);
    size_t resume = seen.size();
    size_t old = 0;
    for (size_t i = 0; current && i < seen.size(); ++i) {
        const auto &r = (*latest)[i];
        if (r.csv_bytes || r.start_ms < r.noon_s * 1000) continue;
        while (old < current->size() && (*current)[old].key() < r.key()) ++old;
        if (old == current->size()) break;
        const auto &before = (*current)[old];
        if (!r.same_samples(before) || r.offset_minutes != before.offset_minutes) continue;

        if (!r.missing_csv_scan || r.missing_csv_scan == before.missing_csv_scan)
            seen[i] = checked[old];
        if (old == index && sample) resume = i;
    }

    rewind_after_file = resume < seen.size();
    if (!rewind_after_file) reset_file();
    index = rewind_after_file ? resume : 0;
    checked.swap(seen);
    current = std::move(latest);
}

bool retry(uint32_t now, const char *error) {
    Log::logf(CAT_OXI, LOG_WARN, "ring CSV %s file=%s\n", error, final.c_str());
    retry_at = now + 60000;
    next(false);
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
    if (current != latest) adopt(latest);
    if (current && index >= current->size() && retry_at &&
        int32_t(now - retry_at) >= 0) {
        index = retry_at = 0;
    }
    while (index < checked.size() && checked[index]) ++index;
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

    constexpr size_t Count = 32;
    const size_t stride = r.bin.stride();
    uint8_t first[RingBinLayout::MaxStride];
    if (input_pos == input_end) {
        const auto bin = r.path();
        size_t count = 1; // Existing CSVs need only the first timestamp and row.
        if (!sample) {
            final = bin.substr(0, bin.size()-3) + "csv";
            part = final + ".part";
        } else {
            if (!input) input = LargeByteBuffer::allocate(Memory::buffer_budget(8192, 2048));
            if (!input) return retry(now, "buffer unavailable");
            count = std::min<size_t>(input->size() / stride, r.samples - sample);
        }

        const int fd = Storage::open_descriptor(bin.c_str(), O_RDONLY);
        if (fd < 0) return retry(now, "BIN open failed");
        const off_t pos = r.bin.offset(sample);
        const size_t length = count * stride;
        const bool ok = lseek(fd, pos, SEEK_SET) == pos &&
                        read(fd, sample ? input->data() : first, length) == ssize_t(length);
        if (Storage::close_descriptor(bin.c_str(), fd) != 0 || !ok)
            return retry(now, "BIN read failed");
        input_pos = 0;
        input_end = count;
    }

    const uint8_t *records = sample ? input->data() + input_pos * stride : first;
    const size_t count = std::min(Count, input_end - input_pos);
    for (size_t i = 0; i < count; ++i) {
        const auto value = r.bin.decode(records + i * r.bin.stride());
        char row[MaxRow];
        const size_t n = format_row(row, r.start_ms + int64_t(sample + i) * r.interval_s * 1000,
                                    r.offset_minutes, value.oxygen, value.pulse, value.motion, -1);
        if (!n) return retry(now, "invalid row");

        if (!sample && !i) {
            struct stat info{};
            replacing = Storage::file_stat(final.c_str(), info);
            if (!replacing && errno != ENOENT) return retry(now, "CSV stat failed");
            if (replacing) {
                // A later clock observation can change the first timestamp. Never
                // replace an unrelated file delivered by a user over USB.
                char prefix[MaxHeader + MaxRow];
                const int csv = Storage::open_descriptor(final.c_str(), O_RDONLY);
                if (csv < 0) return retry(now, "CSV open failed");
                const auto wanted = std::min<off_t>(sizeof(prefix), info.st_size);
                const ssize_t got = read(csv, prefix, wanted);
                if (Storage::close_descriptor(final.c_str(), csv) != 0 || got < 0 || got != wanted)
                    return retry(now, "CSV read failed");

                const auto status = check_ring_csv({prefix, size_t(got)}, r.source, r.id,
                                                   r.offset_minutes, {row, n});

                if (status != RingCsvStatus::Rebuild) {
                    if (status == RingCsvStatus::Unrecognized)
                        Log::logf(CAT_OXI, LOG_WARN, "ring CSV name occupied: %s\n", final.c_str());
                    next();
                    return true;
                }
            }

            chunk = LargeByteBuffer::allocate(Memory::buffer_budget(64 * 1024, 16 * 1024));
            if (!chunk) return retry(now, "buffer unavailable");
            char header[MaxHeader];
            used = format_header(header, r.source, r.id, r.offset_minutes, r.peer, r.serial);
            if (!used) return retry(now, "header too long");
            memcpy(chunk->data(), header, used);
        }

        memcpy(chunk->data() + used, row, n);
        used += n;
    }

    sample += count;
    input_pos += count;
    if (sample < r.samples && chunk->size() - used >= Count * MaxRow) return true;

    const auto space = Storage::space_snapshot();
    if (space.known && !space.fits(used, StorageSpacePolicy::ReclaimBelow))
        return retry(now, "insufficient space");
    if (!write_chunk(part.c_str(), offset, chunk->data(), used, true))
        return retry(now, "write failed");
    offset += used;
    used = 0;
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
    decltype(checked){}.swap(checked);
    reset_file();
    rewind_after_file = false;
    index = retry_at = 0;
}

bool archive_step(uint32_t now) try {
    return step(now);
} catch (const std::bad_alloc &) {
    return retry(now, "allocation failed");
}

}  // namespace aircannect::OximetryCsv
