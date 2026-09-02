#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AC_REPORT_SUMMARY_SESSION_MAX = 16;

enum class ReportSummaryValueEncoding : uint8_t {
    EnumIndex,
    Hundredths,
    Thousandths,
    Tenths,
    Integer,
};

enum class ReportSummaryField : uint8_t {
#define REPORT_SUMMARY_FIELD(name, encoding) name,
#include "report_summary_fields.inc"
#undef REPORT_SUMMARY_FIELD
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
ReportSummaryValueEncoding report_summary_field_encoding(
    ReportSummaryField field);
bool report_summary_field_physical_value(const ReportSummaryRecord &record,
                                         ReportSummaryField field,
                                         float &out);
bool report_summary_sleep_day_epoch_days(const ReportSummaryRecord &record,
                                         int32_t &out);

}  // namespace aircannect
