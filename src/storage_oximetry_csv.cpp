#include "storage_oximetry_csv.h"
#include "oximetry_csv_capture.h"

#include <Arduino.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <fcntl.h>
#include <freertos/queue.h>
#include <unistd.h>

#include "debug_log.h"
#include "board_oximetry.h"
#include "calendar_utils.h"
#include "edf_time.h"
#include "large_byte_buffer.h"
#include "memory_manager.h"
#include "oximetry_archive.h"
#include "storage_internal.h"
#include "storage_service.h"
#include "storage_path.h"
#include "storage_usb.h"
#include "utc_time.h"

namespace aircannect::OximetryCsv {
namespace {
struct PartialWrite {
    bool created = false;
    size_t unconfirmed_end = 0;
};

bool write_at(const char *path, size_t offset, const void *data, size_t size,
              bool replace_partial, PartialWrite *partial) {
    const bool reopen = partial && partial->created;
    const int flags = offset || reopen ? O_WRONLY :
        O_WRONLY | O_CREAT | (replace_partial ? O_TRUNC : O_EXCL);
    const int fd = Storage::open_descriptor(path, flags);
    if (fd < 0) return false;

    if (partial) partial->created = true;
    const off_t end = lseek(fd, 0, SEEK_END);

    // Only our still-open recording may contain an unconfirmed write prefix.
    const bool positioned = end == off_t(offset) ||
        (reopen && end > off_t(offset) && size_t(end) <= partial->unconfirmed_end &&
         lseek(fd, offset, SEEK_SET) == off_t(offset));
    if (positioned && partial) partial->unconfirmed_end = offset + size;
    const bool ok = positioned &&
                    write(fd, data, size) == ssize_t(size) && fsync(fd) == 0;
    if (Storage::close_descriptor(path, fd) != 0 || !ok) return false;
    if (partial) partial->unconfirmed_end = 0;
    return true;
}
}  // namespace

bool write_chunk(const char *path, size_t offset, const void *data, size_t size,
                 bool replace_partial) {
    return write_at(path, offset, data, size, replace_partial, nullptr);
}

namespace {
constexpr size_t Batch = 4096;
constexpr uint32_t FlushMs = 15000, GapMs = 120000, DayMs = 86400000;
struct Input {
    int64_t utc_ms;
    uint32_t observed;
    int16_t oxygen, pulse, motion;
    float pi;
    bool measured, removed;
    char source[18];
    char peer[18], serial[33];
    int32_t device_offset;
};
struct Live {
    std::string source, part, final;
    std::unique_ptr<LargeByteBuffer> buffer;
    size_t used = 0, offset = 0;
    uint32_t first = 0, last = 0, dirty = 0;
    int64_t last_utc = 0;
    int night = 0;
    int32_t offset_minutes = 0;
    size_t serial_pos = 0;
    char pending_serial[33]{};
    uint32_t retry_at = 0, buffered_rows = 0;
    PartialWrite partial;
    bool closing = false, incomplete = false;
};

std::vector<Live, LargeAllocator<Live>> live;
QueueHandle_t queue;
StaticQueue_t queue_control;
void (*notify)();
std::atomic<bool> paused{false}, quiet{true};
std::atomic<bool> mounted{false};
std::atomic<unsigned> lost{0};
size_t buffer_budget = 0;
File root, directory;
bool recovered = false;

// The input FIFO has no room for a gap marker precisely when we need one.
struct Gap {
    char source[18]{};
    uint32_t first = 0, last = 0;
};
struct Gaps {
    Gap notes[1 + AC_OXIMETRY_SENSOR_MAX_KNOWN]{};
    bool untracked = false;
} gaps;
portMUX_TYPE gap_lock = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> have_gaps{false};

void input_lost(const Input &in) {
    ++lost;
    portENTER_CRITICAL(&gap_lock);
    auto end = std::end(gaps.notes);
    auto note = std::find_if(std::begin(gaps.notes), end, [&](const Gap &g) {
        return !g.source[0] || !strcmp(g.source, in.source);
    });
    if (note == end) {
        gaps.untracked = true;
    } else {
        if (!note->source[0]) {
            memcpy(note->source, in.source, sizeof(note->source));
            note->first = in.observed;
        }
        note->last = in.observed;
    }
    have_gaps.store(true);
    portEXIT_CRITICAL(&gap_lock);
}

void mark_input_gaps() {
    if (!have_gaps.load()) return;
    portENTER_CRITICAL(&gap_lock);
    const Gaps observed = gaps;
    gaps = {};
    have_gaps.store(false);
    portEXIT_CRITICAL(&gap_lock);

    for (auto &f : live) {
        if (observed.untracked) f.incomplete = true;
        for (const auto &gap : observed.notes) {
            if (gap.source[0] && f.source == gap.source &&
                int32_t(gap.last - f.first) >= -int32_t(GapMs) &&
                int32_t(gap.first - f.last) <= int32_t(GapMs)) f.incomplete = true;
        }
    }
}

size_t allocated_buffers() {
    size_t bytes = 0;
    for (const auto &f : live) bytes += f.buffer->size();
    return bytes;
}

bool vacant(const std::string &path) {
    struct stat st{};
    return !Storage::file_stat(path.c_str(), st) && errno == ENOENT;
}

bool needs_quarantine(const Live &f) {
    return f.incomplete && f.part.compare(f.part.size() - 5, 5, ".part") == 0;
}

bool flush(Live &f) {
    if (!f.partial.created && !Storage::ensure_parent_directories(f.part.c_str())) return false;
    if (needs_quarantine(f)) {
        auto incomplete = f.part.substr(0, f.part.size() - 4) + "incomplete";
        if (f.partial.created && (!vacant(incomplete) ||
            !Storage::rename(f.part.c_str(), incomplete.c_str()))) return false;
        f.part.swap(incomplete);
        Log::logf(CAT_OXI, LOG_WARN, "keeping incomplete recording: %s\n", f.part.c_str());
    }
    if (f.pending_serial[0]) {
        const size_t n = strlen(f.pending_serial);
        if (!f.offset) {
            memcpy(f.buffer->data() + f.serial_pos, f.pending_serial, n);
        } else {
            const int fd = Storage::open_descriptor(f.part.c_str(), O_WRONLY);
            if (fd < 0) return false;
            const bool ok = lseek(fd, f.serial_pos, SEEK_SET) == off_t(f.serial_pos) &&
                write(fd, f.pending_serial, n) == ssize_t(n) && fsync(fd) == 0;
            if (Storage::close_descriptor(f.part.c_str(), fd) != 0 || !ok) return false;
        }
        f.serial_pos = 0;
        f.pending_serial[0] = 0;
    }

    if (f.used && !write_at(f.part.c_str(), f.offset, f.buffer->data(), f.used,
                            false, &f.partial)) return false;
    f.offset += f.used;
    f.used = f.buffered_rows = 0;
    if (f.buffer->size() > Batch) {
        if (auto small = LargeByteBuffer::allocate(Batch)) f.buffer = std::move(small);
    }
    return true;
}

bool publish(const std::string &part, const std::string &final) {
    if (!vacant(final) || !Storage::ensure_parent_directories(final.c_str()) ||
        !Storage::rename(part.c_str(), final.c_str())) return false;
    const auto dir = std::string_view(final).substr(sizeof(OximetryArchiveRoot));
    const auto slash = dir.find('/');
    StorageService::notify_oximetry_publication(
        ReportOximetryChange::file(OximetryArchiveFile::LiveCsv, dir.substr(0, slash),
                                   dir.substr(slash + 1, 8)));
    return true;
}

bool finish(Live &f) {
    return flush(f) && (f.incomplete || publish(f.part, f.final));
}

bool due(const Live &f, uint32_t now) {
    return !f.retry_at || int32_t(now - f.retry_at) >= 0;
}

void deferred(Live &f, uint32_t now) {
    if (!f.retry_at)
        Log::logf(CAT_OXI, LOG_WARN, "live CSV write failed: %s; retaining rows\n", f.source.c_str());
    f.retry_at = (now + 60000) | 1;
}

// Only files with our live filename and header can be recovered. A derived
// partial is in a date directory and never reaches this function.
bool recover(const std::string &path, const char *name) {
    const size_t n = strlen(name);
    if (n < 28 || strncmp(name, "live-", 5) ||
        strspn(name + 5, "0123456789") != 8 || name[13] != '-' ||
        strcmp(name + n - 9, ".csv.part")) return true;

    const std::string final = path.substr(0, path.size() - n) +
        std::string(name + 5, 8) + "/" + std::string(name, n - 5);
    if (!vacant(final)) return false;

    const int fd = Storage::open_descriptor(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    char header[MaxHeader], tail[MaxRow];
    const off_t size = lseek(fd, 0, SEEK_END);
    size_t header_n = sizeof(Columns) - 1;
    const size_t count = size >= 0 ? std::min<size_t>(size, sizeof(tail)) : 0;
    const ssize_t got = size >= off_t(header_n) && lseek(fd, 0, SEEK_SET) == 0
        ? read(fd, header, sizeof(header)) : -1;
    const std::string_view prefix(header, got > 0 ? size_t(got) : 0);
    size_t columns_at = 0;
    if (prefix.substr(0, 9) == "# Source=") {
        const size_t eol = prefix.find("\r\n");
        if (eol == prefix.npos || prefix.substr(0, eol).find(" Version=2 ") == prefix.npos) {
            Storage::close_descriptor(path.c_str(), fd);
            return false;
        }
        columns_at = eol + 2;
    }
    header_n += columns_at;
    bool ok = prefix.substr(columns_at, sizeof(Columns)-1) == Columns &&
              lseek(fd, size - count, SEEK_SET) == size - off_t(count) &&
              read(fd, tail, count) == ssize_t(count);

    size_t end = count;
    while (ok && end && tail[end-1] != '\n') --end;
    const off_t complete = size - count + end;
    ok = ok && end && complete >= off_t(header_n);
    if (ok && end != count)
        ok = ftruncate(fd, complete) == 0 && fsync(fd) == 0;
    ok = Storage::close_descriptor(path.c_str(), fd) == 0 && ok;
    if (!ok) return false;
    if (complete == off_t(header_n)) return Storage::remove(path.c_str());

    return publish(path, final);
}

bool recovery_step() {
    if (recovered) return false;
    if (!root) {
        struct stat st{};
        if (!Storage::file_stat(OximetryArchiveRoot, st) && errno == ENOENT) {
            recovered = true;
            return true;
        }
        root = Storage::open(OximetryArchiveRoot, FILE_READ);
        if (!root) return false;
    }
    bool dir = false;
    if (directory) {
        const String path = directory.getNextFileName(&dir);
        if (!path.length()) directory.close();
        else if (!dir && !recover(path.c_str(), storage_basename_from_path(path.c_str())))
            Log::logf(CAT_OXI, LOG_WARN, "CSV recovery kept %s\n", path.c_str());
    } else {
        const String path = root.getNextFileName(&dir);
        if (!path.length()) {
            root.close();
            recovered = true;
        } else if (dir && oximetry_source_valid(storage_basename_from_path(path.c_str()))) {
            directory = Storage::open(path.c_str(), FILE_READ);
        }
    }
    return true;
}

bool append(const Input &in, uint32_t now) {
    auto it = std::find_if(live.begin(), live.end(),
                           [&](const Live &f) { return !f.closing && f.source == in.source; });
    const time_t epoch = in.utc_ms / 1000;
    tm local{};
    const bool valid_time = in.utc_ms > 0 && localtime_r(&epoch, &local);
    const int night = valid_time ? calendar_days_from_civil(local.tm_year + 1900,
        local.tm_mon + 1, local.tm_mday) - (local.tm_hour < 12) : 0;
    if (it != live.end() && (!valid_time || in.utc_ms < it->last_utc ||
        in.observed - it->first >= DayMs || in.observed - it->last >= GapMs ||
        it->night != night)) {
        it->closing = true;
        it = live.end();
    }
    if (!valid_time || in.removed || (it == live.end() && !in.measured)) return true;

    if (it == live.end()) {
        Live f;
        f.offset_minutes = in.device_offset;
        if (f.offset_minutes == INT32_MIN &&
            !edf_configured_timezone_offset_minutes(in.utc_ms, f.offset_minutes)) return false;
        if (allocated_buffers() + Batch > buffer_budget) return false;
        f.buffer = LargeByteBuffer::allocate(Batch);
        if (!f.buffer) return false;
        char day[12], id[72];
        if (!strftime(day, sizeof(day), "%Y%m%d", &local)) return false;
        snprintf(id, sizeof(id), "live-%s-%lld-%08lx.csv", day,
                 (long long)in.utc_ms, (unsigned long)esp_random());
        const std::string base = std::string(OximetryArchiveRoot) + "/" + in.source + "/";
        f.part = base + id + ".part";
        f.final = base + day + "/" + id;
        char header[MaxHeader];
        f.used = format_header(header, in.source, id, f.offset_minutes, in.peer, in.serial);
        if (!f.used) return false;
        memcpy(f.buffer->data(), header, f.used);
        if (in.peer[0] && !in.serial[0])
            f.serial_pos = f.used - (sizeof(Columns) - 1) - 2 - 32;
        f.first = f.last = in.observed;
        f.dirty = now;
        f.source = in.source;
        live.push_back(std::move(f));
        it = live.end()-1;
    }
    // The first valid reading can precede the ring's identity response.
    if (it->serial_pos && in.serial[0]) {
        memcpy(it->pending_serial, in.serial, sizeof(in.serial));
    }

    char row[MaxRow];
    const size_t len = format_row(row, in.utc_ms, it->offset_minutes,
                                  in.oxygen, in.pulse, in.motion, in.pi);
    if (!len) return false;
    if (it->used + len > Batch && due(*it, now)) {
        if (!flush(*it)) deferred(*it, now);
        else it->retry_at = 0;
    }
    if (it->used + len > it->buffer->size()) {
        if (allocated_buffers() + Batch > buffer_budget || !it->buffer->grow(it->buffer->size() + Batch)) {
            it->incomplete = true;
            ++lost;
            return true;
        }
    }
    if (!it->used) it->dirty = now;
    memcpy(it->buffer->data() + it->used, row, len);
    it->used += len;
    ++it->buffered_rows;
    it->night = night;
    it->last_utc = in.utc_ms;
    if (in.measured) it->last = in.observed;
    if (!it->offset && due(*it, now)) {
        if (!flush(*it)) deferred(*it, now);
        else it->retry_at = 0;
    }
    return true;
}
}  // namespace

bool begin(void (*wake)()) {
    if (queue) return true;
    buffer_budget = Memory::buffer_budget(64 * 1024, 16 * 1024);
    const size_t count = buffer_budget / sizeof(Input);
    auto *bytes = static_cast<uint8_t *>(Memory::alloc_large(count * sizeof(Input), false));
    if (!bytes) return false;
    notify = wake;
    queue = xQueueCreateStatic(count, sizeof(Input), bytes, &queue_control);
    return queue != nullptr;
}

void observe(const OximetrySample &s, int64_t utc_ms, int32_t device_offset_minutes) {
    if (!mounted.load() || !queue || paused.load() || StorageUsb::suspended()) return;
    Input in{};
    if (s.source == OximetrySource::Udp) strcpy(in.source, "udp");
    else if (s.source != OximetrySource::Ble || !oximetry_ble_source(s.peer, in.source)) return;

    memcpy(in.peer, s.peer, sizeof(in.peer));
    memcpy(in.serial, s.serial, sizeof(in.serial));
    in.device_offset = device_offset_minutes;
    in.utc_ms = utc_ms;
    in.observed = s.observed_ms ? s.observed_ms : millis();
    in.removed = s.contact_known && !s.contact_present && s.spo2 < 0 && s.pulse_bpm < 0;
    in.measured = !in.removed && (s.spo2 >= 0 || s.pulse_bpm >= 0 || s.pi_percent >= 0 || s.motion >= 0);
    in.oxygen = s.spo2 >= 0 ? s.spo2 : 255;
    in.pulse = s.pulse_bpm >= 0 ? s.pulse_bpm : 255;
    in.pi = s.pi_percent;
    in.motion = s.motion;
    if (!xQueueSend(queue, &in, 0)) input_lost(in);
    quiet.store(false);
    notify();
}

void pause(bool value) {
    if (paused.exchange(value) != value && notify) notify();
}

bool drained() { return quiet.load() && (!queue || !uxQueueMessagesWaiting(queue)); }
uint32_t lost_rows() { return lost.load(); }

void set_mounted(bool value) {
    if (mounted.exchange(value) == value || value) return;
    if (queue) {
        size_t rows = uxQueueMessagesWaiting(queue);
        for (const auto &f : live) rows += f.buffered_rows;
        if (rows) {
            lost += rows;
            Log::logf(CAT_OXI, LOG_WARN, "SD unavailable; live CSV lost rows=%u\n", unsigned(rows));
        }
        xQueueReset(queue);
        mark_input_gaps();
    }
    if (!live.empty()) {
        Log::logf(CAT_OXI, LOG_WARN, "SD unavailable; keeping flushed live CSV rows\n");
        live.clear();
    }
    root.close();
    directory.close();
    recovered = false;
    quiet.store(true);
    reset_archive();
}

bool live_step(uint32_t now) try {
    if (!queue) return false;
    const bool stop = paused.load() || StorageUsb::suspended();
    if (!stop && recovery_step()) return true;
    static uint32_t last_warning = 0, reported = 0;
    const auto total = lost.load();
    if (total != reported && (!last_warning || now - last_warning >= 60000)) {
        Log::logf(CAT_OXI, LOG_WARN, "live CSV lost rows=%u total=%u\n", total - reported, total);
        last_warning = now;
        reported = total;
    }

    Input in{};
    const bool queued = xQueuePeek(queue, &in, 0);
    if (queued && append(in, now)) {
        xQueueReceive(queue, &in, 0);
        return true;
    }
    if (!queued) mark_input_gaps();
    for (auto it = live.begin(); it != live.end(); ++it) {
        if (!due(*it, now)) continue;
        if (stop || it->closing || now - it->last >= GapMs || now - it->first >= DayMs) {
            if (!finish(*it)) deferred(*it, now);
            else live.erase(it);
            return true;
        }
        if (needs_quarantine(*it) || (it->used && now - it->dirty >= FlushMs)) {
            if (!flush(*it)) deferred(*it, now);
            else it->retry_at = 0;
            return true;
        }
    }
    quiet.store(live.empty());
    return false;
} catch (const std::bad_alloc &) {
    return false;
}

bool release_media() {
    if (live_step(millis())) return false;
    set_mounted(false);
    return true;
}

}  // namespace aircannect::OximetryCsv
