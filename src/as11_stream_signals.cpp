#include "as11_stream_signals.h"

#include <stddef.h>
#include <string.h>

namespace aircannect {
namespace {

struct SignalAlias {
    const char *name;
    StreamSignalId id;
};

const SignalAlias SIGNAL_ALIASES[] = {
    {"PatientFlow", StreamSignalId::PatientFlow},
    {"PatientFlow-100hz", StreamSignalId::PatientFlow},
    {"MaskPressure", StreamSignalId::MaskPressure},
    {"MaskPressure-100hz", StreamSignalId::MaskPressure},
    {"MaskPressure-TwoSecond", StreamSignalId::MaskPressureTwoSecond},
    {"InspiratoryPressure-50hz", StreamSignalId::InspiratoryPressure},
    {"ExpiratoryPressure-50hz", StreamSignalId::ExpiratoryPressure},
    {"InspiratoryPressure-TwoSecond",
     StreamSignalId::InspiratoryPressureTwoSecond},
    {"ExpiratoryPressure-TwoSecond",
     StreamSignalId::ExpiratoryPressureTwoSecond},
    {"Leak", StreamSignalId::Leak},
    {"Leak-50hz", StreamSignalId::Leak},
    {"RespiratoryRate", StreamSignalId::RespiratoryRate},
    {"RespiratoryRate-50hz", StreamSignalId::RespiratoryRate},
    {"_RR2", StreamSignalId::RespiratoryRate},
    {"TidalVolume", StreamSignalId::TidalVolume},
    {"TidalVolume-50hz", StreamSignalId::TidalVolume},
    {"_TD2", StreamSignalId::TidalVolume},
    {"MinuteVentilation", StreamSignalId::MinuteVentilation},
    {"MinuteVentilation-50hz", StreamSignalId::MinuteVentilation},
    {"_MV2", StreamSignalId::MinuteVentilation},
    {"TargetMinuteVentilation", StreamSignalId::TargetMinuteVentilation},
    {"_TGT", StreamSignalId::TargetMinuteVentilation},
    {"IeRatio", StreamSignalId::IeRatio},
    {"_IE2", StreamSignalId::IeRatio},
    {"SnoreIndex", StreamSignalId::SnoreIndex},
    {"SnoreIndex-50hz", StreamSignalId::SnoreIndex},
    {"FlowLimitation", StreamSignalId::FlowLimitation},
    {"FlowLimitation-50hz", StreamSignalId::FlowLimitation},
    {"InspiratoryDuration", StreamSignalId::InspiratoryDuration},
    {"HeartRate", StreamSignalId::HeartRate},
    {"SpO2", StreamSignalId::SpO2},
};

}  // namespace

StreamSignalId as11_stream_signal_id_from_name(const char *name) {
    if (!name) return StreamSignalId::Unknown;
    for (size_t i = 0; i < sizeof(SIGNAL_ALIASES) /
                               sizeof(SIGNAL_ALIASES[0]);
         ++i) {
        if (strcmp(name, SIGNAL_ALIASES[i].name) == 0) {
            return SIGNAL_ALIASES[i].id;
        }
    }
    return StreamSignalId::Unknown;
}

const char *as11_stream_signal_id_name(StreamSignalId id) {
    switch (id) {
        case StreamSignalId::PatientFlow: return "PatientFlow";
        case StreamSignalId::MaskPressure: return "MaskPressure";
        case StreamSignalId::MaskPressureTwoSecond:
            return "MaskPressure-TwoSecond";
        case StreamSignalId::InspiratoryPressure:
            return "InspiratoryPressure";
        case StreamSignalId::ExpiratoryPressure:
            return "ExpiratoryPressure";
        case StreamSignalId::InspiratoryPressureTwoSecond:
            return "InspiratoryPressure-TwoSecond";
        case StreamSignalId::ExpiratoryPressureTwoSecond:
            return "ExpiratoryPressure-TwoSecond";
        case StreamSignalId::Leak: return "Leak";
        case StreamSignalId::RespiratoryRate:
            return "RespiratoryRate";
        case StreamSignalId::TidalVolume: return "TidalVolume";
        case StreamSignalId::MinuteVentilation:
            return "MinuteVentilation";
        case StreamSignalId::TargetMinuteVentilation:
            return "TargetMinuteVentilation";
        case StreamSignalId::IeRatio: return "IeRatio";
        case StreamSignalId::SnoreIndex: return "SnoreIndex";
        case StreamSignalId::FlowLimitation:
            return "FlowLimitation";
        case StreamSignalId::InspiratoryDuration:
            return "InspiratoryDuration";
        case StreamSignalId::HeartRate: return "HeartRate";
        case StreamSignalId::SpO2: return "SpO2";
        case StreamSignalId::Unknown:
        default: return "Unknown";
    }
}

uint32_t as11_stream_signal_sample_interval_ms(
    const char *name,
    uint32_t fallback_interval_ms) {
    if (!name) return fallback_interval_ms;
    if (strstr(name, "-100hz")) return 10;
    if (strstr(name, "-50hz")) return 20;
    return fallback_interval_ms;
}

}  // namespace aircannect
