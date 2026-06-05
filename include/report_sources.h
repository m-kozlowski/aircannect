#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class ReportSourceId : uint8_t {
    Summary,
    UsageEvents,
    RespiratoryEvents,
    TherapyOneMinute,
    RespiratoryFlow6p25Hz,
    MaskPressure6p25Hz,
    InspiratoryPressure0p5Hz,
    Leak0p5Hz,
};

enum class ReportSignalId : uint8_t {
    Flow,
    InspiratoryPressure,
    ExpiratoryPressure,
    Leak,
    MinuteVentilation,
    MaskPressure,
    InspiratoryDuration,
    RespiratoryRate,
    IeRatio,
};

enum ReportSourcePurpose : uint16_t {
    REPORT_SOURCE_SUMMARY = 1u << 0,
    REPORT_SOURCE_SESSION_BOUNDARIES = 1u << 1,
    REPORT_SOURCE_EVENT_FLAGS = 1u << 2,
    REPORT_SOURCE_TREND_SERIES = 1u << 3,
    REPORT_SOURCE_HIGH_RES_SERIES = 1u << 4,
};

struct ReportSourceDef {
    ReportSourceId id;
    const char *spool_type;
    uint32_t parser_schema;
    uint16_t purposes;
};

struct ReportSignalDef {
    ReportSignalId id;
    const char *store_name;
    const char *label;
    ReportSourceId preferred_source;
    ReportSourceId fallback_source;
};

const ReportSourceDef *report_source_defs(size_t &count);
const ReportSignalDef *report_signal_defs(size_t &count);
const ReportSourceDef *report_source_def(ReportSourceId id);
const char *report_source_spool_type(ReportSourceId id);
uint32_t report_source_parser_schema(ReportSourceId id);
const char *report_signal_store_name(ReportSignalId id);

}  // namespace aircannect
