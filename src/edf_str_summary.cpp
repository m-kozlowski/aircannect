#include "edf_str_summary.h"

#include <math.h>
#include <time.h>

#include "edf_storage_catalog.h"
#include "report_proto.h"

namespace aircannect {
namespace {

int16_t clamp_i16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return static_cast<int16_t>(value);
}

int16_t summary_scaled_digital(uint32_t value, float multiplier) {
    const float scaled = static_cast<float>(value) * multiplier;
    return clamp_i16(static_cast<int32_t>(lroundf(scaled)));
}

bool summary_str_digital(ReportSummaryField field,
                         uint32_t value,
                         float multiplier,
                         int16_t &digital) {
    static constexpr int16_t kTubeConnectedCodes[] = {3, 4, 1, 5, 2};
    if (field == ReportSummaryField::TubeConnected) {
        if (value >= sizeof(kTubeConnectedCodes) /
                         sizeof(kTubeConnectedCodes[0])) {
            return false;
        }
        digital = kTubeConnectedCodes[value];
        return true;
    }

    digital = summary_scaled_digital(value, multiplier);
    return true;
}

struct SummaryStrMap {
    ReportSummaryField field = ReportSummaryField::Count;
    size_t signal_index = 0;
    float multiplier = 1.0f;
};

static constexpr SummaryStrMap SUMMARY_STR_MAP[] = {
    {ReportSummaryField::TubeConnected, 76, 1.0f},
    {ReportSummaryField::HumidifierConnected, 77, 1.0f},
    {ReportSummaryField::BlowPressure95, 78, 2.0f},
    {ReportSummaryField::BlowPressure5, 79, 2.0f},
    {ReportSummaryField::Flow95, 80, 0.2f},
    {ReportSummaryField::Flow5, 81, 0.2f},
    {ReportSummaryField::BlowerFlow50, 82, 0.2f},
    {ReportSummaryField::AmbientHumidity50, 83, 10.0f},
    {ReportSummaryField::HumidifierTemperature50, 84, 10.0f},
    {ReportSummaryField::HeatedTubeTemperature50, 85, 10.0f},
    {ReportSummaryField::HeatedTubePower50, 86, 10.0f},
    {ReportSummaryField::HumidifierPower50, 87, 10.0f},
    {ReportSummaryField::Spo2Median, 88, 1.0f},
    {ReportSummaryField::Spo2_95, 89, 1.0f},
    {ReportSummaryField::Spo2Max, 90, 1.0f},
    {ReportSummaryField::Spo2ThresholdMinutes, 91, 1.0f},
    {ReportSummaryField::SpontaneousTriggerPercent, 92, 1.0f},
    {ReportSummaryField::SpontaneousCyclePercent, 93, 1.0f},
    {ReportSummaryField::MaskPressureMedian, 94, 2.0f},
    {ReportSummaryField::MaskPressure95, 95, 2.0f},
    {ReportSummaryField::MaskPressureMax, 96, 2.0f},
    {ReportSummaryField::TargetIpapMedian, 97, 2.0f},
    {ReportSummaryField::TargetIpap95, 98, 2.0f},
    {ReportSummaryField::TargetIpapMax, 99, 2.0f},
    {ReportSummaryField::TargetEpapMedian, 100, 2.0f},
    {ReportSummaryField::TargetEpap95, 101, 2.0f},
    {ReportSummaryField::TargetEpapMax, 102, 2.0f},
    {ReportSummaryField::LeakMedian, 103, 2.0f},
    {ReportSummaryField::Leak95, 104, 2.0f},
    {ReportSummaryField::Leak70, 105, 2.0f},
    {ReportSummaryField::LeakMax, 106, 2.0f},
    {ReportSummaryField::MinuteVentMedian, 107, 8.0f},
    {ReportSummaryField::MinuteVent95, 108, 8.0f},
    {ReportSummaryField::MinuteVentMax, 109, 8.0f},
    {ReportSummaryField::RespiratoryRateMedian, 110, 5.0f},
    {ReportSummaryField::RespiratoryRate95, 111, 5.0f},
    {ReportSummaryField::RespiratoryRateMax, 112, 5.0f},
    {ReportSummaryField::TidalVolumeMedian, 113, 2.0f},
    {ReportSummaryField::TidalVolume95, 114, 2.0f},
    {ReportSummaryField::TidalVolumeMax, 115, 2.0f},
    {ReportSummaryField::TargetVentMedian, 116, 1.0f},
    {ReportSummaryField::TargetVent95, 117, 1.0f},
    {ReportSummaryField::TargetVentMax, 118, 1.0f},
    {ReportSummaryField::IeRatioMedian, 119, 1.0f},
    {ReportSummaryField::IeRatio95, 120, 1.0f},
    {ReportSummaryField::IeRatioMax, 121, 1.0f},
    {ReportSummaryField::InspirationTimeMedian, 122, 1.0f},
    {ReportSummaryField::InspirationTime95, 123, 1.0f},
    {ReportSummaryField::InspirationTimeMax, 124, 1.0f},
    {ReportSummaryField::Ahi, 125, 1.0f},
    {ReportSummaryField::HypopneaIndex, 126, 1.0f},
    {ReportSummaryField::ApneaIndex, 127, 1.0f},
    {ReportSummaryField::ObstructiveApneaIndex, 128, 1.0f},
    {ReportSummaryField::CentralApneaIndex, 129, 1.0f},
    {ReportSummaryField::UnknownApneaIndex, 130, 1.0f},
    {ReportSummaryField::ReraIndex, 131, 1.0f},
    {ReportSummaryField::Csr, 132, 1.0f},
};
static_assert(sizeof(SUMMARY_STR_MAP) / sizeof(SUMMARY_STR_MAP[0]) ==
                  AC_REPORT_SUMMARY_FIELD_COUNT,
              "every report summary field must have an STR mapping");

}  // namespace

bool edf_str_summary_sleep_day(const ReportSummaryRecord &record,
                               uint16_t &day) {
    day = 0;
    if (!record.start_ms || !record.has_tz_offset_min) return false;
    const int64_t local_ms =
        static_cast<int64_t>(record.start_ms) +
        static_cast<int64_t>(record.tz_offset_min) * 60LL * 1000LL;
    if (local_ms < 0) return false;

    const time_t seconds = static_cast<time_t>(local_ms / 1000LL);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;

    EdfLocalDateTime local;
    local.year = tmv.tm_year + 1900;
    local.month = tmv.tm_mon + 1;
    local.day = tmv.tm_mday;
    local.hour = tmv.tm_hour;
    local.minute = tmv.tm_min;
    local.second = tmv.tm_sec;
    return edf_sleep_day_epoch_days(local, day);
}

bool edf_str_apply_summary_record(const ReportSummaryRecord &record,
                                  EdfStrSessionAccumulator &session,
                                  EdfStrSummaryApplyResult &result) {
    result = {};
    if (!session.active()) return false;
    if (!edf_str_summary_sleep_day(record, result.day) ||
        result.day != session.day_epoch_days()) {
        return false;
    }

    for (const SummaryStrMap &map : SUMMARY_STR_MAP) {
        uint32_t value = 0;
        int16_t digital = 0;
        if (report_summary_field_value(record, map.field, value) &&
            summary_str_digital(map.field, value, map.multiplier, digital) &&
            session.set_signal_digital(map.signal_index, digital)) {
            result.values++;
        }
    }
    return result.values > 0;
}

}  // namespace aircannect
