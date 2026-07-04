#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_manager_limits.h"
#include "report_night_index.h"

namespace aircannect {

struct ReportNightIndexCacheKey {
    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = 0;
    uint32_t catalog_refresh_id = 0;
};

bool report_night_index_cache_key_equal(
    const ReportNightIndexCacheKey &a,
    const ReportNightIndexCacheKey &b);

class ReportNightIndexCache {
public:
    ~ReportNightIndexCache();

    bool begin();

    bool copy(const ReportNightIndexCacheKey &key,
              ReportIndexedNight *out,
              size_t capacity,
              size_t &count) const;
    bool publish(const ReportNightIndexCacheKey &key,
                 const ReportIndexedNight *src,
                 size_t count);
    void clear();

private:
    bool matches_locked(const ReportNightIndexCacheKey &key) const;
    bool ensure_entries_locked();
    void clear_entries_locked();

    SemaphoreHandle_t lock_ = nullptr;
    ReportIndexedNight *entries_ = nullptr;
    size_t count_ = 0;
    bool valid_ = false;
    ReportNightIndexCacheKey key_;
};

}  // namespace aircannect
