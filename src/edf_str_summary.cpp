#include "edf_str_summary.h"

#include <stdint.h>
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

int16_t summary_scaled_digital(uint32_t value,
                               uint16_t numerator,
                               uint16_t denominator) {
    if (denominator == 0) return 0;
    const uint64_t scaled =
        (static_cast<uint64_t>(value) * numerator + denominator / 2) /
        denominator;
    return clamp_i16(scaled > INT32_MAX ? INT32_MAX
                                        : static_cast<int32_t>(scaled));
}

bool summary_str_digital(ReportSummaryField field,
                         uint32_t value,
                         uint16_t numerator,
                         uint16_t denominator,
                         int16_t &digital) {
    static constexpr int16_t kTubeConnectedCodes[] = {3, 4, 1, 5, 2};
    static constexpr int16_t kHumidifierConnectedCodes[] = {1, 2, 3};
    if (field == ReportSummaryField::TubeConnected) {
        if (value >= sizeof(kTubeConnectedCodes) /
                         sizeof(kTubeConnectedCodes[0])) {
            return false;
        }
        digital = kTubeConnectedCodes[value];
        return true;
    }
    if (field == ReportSummaryField::HumidifierConnected) {
        if (value >= sizeof(kHumidifierConnectedCodes) /
                         sizeof(kHumidifierConnectedCodes[0])) {
            return false;
        }
        digital = kHumidifierConnectedCodes[value];
        return true;
    }

    digital = summary_scaled_digital(value, numerator, denominator);
    return true;
}

struct SummaryStrMap {
    ReportSummaryField field = ReportSummaryField::Count;
    size_t signal_index = 0;
    // Converts the raw integer stored in ReportSummaryRecord to the STR
    // signal's EDF digital sample value.
    uint16_t numerator = 1;
    uint16_t denominator = 1;
};

static constexpr SummaryStrMap SUMMARY_STR_MAP[] = {
    {ReportSummaryField::TubeConnected, 76, 1, 1},
    {ReportSummaryField::HumidifierConnected, 77, 1, 1},
    {ReportSummaryField::BlowPressure95, 78, 1, 2},
    {ReportSummaryField::BlowPressure5, 79, 1, 2},
    {ReportSummaryField::Flow95, 80, 5, 1},
    {ReportSummaryField::Flow5, 81, 5, 1},
    {ReportSummaryField::BlowerFlow50, 82, 5, 1},
    {ReportSummaryField::AmbientHumidity50, 83, 1, 10},
    {ReportSummaryField::HumidifierTemperature50, 84, 1, 10},
    {ReportSummaryField::HeatedTubeTemperature50, 85, 1, 10},
    {ReportSummaryField::HeatedTubePower50, 86, 1, 10},
    {ReportSummaryField::HumidifierPower50, 87, 1, 10},
    {ReportSummaryField::Spo2Median, 88, 1, 1},
    {ReportSummaryField::Spo2_95, 89, 1, 1},
    {ReportSummaryField::Spo2Max, 90, 1, 1},
    {ReportSummaryField::Spo2ThresholdMinutes, 91, 1, 1},
    {ReportSummaryField::SpontaneousTriggerPercent, 92, 1, 50},
    {ReportSummaryField::SpontaneousCyclePercent, 93, 1, 50},
    {ReportSummaryField::MaskPressureMedian, 94, 1, 2},
    {ReportSummaryField::MaskPressure95, 95, 1, 2},
    {ReportSummaryField::MaskPressureMax, 96, 1, 2},
    {ReportSummaryField::TargetIpapMedian, 97, 1, 2},
    {ReportSummaryField::TargetIpap95, 98, 1, 2},
    {ReportSummaryField::TargetIpapMax, 99, 1, 2},
    {ReportSummaryField::TargetEpapMedian, 100, 1, 2},
    {ReportSummaryField::TargetEpap95, 101, 1, 2},
    {ReportSummaryField::TargetEpapMax, 102, 1, 2},
    {ReportSummaryField::LeakMedian, 103, 1, 2},
    {ReportSummaryField::Leak95, 104, 1, 2},
    {ReportSummaryField::Leak70, 105, 1, 2},
    {ReportSummaryField::LeakMax, 106, 1, 2},
    {ReportSummaryField::MinuteVentMedian, 107, 2, 25},
    {ReportSummaryField::MinuteVent95, 108, 2, 25},
    {ReportSummaryField::MinuteVentMax, 109, 2, 25},
    {ReportSummaryField::RespiratoryRateMedian, 110, 1, 20},
    {ReportSummaryField::RespiratoryRate95, 111, 1, 20},
    {ReportSummaryField::RespiratoryRateMax, 112, 1, 20},
    {ReportSummaryField::TidalVolumeMedian, 113, 1, 2},
    {ReportSummaryField::TidalVolume95, 114, 1, 2},
    {ReportSummaryField::TidalVolumeMax, 115, 1, 2},
    {ReportSummaryField::TargetVentMedian, 116, 2, 25},
    {ReportSummaryField::TargetVent95, 117, 2, 25},
    {ReportSummaryField::TargetVentMax, 118, 2, 25},
    {ReportSummaryField::IeRatioMedian, 119, 1, 100},
    {ReportSummaryField::IeRatio95, 120, 1, 100},
    {ReportSummaryField::IeRatioMax, 121, 1, 100},
    {ReportSummaryField::InspirationTimeMedian, 122, 1, 20},
    {ReportSummaryField::InspirationTime95, 123, 1, 20},
    {ReportSummaryField::InspirationTimeMax, 124, 1, 20},
    {ReportSummaryField::Ahi, 125, 1, 1},
    {ReportSummaryField::HypopneaIndex, 126, 1, 1},
    {ReportSummaryField::ApneaIndex, 127, 1, 1},
    {ReportSummaryField::ObstructiveApneaIndex, 128, 1, 1},
    {ReportSummaryField::CentralApneaIndex, 129, 1, 1},
    {ReportSummaryField::UnknownApneaIndex, 130, 1, 1},
    {ReportSummaryField::ReraIndex, 131, 1, 1},
    {ReportSummaryField::Csr, 132, 1, 1},
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
            summary_str_digital(map.field,
                                value,
                                map.numerator,
                                map.denominator,
                                digital) &&
            session.set_signal_digital(map.signal_index, digital)) {
            result.values++;
        }
    }
    return result.values > 0;
}

}  // namespace aircannect
