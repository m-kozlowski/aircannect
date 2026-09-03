#include "published_json_snapshot.h"

#include <ESPAsyncWebServer.h>
#include <string.h>

#include "http_response_utils.h"

namespace aircannect {

bool PublishedJsonSnapshot::begin(size_t capacity) {
    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }

    return mutex_ && json_.reserve(capacity);
}

bool PublishedJsonSnapshot::replace(LargeTextBuffer &next,
                                    bool advance_revision) {
    if (!mutex_ || next.overflowed() ||
        xSemaphoreTake(mutex_, 0) != pdTRUE) {
        return false;
    }

    const bool replaced = replace_locked(next, advance_revision);
    xSemaphoreGive(mutex_);
    return replaced;
}

bool PublishedJsonSnapshot::publish_if_changed(LargeTextBuffer &next,
                                               bool force) {
    if (!mutex_ || next.overflowed() ||
        xSemaphoreTake(mutex_, 0) != pdTRUE) {
        return false;
    }

    const bool changed =
        json_.length() != next.length() ||
        (json_.length() &&
         memcmp(json_.c_str(), next.c_str(), json_.length()) != 0);
    const bool published = !force && !changed
        ? true
        : replace_locked(next, true);
    xSemaphoreGive(mutex_);
    return published;
}

bool PublishedJsonSnapshot::copy(LargeTextBuffer &out,
                                 uint32_t &revision) const {
    if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return false;

    out.clear();
    const bool copied = json_.length() &&
        out.append(json_.c_str(), json_.length());
    if (copied) {
        revision = revision_.load(std::memory_order_relaxed);
    }
    xSemaphoreGive(mutex_);
    return copied;
}

JsonSnapshotResponse PublishedJsonSnapshot::prepare_response(
    AsyncWebServerRequest *request,
    AsyncResponseStream *&response,
    uint32_t timeout_ms) const {
    response = nullptr;
    if (!mutex_ ||
        xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return JsonSnapshotResponse::Busy;
    }

    const bool prepared = http_prepare_json_response(request, json_, response);
    xSemaphoreGive(mutex_);
    return prepared ? JsonSnapshotResponse::Ready
                    : JsonSnapshotResponse::AllocationFailed;
}

bool PublishedJsonSnapshot::replace_locked(LargeTextBuffer &next,
                                           bool advance_revision) {
    json_.swap(next);
    if (!advance_revision) return true;

    uint32_t revision = revision_.load(std::memory_order_relaxed) + 1;
    if (revision == 0) revision = 1;
    revision_.store(revision, std::memory_order_release);
    return true;
}

}  // namespace aircannect
