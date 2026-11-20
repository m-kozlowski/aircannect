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

bool write_chunk(const char *path, size_t offset, const void *data, size_t size,
                 bool replace_partial) {
    const int flags = offset ? O_WRONLY :
        O_WRONLY | O_CREAT | (replace_partial ? O_TRUNC : O_EXCL);
    const int fd = Storage::open_descriptor(path, flags);
    if (fd < 0) return false;

    const bool ok = lseek(fd, 0, SEEK_END) == off_t(offset) &&
                    write(fd, data, size) == ssize_t(size) && fsync(fd) == 0;
    return Storage::close_descriptor(path, fd) == 0 && ok;
}

namespace {
constexpr size_t Batch = 4096;
constexpr uint32_t FlushMs = 15000, GapMs = 120000, DayMs = 86400000;
struct Input {
    int64_t utc_ms;
    uint32_t observed;
    int16_t oxygen, pulse;
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
};

std::vector<Live, LargeAllocator<Live>> live;
QueueHandle_t queue;
StaticQueue_t queue_control;
void (*notify)();
std::atomic<bool> paused{false}, quiet{true};
std::atomic<bool> mounted{false};
std::atomic<unsigned> lost{0};
uint32_t retry_at = 0;
File root, directory;
bool recovered = false;

bool flush(Live &f) {
    if (!f.used) return true;
    if (!write_chunk(f.part.c_str(), f.offset, f.buffer->data(), f.used)) return false;
    f.offset += f.used;
    f.used = 0;
    return true;
}

bool vacant(const std::string &path) {
    struct stat st{};
    return !Storage::file_stat(path.c_str(), st) && errno == ENOENT;
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
    return flush(f) && publish(f.part, f.final);
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
                           [&](const Live &f) { return f.source == in.source; });
    const time_t epoch = in.utc_ms / 1000;
    tm local{};
    const bool valid_time = in.utc_ms > 0 && localtime_r(&epoch, &local);
    const int night = valid_time ? calendar_days_from_civil(local.tm_year + 1900,
        local.tm_mon + 1, local.tm_mday) - (local.tm_hour < 12) : 0;
    if (it != live.end() && (!valid_time || in.utc_ms < it->last_utc ||
        in.observed - it->first >= DayMs || in.observed - it->last >= GapMs ||
        it->night != night)) {
        if (!finish(*it)) return false;
        live.erase(it);
        it = live.end();
    }
    if (!valid_time || in.removed || (it == live.end() && !in.measured)) return true;

    if (it == live.end()) {
        Live f;
        f.offset_minutes = in.device_offset;
        if (f.offset_minutes == INT32_MIN &&
            !edf_configured_timezone_offset_minutes(in.utc_ms, f.offset_minutes)) return false;
        f.buffer = LargeByteBuffer::allocate(Batch);
        if (!f.buffer) return false;
        char day[12], id[72];
        if (!strftime(day, sizeof(day), "%Y%m%d", &local)) return false;
        snprintf(id, sizeof(id), "live-%s-%lld-%08lx.csv", day,
                 (long long)in.utc_ms, (unsigned long)esp_random());
        const std::string base = std::string(OximetryArchiveRoot) + "/" + in.source + "/";
        f.part = base + id + ".part";
        f.final = base + day + "/" + id;
        if (!Storage::ensure_parent_directories(f.part.c_str())) return false;
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
        const int fd = Storage::open_descriptor(it->part.c_str(), O_WRONLY);
        if (fd < 0) return false;
        const size_t n = strlen(in.serial);
        const bool ok = lseek(fd, it->serial_pos, SEEK_SET) == off_t(it->serial_pos) &&
                        write(fd, in.serial, n) == ssize_t(n) && fsync(fd) == 0;
        if (Storage::close_descriptor(it->part.c_str(), fd) != 0 || !ok) return false;
        it->serial_pos = 0;
    }

    char row[MaxRow];
    const size_t len = format_row(row, in.utc_ms, it->offset_minutes, in.oxygen, in.pulse, -1, in.pi);
    if (!len || (it->used + len > Batch && !flush(*it))) return false;
    if (!it->used) it->dirty = now;
    memcpy(it->buffer->data() + it->used, row, len);
    it->used += len;
    it->night = night;
    it->last_utc = in.utc_ms;
    if (in.measured) it->last = in.observed;
    return it->offset || flush(*it);
}

void failed(uint32_t now) {
    Log::logf(CAT_OXI, LOG_WARN, "live CSV write failed; keeping partial recording\n");
    live.clear();
    directory.close();
    root.close();
    recovered = false;
    retry_at = now + 60000;
}
}  // namespace

bool begin(void (*wake)()) {
    if (queue) return true;
    constexpr size_t Count = 32;
    auto *bytes = static_cast<uint8_t *>(Memory::alloc_large(Count * sizeof(Input)));
    if (!bytes) return false;
    notify = wake;
    queue = xQueueCreateStatic(Count, sizeof(Input), bytes, &queue_control);
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
    in.removed = s.contact_known && !s.contact_present;
    in.measured = !in.removed && (s.valid || s.pi_percent >= 0);
    in.oxygen = s.valid ? s.spo2 : 255;
    in.pulse = s.valid ? s.pulse_bpm : 255;
    in.pi = s.pi_percent;
    if (!xQueueSend(queue, &in, 0)) ++lost;
    quiet.store(false);
    notify();
}

void pause(bool value) {
    if (paused.exchange(value) != value && notify) notify();
}

bool drained() { return quiet.load() && (!queue || !uxQueueMessagesWaiting(queue)); }

void set_mounted(bool value) {
    if (mounted.exchange(value) == value || value) return;
    if (queue) xQueueReset(queue);
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
    if (retry_at && int32_t(now - retry_at) < 0) {
        xQueueReset(queue);
        quiet.store(true);
        return false;
    }
    retry_at = 0;
    if (!stop && recovery_step()) return true;
    static uint32_t last_warning = 0;
    if (lost.load() && (!last_warning || now - last_warning >= 60000)) {
        Log::logf(CAT_OXI, LOG_WARN, "live CSV input overflow samples=%u\n", lost.exchange(0));
        last_warning = now;
    }

    Input in{};
    if (xQueueReceive(queue, &in, 0)) {
        if (!append(in, now)) failed(now);
        return true;
    }
    for (auto it = live.begin(); it != live.end(); ++it) {
        if (stop || now - it->last >= GapMs || now - it->first >= DayMs) {
            if (!finish(*it)) failed(now);
            else live.erase(it);
            return true;
        }
        if (it->used && now - it->dirty >= FlushMs) {
            if (!flush(*it)) failed(now);
            return true;
        }
    }
    quiet.store(live.empty());
    return false;
} catch (const std::bad_alloc &) {
    failed(now);
    return false;
}

bool release_media() {
    if (live_step(millis())) return false;
    set_mounted(false);
    return true;
}

}  // namespace aircannect::OximetryCsv
