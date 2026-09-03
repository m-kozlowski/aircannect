#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"

class AsyncResponseStream;
class AsyncWebServerRequest;

namespace aircannect {

enum class JsonSnapshotResponse : uint8_t {
    Ready,
    Busy,
    AllocationFailed,
};

class PublishedJsonSnapshot {
public:
    bool begin(size_t capacity);

    uint32_t revision() const {
        return revision_.load(std::memory_order_acquire);
    }

    bool replace(LargeTextBuffer &next, bool advance_revision = true);
    bool publish_if_changed(LargeTextBuffer &next, bool force = false);

    bool copy(LargeTextBuffer &out, uint32_t &revision) const;
    JsonSnapshotResponse prepare_response(
        AsyncWebServerRequest *request,
        AsyncResponseStream *&response,
        uint32_t timeout_ms = 50) const;

private:
    bool replace_locked(LargeTextBuffer &next, bool advance_revision);

    mutable StaticSemaphore_t mutex_storage_ = {};
    mutable SemaphoreHandle_t mutex_ = nullptr;
    LargeTextBuffer json_;
    std::atomic<uint32_t> revision_{0};
};

}  // namespace aircannect
