#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class ReportSourceId : uint8_t {
    Summary = 0,
    UsageEvents = 1,
    RespiratoryEvents = 2,
    TherapyOneMinute = 3,
    RespiratoryFlow6p25Hz = 4,
    MaskPressure6p25Hz = 5,
    InspiratoryPressure0p5Hz = 6,
    Leak0p5Hz = 7,
    OximetryOneSecond = 8,
};

enum class ReportSignalId : uint8_t {
    Flow = 0,
    InspiratoryPressure = 1,
    ExpiratoryPressure = 2,
    Leak = 3,
    MinuteVentilation = 4,
    MaskPressure = 5,
    InspiratoryDuration = 6,
    RespiratoryRate = 7,
    IeRatio = 8,
    FlowLimitation = 9,
    Invalid = 10,
    Snore = 11,
    TidalVolume = 12,
    SpO2 = 13,
    Pulse = 14,
    Count = 15,
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
    REPORT_SIGNAL_NO_FALLBACK = 1u << 1,
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
    Unavailable = 3,
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

struct ReportSeriesDescriptor {
    ReportSignalId signal = ReportSignalId::Flow;
    ReportSourceId source = ReportSourceId::Summary;
    uint32_t sample_interval_ms = 0;
    bool primary = false;
};

const ReportSignalDef *report_signal_defs(size_t &count);
const ReportSourceDef *report_source_def(ReportSourceId id);
const ReportSignalDef *report_signal_def(ReportSignalId id);
const char *report_signal_store_name(ReportSignalId id);
uint32_t report_signal_bit(ReportSignalId signal);
uint32_t report_signal_mask_all();
uint32_t report_signal_required_mask();
bool report_source_is_sampled(const ReportSourceDef &source);

}  // namespace aircannect
