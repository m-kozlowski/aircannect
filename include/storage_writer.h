#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

namespace aircannect {

struct StorageWriterStatus {
    bool initialized = false;
    bool available = false;
    bool using_psram = false;
    size_t capacity = 0;
    size_t queued = 0;
    size_t chunk_bytes = 0;
    uint32_t enqueued = 0;
    uint32_t written = 0;
    uint32_t queue_drops = 0;
    uint32_t unavailable_drops = 0;
    uint32_t open_errors = 0;
    uint32_t write_errors = 0;
    uint32_t rotations = 0;
    uint32_t rotate_errors = 0;
    uint64_t bytes_enqueued = 0;
    uint64_t bytes_written = 0;
    uint32_t last_activity_ms = 0;
    char last_path[AC_STORAGE_WRITE_PATH_MAX] = {};
    char last_error[96] = {};
};

namespace StorageWriter {

void begin();
void poll();

bool enqueue_append(const char *path, const uint8_t *data, size_t len);
bool enqueue_rotating_append(const char *path,
                             const uint8_t *data,
                             size_t len,
                             size_t rotate_bytes,
                             uint8_t archive_count,
                             bool report_errors = true);

StorageWriterStatus status();

}  // namespace StorageWriter
}  // namespace aircannect
