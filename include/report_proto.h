#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AC_REPORT_SUMMARY_SESSION_MAX = 16;

enum class ReportSummaryField : uint8_t {
    TubeConnected,
    HumidifierConnected,
    BlowPressure95,
    BlowPressure5,
    Flow95,
    Flow5,
    BlowerFlow50,
    AmbientHumidity50,
    HumidifierTemperature50,
    HeatedTubeTemperature50,
    HeatedTubePower50,
    HumidifierPower50,
    Spo2Median,
    Spo2_95,
    Spo2Max,
    Spo2ThresholdMinutes,
    SpontaneousTriggerPercent,
    SpontaneousCyclePercent,
    MaskPressureMedian,
    MaskPressure95,
    MaskPressureMax,
    TargetIpapMedian,
    TargetIpap95,
    TargetIpapMax,
    TargetEpapMedian,
    TargetEpap95,
    TargetEpapMax,
    LeakMedian,
    Leak95,
    Leak70,
    LeakMax,
    MinuteVentMedian,
    MinuteVent95,
    MinuteVentMax,
    RespiratoryRateMedian,
    RespiratoryRate95,
    RespiratoryRateMax,
    TidalVolumeMedian,
    TidalVolume95,
    TidalVolumeMax,
    TargetVentMedian,
    TargetVent95,
    TargetVentMax,
    IeRatioMedian,
    IeRatio95,
    IeRatioMax,
    InspirationTimeMedian,
    InspirationTime95,
    InspirationTimeMax,
    Ahi,
    HypopneaIndex,
    ApneaIndex,
    ObstructiveApneaIndex,
    CentralApneaIndex,
    UnknownApneaIndex,
    ReraIndex,
    Csr,
    Count,
};

static constexpr size_t AC_REPORT_SUMMARY_FIELD_COUNT =
    static_cast<size_t>(ReportSummaryField::Count);
static_assert(AC_REPORT_SUMMARY_FIELD_COUNT <= 64,
              "summary field mask is 64 bits");

struct ReportSummarySession {
    uint64_t start_ms = 0;
    uint32_t duration_min = 0;
};

struct ReportSummaryRecord {
    bool valid = false;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    uint32_t duration_min = 0;

    bool has_tz_offset_min = false;
    int32_t tz_offset_min = 0;

    bool has_ahi = false;
    float ahi = 0.0f;
    bool has_apnea_index = false;
    float apnea_index = 0.0f;
    bool has_hypopnea_index = false;
    float hypopnea_index = 0.0f;
    bool has_oa_index = false;
    float oa_index = 0.0f;
    bool has_ca_index = false;
    float ca_index = 0.0f;
    bool has_ua_index = false;
    float ua_index = 0.0f;
    bool has_rera_index = false;
    float rera_index = 0.0f;

    bool has_session_count = false;
    uint32_t session_count = 0;
    uint32_t session_interval_count = 0;
    ReportSummarySession sessions[AC_REPORT_SUMMARY_SESSION_MAX] = {};

    uint64_t summary_field_mask = 0;
    uint32_t summary_field_values[AC_REPORT_SUMMARY_FIELD_COUNT] = {};
};

using ReportSummaryRecordCallback =
    bool (*)(void *context, const ReportSummaryRecord &record);

struct ReportProtoField {
    uint32_t field = 0;
    uint8_t wire = 0;
    uint64_t value = 0;
    const uint8_t *data = nullptr;
    size_t len = 0;
};

bool report_proto_read_varint(const uint8_t *data,
                              size_t len,
                              size_t &index,
                              uint64_t &out);
bool report_proto_next(const uint8_t *data,
                       size_t len,
                       size_t &index,
                       ReportProtoField &out);
bool report_proto_all_length_fields(const uint8_t *data,
                                    size_t len,
                                    uint32_t field_id);

bool report_parse_summary_records(const uint8_t *data,
                                  size_t len,
                                  ReportSummaryRecordCallback callback,
                                  void *context,
                                  char *error,
                                  size_t error_len);
bool report_summary_field_value(const ReportSummaryRecord &record,
                                ReportSummaryField field,
                                uint32_t &out);

}  // namespace aircannect
