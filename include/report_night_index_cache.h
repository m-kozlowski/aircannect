#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_night_index_snapshot.h"

namespace aircannect {

struct ReportNightIndexCacheKey {
    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = 0;
    uint32_t catalog_refresh_id = 0;
    uint32_t timezone_revision = 0;
};

bool report_night_index_cache_key_equal(
    const ReportNightIndexCacheKey &a,
    const ReportNightIndexCacheKey &b);

enum class ReportNightIndexCacheAcquire : uint8_t {
    Hit,
    Build,
    Busy,
    Error,
};

class ReportNightIndexCache {
public:
    ~ReportNightIndexCache();

    bool begin();

    ReportNightIndexCacheAcquire acquire(
        const ReportNightIndexCacheKey &key,
        ReportNightIndexSnapshotRef &out);
    bool complete_build(const ReportNightIndexCacheKey &key,
                        const ReportNightIndexSnapshotRef &snapshot);
    void cancel_build(const ReportNightIndexCacheKey &key);
    void clear();

private:
    bool matches_locked(const ReportNightIndexCacheKey &key) const;

    SemaphoreHandle_t lock_ = nullptr;
    ReportNightIndexSnapshotRef snapshot_;
    bool valid_ = false;
    ReportNightIndexCacheKey key_;
    bool build_active_ = false;
    ReportNightIndexCacheKey build_key_;
};

}  // namespace aircannect
