#pragma once

#include <stddef.h>
#include <stdint.h>

#include "night_catalog_builder.h"

namespace aircannect {

struct NightCatalogClockContext {
    const NightCatalogSummaryInput *summary_records = nullptr;
    size_t summary_record_count = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
};

const NightCatalogSummaryInput *night_catalog_find_summary(
    const NightCatalogClockContext &context,
    SleepDayId sleep_day);

bool night_catalog_resolve_local_time(
    const NightCatalogClockContext &context,
    SleepDayId sleep_day,
    int64_t local_ms,
    int64_t &utc_ms);

bool night_catalog_resolve_local_minute(void *context,
                                        SleepDayId sleep_day,
                                        uint16_t minute_from_noon,
                                        int64_t &utc_ms);

}  // namespace aircannect
