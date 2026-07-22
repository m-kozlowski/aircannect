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
    FlowLimitation,
    Count,
};

enum ReportSourcePurpose : uint16_t {
    REPORT_SOURCE_SUMMARY = 1u << 0,
    REPORT_SOURCE_SESSION_BOUNDARIES = 1u << 1,
    REPORT_SOURCE_EVENT_FLAGS = 1u << 2,
    REPORT_SOURCE_TREND_SERIES = 1u << 3,
    REPORT_SOURCE_HIGH_RES_SERIES = 1u << 4,
};

enum ReportSignalFlags : uint16_t {
    REPORT_SIGNAL_REQUIRED = 1u << 0,
};

enum ReportEventSourceFlag : uint8_t {
    REPORT_EVENT_SCORED = 1u << 0,
    REPORT_EVENT_CSR = 1u << 1,
};

static constexpr uint8_t REPORT_EVENT_ALL =
    REPORT_EVENT_SCORED | REPORT_EVENT_CSR;

enum class ReportFallbackSectionKind : uint8_t {
    Series = 1,
    Events = 2,
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
    uint16_t flags;
};

const ReportSourceDef *report_source_defs(size_t &count);
const ReportSignalDef *report_signal_defs(size_t &count);
const ReportSourceDef *report_source_def(ReportSourceId id);
const char *report_source_spool_type(ReportSourceId id);
const char *report_signal_store_name(ReportSignalId id);
uint32_t report_signal_bit(ReportSignalId signal);
uint32_t report_signal_mask_all();
uint32_t report_signal_required_mask();
bool report_signal_required_for_result(const ReportSignalDef &signal);
bool report_source_is_sampled(const ReportSourceDef &source);

}  // namespace aircannect
