#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_daily_metrics.h"
#include "report_daily_statistics.h"
#include "sleep_day_id.h"
#include "report_therapy_settings.h"
#include "resmed_device_model.h"

namespace aircannect {

static constexpr size_t AC_NIGHT_STR_MASK_WINDOW_MAX = 20;

struct NightStrMaskWindow {
    uint16_t on_minute = 0;
    uint16_t off_minute = 0;
};

struct NightStrRecord {
    SleepDayId sleep_day;
    ReportDailyMetrics metrics;
    ReportTherapySettings settings;
    ReportDailyStatistics statistics;
    uint64_t source_identity = 0;
    uint8_t mask_window_count = 0;
    NightStrMaskWindow mask_windows[AC_NIGHT_STR_MASK_WINDOW_MAX] = {};
};

bool night_str_record_parse(const uint8_t *record,
                            size_t len,
                            NightStrRecord &out,
                            ResmedDeviceModel model);

}  // namespace aircannect
