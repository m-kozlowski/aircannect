#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace aircannect {

class ReportResultSlotCache;

enum class ReportCacheLoadRequest : uint8_t {
    Queued,
    Pending,
    Missing,
    Failed,
    Full,
    Unavailable,
};

class ReportResultCacheLoader {
public:
    explicit ReportResultCacheLoader(ReportResultSlotCache &slots)
        : slots_(slots) {}
    ~ReportResultCacheLoader();

    bool begin();
    ReportCacheLoadRequest request(uint64_t night_start_ms, const char *etag);
    bool active() const;
    bool service();
    void invalidate(uint64_t night_start_ms, bool all);

private:
    struct State;

    bool ensure_lock();
    void finish_current();

    ReportResultSlotCache &slots_;
    State *state_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
};

}  // namespace aircannect
