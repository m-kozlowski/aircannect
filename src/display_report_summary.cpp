#include "display_report_summary.h"

#include <stdio.h>

#include "night_catalog.h"

namespace aircannect {
namespace {

const NightCatalogRecord *latest_record(const NightCatalog &catalog) {
    const NightCatalogRecord *latest = nullptr;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *record = catalog.record(i);
        if (!record || !record->sleep_day.valid()) continue;
        if (!latest || latest->sleep_day < record->sleep_day) {
            latest = record;
        }
    }
    return latest;
}

void format_date(SleepDayId sleep_day, char (&out)[11]) {
    char compact[9] = {};
    if (!sleep_day.format_yyyymmdd(compact, sizeof(compact))) return;

    snprintf(out, sizeof(out), "%.4s-%.2s-%.2s",
             compact, compact + 4, compact + 6);
}

}  // namespace

DisplayReportSummary build_display_report_summary(
    const NightCatalog &catalog) {
    DisplayReportSummary out;
    const NightCatalogRecord *latest = latest_record(catalog);
    if (!latest) return out;

    out.latest.valid = true;
    format_date(latest->sleep_day, out.latest.date);
    out.latest.duration_valid =
        latest->metrics.has(NightCatalogMetric::DurationMinutes) ||
        latest->session_count > 0;
    out.latest.duration_min =
        night_catalog_duration_minutes(catalog, *latest);
    out.latest.ahi_valid = latest->metrics.has(NightCatalogMetric::Ahi);
    out.latest.ahi = latest->metrics.ahi;
    out.latest.rdi_valid = out.latest.ahi_valid &&
        latest->metrics.has(NightCatalogMetric::ArousalIndex);
    out.latest.rdi = latest->metrics.ahi +
        latest->metrics.arousal_index;
    out.latest.pressure_valid =
        latest->metrics.has(NightCatalogMetric::MaskPressure50);
    out.latest.pressure_cm_h2o =
        latest->metrics.mask_pressure_50_cm_h2o;
    out.latest.pressure_95_valid =
        latest->metrics.has(NightCatalogMetric::MaskPressure95);
    out.latest.pressure_95_cm_h2o =
        latest->metrics.mask_pressure_95_cm_h2o;
    out.latest.leak_valid =
        latest->metrics.has(NightCatalogMetric::Leak50);
    out.latest.leak_l_min = latest->metrics.leak_50_l_min;
    out.latest.leak_95_valid =
        latest->metrics.has(NightCatalogMetric::Leak95);
    out.latest.leak_95_l_min = latest->metrics.leak_95_l_min;

    const int32_t first_day = latest->sleep_day.epoch_days() - 29;
    uint32_t duration_nights = 0;
    double weighted_ahi = 0.0;
    double weighted_rdi = 0.0;
    uint32_t weighted_minutes = 0;
    double weighted_pressure = 0.0;
    uint32_t pressure_minutes = 0;
    double weighted_pressure_95 = 0.0;
    uint32_t pressure_95_minutes = 0;
    double weighted_leak = 0.0;
    uint32_t leak_minutes = 0;
    double weighted_leak_95 = 0.0;
    uint32_t leak_95_minutes = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *record = catalog.record(i);
        if (!record || !record->sleep_day.valid() ||
            record->sleep_day.epoch_days() < first_day ||
            latest->sleep_day < record->sleep_day) {
            continue;
        }
        if (record->session_count > 0) ++out.last_30_days.used_nights;
        const bool duration_valid =
            record->metrics.has(NightCatalogMetric::DurationMinutes) ||
            record->session_count > 0;
        if (!duration_valid) {
            continue;
        }

        const uint32_t duration =
            night_catalog_duration_minutes(catalog, *record);
        out.last_30_days.total_duration_min += duration;
        ++duration_nights;
        if (duration > 0 &&
            record->metrics.has(NightCatalogMetric::Ahi) &&
            record->metrics.has(NightCatalogMetric::ArousalIndex)) {
            weighted_ahi +=
                static_cast<double>(record->metrics.ahi) * duration;
            weighted_rdi += static_cast<double>(
                record->metrics.ahi + record->metrics.arousal_index) *
                duration;
            weighted_minutes += duration;
        }
        if (duration > 0 &&
            record->metrics.has(NightCatalogMetric::MaskPressure50)) {
            weighted_pressure += static_cast<double>(
                record->metrics.mask_pressure_50_cm_h2o) * duration;
            pressure_minutes += duration;
        }
        if (duration > 0 &&
            record->metrics.has(NightCatalogMetric::MaskPressure95)) {
            weighted_pressure_95 += static_cast<double>(
                record->metrics.mask_pressure_95_cm_h2o) * duration;
            pressure_95_minutes += duration;
        }
        if (duration > 0 &&
            record->metrics.has(NightCatalogMetric::Leak50)) {
            weighted_leak += static_cast<double>(
                record->metrics.leak_50_l_min) * duration;
            leak_minutes += duration;
        }
        if (duration > 0 &&
            record->metrics.has(NightCatalogMetric::Leak95)) {
            weighted_leak_95 += static_cast<double>(
                record->metrics.leak_95_l_min) * duration;
            leak_95_minutes += duration;
        }
    }

    out.last_30_days.valid = out.last_30_days.used_nights > 0;
    if (duration_nights > 0 &&
        duration_nights == out.last_30_days.used_nights) {
        out.last_30_days.average_duration_valid = true;
        out.last_30_days.average_duration_min =
            (out.last_30_days.total_duration_min +
             out.last_30_days.used_nights / 2) /
            out.last_30_days.used_nights;
    }
    if (weighted_minutes > 0) {
        out.last_30_days.ahi_valid = true;
        out.last_30_days.duration_weighted_ahi =
            static_cast<float>(weighted_ahi / weighted_minutes);
        out.last_30_days.rdi_valid = true;
        out.last_30_days.duration_weighted_rdi =
            static_cast<float>(weighted_rdi / weighted_minutes);
    }
    if (pressure_minutes > 0) {
        out.last_30_days.pressure_valid = true;
        out.last_30_days.duration_weighted_pressure_cm_h2o =
            static_cast<float>(weighted_pressure / pressure_minutes);
    }
    if (pressure_95_minutes > 0) {
        out.last_30_days.pressure_95_valid = true;
        out.last_30_days.duration_weighted_pressure_95_cm_h2o =
            static_cast<float>(weighted_pressure_95 /
                               pressure_95_minutes);
    }
    if (leak_minutes > 0) {
        out.last_30_days.leak_valid = true;
        out.last_30_days.duration_weighted_leak_l_min =
            static_cast<float>(weighted_leak / leak_minutes);
    }
    if (leak_95_minutes > 0) {
        out.last_30_days.leak_95_valid = true;
        out.last_30_days.duration_weighted_leak_95_l_min =
            static_cast<float>(weighted_leak_95 / leak_95_minutes);
    }
    return out;
}

}  // namespace aircannect
