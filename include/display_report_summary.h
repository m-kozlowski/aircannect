#pragma once

#include <stdint.h>

namespace aircannect {

class NightCatalog;

struct DisplayNightSummary {
    bool valid = false;
    char date[11] = {};

    bool duration_valid = false;
    uint32_t duration_min = 0;
    bool ahi_valid = false;
    float ahi = 0.0f;
    bool rdi_valid = false;
    float rdi = 0.0f;
    bool pressure_valid = false;
    float pressure_cm_h2o = 0.0f;
    bool pressure_95_valid = false;
    float pressure_95_cm_h2o = 0.0f;
    bool leak_valid = false;
    float leak_l_min = 0.0f;
    bool leak_95_valid = false;
    float leak_95_l_min = 0.0f;
};

struct DisplayPeriodSummary {
    bool valid = false;
    uint16_t used_nights = 0;
    uint32_t total_duration_min = 0;
    bool average_duration_valid = false;
    uint32_t average_duration_min = 0;
    bool ahi_valid = false;
    float duration_weighted_ahi = 0.0f;
    bool rdi_valid = false;
    float duration_weighted_rdi = 0.0f;
    bool pressure_valid = false;
    float duration_weighted_pressure_cm_h2o = 0.0f;
    bool pressure_95_valid = false;
    float duration_weighted_pressure_95_cm_h2o = 0.0f;
    bool leak_valid = false;
    float duration_weighted_leak_l_min = 0.0f;
    bool leak_95_valid = false;
    float duration_weighted_leak_95_l_min = 0.0f;
};

struct DisplayReportSummary {
    DisplayNightSummary latest;
    DisplayPeriodSummary last_30_days;
};

DisplayReportSummary build_display_report_summary(
    const NightCatalog &catalog);

}  // namespace aircannect
