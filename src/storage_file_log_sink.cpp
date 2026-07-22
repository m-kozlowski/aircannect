#include "storage_file_log_sink.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "fixed_queue.h"
#include "memory_manager.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

static constexpr size_t FILE_LOG_TIMESTAMP_BYTES = 23;
static constexpr size_t FILE_LOG_LINE_MAX =
    FILE_LOG_TIMESTAMP_BYTES + 1 + AC_LOG_LINE_MAX + 2;

bool format_archive_path(uint8_t index, char *out, size_t out_size) {
    if (!out || out_size == 0 || index == 0) return false;

    const int length = snprintf(out, out_size, "%s.%u", AC_FILE_LOG_PATH,
                                static_cast<unsigned>(index));
    return length > 0 && length < static_cast<int>(out_size);
}

}  // namespace

struct StorageFileLogSink::Line {
    uint16_t length = 0;
    char bytes[FILE_LOG_LINE_MAX] = {};
};

class StorageFileLogSink::Queue final
    : public FixedQueue<Line, AC_FILE_LOG_QUEUE_DEPTH> {};

bool StorageFileLogSink::lock(uint32_t timeout_ms) const {
    return lock_ &&
           xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void StorageFileLogSink::unlock() const {
    if (lock_) xSemaphoreGive(lock_);
}

bool StorageFileLogSink::begin() {
#if AC_FILE_LOG_ENABLED
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    if (!lock_) return false;

    if (!queue_) {
        void *memory = Memory::alloc_large(sizeof(Queue), false);
        if (!memory) return false;
        queue_ = new (memory) Queue();
    }

    if (lock()) {
        status_.available = true;
        status_.queue_capacity = AC_FILE_LOG_QUEUE_DEPTH;
        update_queue_status_locked();
        unlock();
    }
    return true;
#else
    return false;
#endif
}

void StorageFileLogSink::set_enabled(bool enabled) {
#if AC_FILE_LOG_ENABLED
    if (!lock_) (void)begin();
    if (!lock()) return;

    desired_enabled_ = enabled;
    if (!enabled && queue_) queue_->clear();
    update_queue_status_locked();
    unlock();
#else
    (void)enabled;
#endif
}

void StorageFileLogSink::set_rotation_allowed(bool allowed) {
#if AC_FILE_LOG_ENABLED
    if (!lock_) (void)begin();
    if (!lock()) return;
    rotation_allowed_ = allowed;
    unlock();
#else
    (void)allowed;
#endif
}

bool StorageFileLogSink::enqueue(const char *line, size_t length) {
#if AC_FILE_LOG_ENABLED
    if (!line || length == 0 || length >= FILE_LOG_LINE_MAX) return false;
    if (!queue_ && !begin()) return false;
    if (!lock(0)) return false;

    bool accepted = false;
    if (desired_enabled_ && queue_) {
        Line item;
        item.length = static_cast<uint16_t>(length);
        memcpy(item.bytes, line, length);
        accepted = queue_->push(item);
    }
    update_queue_status_locked();
    unlock();
    return accepted;
#else
    (void)line;
    (void)length;
    return false;
#endif
}

bool StorageFileLogSink::pop_line(Line &line) {
    if (!lock()) return false;
    const bool available = desired_enabled_ && queue_ && queue_->pop(line);
    update_queue_status_locked();
    unlock();
    return available;
}

void StorageFileLogSink::restore_line(const Line &line) {
    if (!lock()) return;
    if (desired_enabled_ && queue_ && !queue_->push_front(line)) {
        status_.drops++;
    }
    update_queue_status_locked();
    unlock();
}

void StorageFileLogSink::update_queue_status_locked() {
    status_.enabled = desired_enabled_;
    status_.queued = queue_ ? queue_->count() : 0;
    status_.queue_capacity = queue_ ? queue_->capacity() : 0;
    status_.drops = queue_ ? queue_->dropped() : 0;
}

bool StorageFileLogSink::ensure_directory() {
    if (directory_ready_) return true;
    if (!Storage::mounted()) return false;
    if (!Storage::ensure_dir("/aircannect") ||
        !Storage::ensure_dir(AC_FILE_LOG_DIR)) {
        return false;
    }

    directory_ready_ = true;
    return true;
}

void StorageFileLogSink::close_file(bool flush) {
    if (!file_) return;

    Storage::Guard guard;
    if (flush) file_.flush();
    file_.close();
    last_flush_ms_ = 0;

    if (lock()) {
        status_.open = false;
        unlock();
    }
}

bool StorageFileLogSink::rotate_file() {
    close_file(true);
    if (!ensure_directory()) return false;

    char source[AC_STORAGE_WRITE_PATH_MAX + 8] = {};
    char target[AC_STORAGE_WRITE_PATH_MAX + 8] = {};
    if (AC_FILE_LOG_ARCHIVES > 0 &&
        format_archive_path(AC_FILE_LOG_ARCHIVES,
                            target,
                            sizeof(target)) &&
        !Storage::remove(target)) {
        return false;
    }

    for (int index = static_cast<int>(AC_FILE_LOG_ARCHIVES);
         index >= 2;
         --index) {
        if (!format_archive_path(static_cast<uint8_t>(index - 1),
                                 source,
                                 sizeof(source)) ||
            !format_archive_path(static_cast<uint8_t>(index),
                                 target,
                                 sizeof(target))) {
            return false;
        }
        if (!Storage::exists(source)) continue;
        if (!Storage::remove(target) || !Storage::rename(source, target)) {
            return false;
        }
    }

    if (Storage::exists(AC_FILE_LOG_PATH)) {
        if (AC_FILE_LOG_ARCHIVES == 0) {
            if (!Storage::remove(AC_FILE_LOG_PATH)) return false;
        } else {
            if (!format_archive_path(1, target, sizeof(target)) ||
                !Storage::remove(target) ||
                !Storage::rename(AC_FILE_LOG_PATH, target)) {
                return false;
            }
        }
    }

    file_size_ = 0;
    rotation_pending_ = false;
    return true;
}

bool StorageFileLogSink::open_file(size_t next_write_length) {
    if (file_) return true;
    if (!ensure_directory()) return false;

    bool rotation_allowed = true;
    if (lock()) {
        rotation_allowed = rotation_allowed_;
        unlock();
    }

    if (rotation_allowed &&
        (rotation_pending_ ||
         (AC_FILE_LOG_ROTATE_BYTES > 0 &&
          Storage::exists(AC_FILE_LOG_PATH)))) {
        File existing = Storage::open(AC_FILE_LOG_PATH, "r");
        uint64_t existing_size = 0;
        if (existing) {
            Storage::Guard guard;
            existing_size = existing.size();
            existing.close();
        }

        if (rotation_pending_ ||
            existing_size + next_write_length > AC_FILE_LOG_ROTATE_BYTES) {
            if (!rotate_file()) return false;
        } else {
            file_size_ = existing_size;
        }
    }

    file_ = Storage::open(AC_FILE_LOG_PATH, FILE_APPEND);
    if (!file_) return false;
    {
        Storage::Guard guard;
        file_size_ = file_.size();
    }
    last_flush_ms_ = millis();

    if (lock()) {
        status_.open = true;
        status_.bytes = file_size_;
        unlock();
    }
    return true;
}

bool StorageFileLogSink::write_line(const Line &line) {
    bool rotation_allowed = true;
    if (lock()) {
        rotation_allowed = rotation_allowed_;
        unlock();
    }

    if (AC_FILE_LOG_ROTATE_BYTES > 0 &&
        file_size_ + line.length > AC_FILE_LOG_ROTATE_BYTES) {
        if (rotation_allowed) {
            if (!rotate_file()) return false;
        } else {
            rotation_pending_ = true;
        }
    } else if (rotation_allowed && rotation_pending_ &&
               !rotate_file()) {
        return false;
    }

    if (!open_file(line.length)) return false;

    size_t written = 0;
    {
        Storage::Guard guard;
        written = file_.write(
            reinterpret_cast<const uint8_t *>(line.bytes), line.length);
    }
    if (written != line.length) return false;

    file_size_ += written;
    if (lock()) {
        status_.written++;
        status_.bytes = file_size_;
        unlock();
    }
    return true;
}

bool StorageFileLogSink::flush_if_due() {
    if (!file_ ||
        static_cast<int32_t>(millis() - last_flush_ms_) <
            static_cast<int32_t>(AC_FILE_LOG_FLUSH_MS)) {
        return false;
    }

    Storage::Guard guard;
    file_.flush();
    last_flush_ms_ = millis();
    return true;
}

bool StorageFileLogSink::apply_enabled_state() {
    bool enabled = false;
    if (lock()) {
        enabled = desired_enabled_;
        unlock();
    }

    if (enabled) return false;
    if (!file_) return false;
    close_file(true);
    return true;
}

bool StorageFileLogSink::step() {
#if AC_FILE_LOG_ENABLED
    if (apply_enabled_state()) return true;

    bool enabled = false;
    bool rotation_allowed = true;
    if (lock()) {
        enabled = desired_enabled_;
        rotation_allowed = rotation_allowed_;
        unlock();
    }
    if (!enabled) return false;

    if (!Storage::mounted()) {
        close_file(false);
        directory_ready_ = false;
        file_size_ = 0;
        return false;
    }

    Line line;
    if (pop_line(line)) {
        if (write_line(line)) return true;

        restore_line(line);
        if (lock()) {
            status_.errors++;
            unlock();
        }
        return false;
    }

    if (rotation_allowed && rotation_pending_) {
        const bool rotated = rotate_file();
        if (!rotated && lock()) {
            status_.errors++;
            unlock();
        }
        return rotated;
    }
    return flush_if_due();
#else
    return false;
#endif
}

StorageFileLogStatus StorageFileLogSink::status() const {
    StorageFileLogStatus result;
    if (!lock()) return result;
    result = status_;
    unlock();
    return result;
}

}  // namespace aircannect
