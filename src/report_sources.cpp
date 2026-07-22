#include "report_sources.h"

namespace aircannect {
namespace {

static constexpr uint32_t SCHEMA_SUMMARY_V1 = 1;
static constexpr uint32_t SCHEMA_USAGE_EVENTS_V1 = 1;
static constexpr uint32_t SCHEMA_RESPIRATORY_EVENTS_V1 = 1;
static constexpr uint32_t SCHEMA_THERAPY_ONE_MINUTE_V1 = 1;
static constexpr uint32_t SCHEMA_RC03_SIGNAL_V1 = 1;

const ReportSourceDef SOURCES[] = {
    {ReportSourceId::Summary,
     "Summary",
     SCHEMA_SUMMARY_V1,
     REPORT_SOURCE_SUMMARY},
    {ReportSourceId::UsageEvents,
     "UsageEvents-TherapyStatusEvents",
     SCHEMA_USAGE_EVENTS_V1,
     REPORT_SOURCE_SESSION_BOUNDARIES},
    {ReportSourceId::RespiratoryEvents,
     "TherapyEvents-RespiratoryEvents",
     SCHEMA_RESPIRATORY_EVENTS_V1,
     REPORT_SOURCE_EVENT_FLAGS},
    {ReportSourceId::TherapyOneMinute,
     "TherapyOneMinutePeriodic",
     SCHEMA_THERAPY_ONE_MINUTE_V1,
     REPORT_SOURCE_TREND_SERIES},
    {ReportSourceId::RespiratoryFlow6p25Hz,
     "RespiratoryFlow6p25Hz",
     SCHEMA_RC03_SIGNAL_V1,
     REPORT_SOURCE_HIGH_RES_SERIES},
    {ReportSourceId::MaskPressure6p25Hz,
     "MaskPressure6p25Hz",
     SCHEMA_RC03_SIGNAL_V1,
     REPORT_SOURCE_HIGH_RES_SERIES},
    {ReportSourceId::InspiratoryPressure0p5Hz,
     "InspiratoryPressure0p5Hz",
     SCHEMA_RC03_SIGNAL_V1,
     REPORT_SOURCE_HIGH_RES_SERIES},
    {ReportSourceId::Leak0p5Hz,
     "Leak0p5Hz",
     SCHEMA_RC03_SIGNAL_V1,
     REPORT_SOURCE_HIGH_RES_SERIES},
};

const ReportSignalDef SIGNALS[] = {
    {ReportSignalId::Flow,
     "flow",
     "Flow",
     ReportSourceId::RespiratoryFlow6p25Hz,
     ReportSourceId::RespiratoryFlow6p25Hz,
     REPORT_SIGNAL_REQUIRED},
    {ReportSignalId::InspiratoryPressure,
     "inspiratory_pressure",
     "Inspiratory Pressure",
     ReportSourceId::InspiratoryPressure0p5Hz,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::ExpiratoryPressure,
     "expiratory_pressure",
     "Expiratory Pressure",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::Leak,
     "leak",
     "Leak",
     ReportSourceId::Leak0p5Hz,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::FlowLimitation,
     "flow_limitation",
     "Flow Limit",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::MinuteVentilation,
     "minute_ventilation",
     "Minute Vent",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::MaskPressure,
     "mask_pressure",
     "Mask Pressure",
     ReportSourceId::MaskPressure6p25Hz,
     ReportSourceId::MaskPressure6p25Hz,
     REPORT_SIGNAL_REQUIRED},
    {ReportSignalId::InspiratoryDuration,
     "inspiratory_duration",
     "Insp. Duration",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::RespiratoryRate,
     "respiratory_rate",
     "Resp. Rate",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
    {ReportSignalId::IeRatio,
     "ie_ratio",
     "I:E",
     ReportSourceId::TherapyOneMinute,
     ReportSourceId::TherapyOneMinute,
     0},
};

}  // namespace

const ReportSourceDef *report_source_defs(size_t &count) {
    count = sizeof(SOURCES) / sizeof(SOURCES[0]);
    return SOURCES;
}

const ReportSignalDef *report_signal_defs(size_t &count) {
    count = sizeof(SIGNALS) / sizeof(SIGNALS[0]);
    return SIGNALS;
}

const ReportSourceDef *report_source_def(ReportSourceId id) {
    size_t count = 0;
    const ReportSourceDef *sources = report_source_defs(count);
    for (size_t i = 0; i < count; ++i) {
        if (sources[i].id == id) return &sources[i];
    }
    return nullptr;
}

const char *report_source_spool_type(ReportSourceId id) {
    const ReportSourceDef *def = report_source_def(id);
    return def ? def->spool_type : "";
}

const char *report_signal_store_name(ReportSignalId id) {
    size_t count = 0;
    const ReportSignalDef *signals = report_signal_defs(count);
    for (size_t i = 0; i < count; ++i) {
        if (signals[i].id == id) return signals[i].store_name;
    }
    return "";
}

uint32_t report_signal_bit(ReportSignalId signal) {
    const uint8_t index = static_cast<uint8_t>(signal);
    if (index >= static_cast<uint8_t>(ReportSignalId::Count) || index >= 32) {
        return 0;
    }
    return 1u << index;
}

uint32_t report_signal_mask_all() {
    const uint8_t count = static_cast<uint8_t>(ReportSignalId::Count);
    return count >= 32 ? UINT32_MAX : (1u << count) - 1u;
}

bool report_signal_required_for_result(const ReportSignalDef &signal) {
    return (signal.flags & REPORT_SIGNAL_REQUIRED) != 0;
}

bool report_source_is_sampled(const ReportSourceDef &source) {
    return (source.purposes &
            (REPORT_SOURCE_TREND_SERIES | REPORT_SOURCE_HIGH_RES_SERIES)) != 0;
}

}  // namespace aircannect
