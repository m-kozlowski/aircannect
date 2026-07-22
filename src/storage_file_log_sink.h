#pragma once

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "storage_service.h"

namespace aircannect {

class StorageFileLogSink {
public:
    bool begin();

    void set_enabled(bool enabled);
    void set_rotation_allowed(bool allowed);
    bool enqueue(const char *line, size_t length);

    bool step();
    StorageFileLogStatus status() const;

private:
    struct Line;
    class Queue;

    // Queue and cross-task control
    bool lock(uint32_t timeout_ms = 10) const;
    void unlock() const;
    bool pop_line(Line &line);
    void restore_line(const Line &line);
    void update_queue_status_locked();

    // File lifecycle, used only by the storage task
    bool ensure_directory();
    bool open_file(size_t next_write_length);
    void close_file(bool flush);
    bool rotate_file();
    bool write_line(const Line &line);
    bool flush_if_due();
    bool apply_enabled_state();

    mutable SemaphoreHandle_t lock_ = nullptr;
    Queue *queue_ = nullptr;
    bool desired_enabled_ = false;
    bool rotation_allowed_ = true;
    StorageFileLogStatus status_;

    File file_;
    uint64_t file_size_ = 0;
    uint32_t last_flush_ms_ = 0;
    bool directory_ready_ = false;
    bool rotation_pending_ = false;
};

}  // namespace aircannect
