#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "crash_diagnostics_types.h"

namespace aircannect {

class LargeByteBuffer;

class CrashDiagnostics {
public:
    bool begin();
    void poll();
    void log_previous_crash() const;

    bool copy_snapshot(CrashDiagnosticsSnapshot &out) const;
    std::shared_ptr<const LargeByteBuffer> copy_dump(
        char *error,
        size_t error_size) const;
    bool clear(char *error, size_t error_size);

private:
    void refresh_dump_locked();

    StaticSemaphore_t mutex_storage_ = {};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    const void *partition_ = nullptr;
    uint32_t next_time_capture_ms_ = 0;
    CrashDiagnosticsSnapshot snapshot_;
};

}  // namespace aircannect
