#include "as11_stream_signals.h"

#include <stddef.h>
#include <string.h>

#include "board.h"
#include "data_id_csv.h"

namespace aircannect {
namespace {

struct SignalAlias {
    const char *name;
    StreamSignalId id;
};

struct AirMiniSelector {
    const char *canonical_name;
    const char *wire_name;
    StreamSignalId id;
};

const AirMiniSelector AIRMINI_SELECTORS[] = {
    {"_RFL", "PatientFlow-100hz", StreamSignalId::PatientFlow},
    {"_MKP", "MaskPressure-100hz", StreamSignalId::MaskPressure},
    {"_MKF", "MaskPressure-TwoSecond",
     StreamSignalId::MaskPressureTwoSecond},
    {"_MKI", "InspiratoryPressure-50hz",
     StreamSignalId::InspiratoryPressureTwoSecond},
    {"_MKE", "ExpiratoryPressure-50hz",
     StreamSignalId::ExpiratoryPressureTwoSecond},
    {"_LKF", "Leak-TwoSecond", StreamSignalId::Leak},
    {"_SNI", "SnoreIndex-Breath", StreamSignalId::SnoreIndex},
    {"_FFL", "FlowLimitation-Breath", StreamSignalId::FlowLimitation},
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
    {"_BYV", StreamSignalId::TriggerCycleEvent},
};

}  // namespace

StreamSignalId as11_stream_signal_id_from_name(const char *name,
                                              ResmedDeviceModel model) {
    if (!name) return StreamSignalId::Unknown;

    if (model == ResmedDeviceModel::AirMini) {
        for (const AirMiniSelector &selector : AIRMINI_SELECTORS) {
            if (strcmp(name, selector.wire_name) == 0) return selector.id;
        }
        return StreamSignalId::Unknown;
    }

    for (size_t i = 0; i < sizeof(SIGNAL_ALIASES) /
                               sizeof(SIGNAL_ALIASES[0]);
         ++i) {
        if (strcmp(name, SIGNAL_ALIASES[i].name) == 0) {
            return SIGNAL_ALIASES[i].id;
        }
    }
    return StreamSignalId::Unknown;
}

uint32_t as11_stream_signal_sample_interval_ms(
    const char *name,
    uint32_t fallback_interval_ms,
    ResmedDeviceModel model) {
    if (!name) return fallback_interval_ms;
    if (model == ResmedDeviceModel::AirMini) return fallback_interval_ms;
    if (strstr(name, "-100hz")) return 10;
    if (strstr(name, "-50hz")) return 20;
    return fallback_interval_ms;
}

const char *as11_stream_signal_wire_name(const char *canonical_name,
                                         ResmedDeviceModel model) {
    if (!canonical_name) return nullptr;
    if (model != ResmedDeviceModel::AirMini) return canonical_name;

    for (const AirMiniSelector &selector : AIRMINI_SELECTORS) {
        if (strcmp(canonical_name, selector.canonical_name) == 0) {
            return selector.wire_name;
        }
    }
    return nullptr;
}

const char *as11_stream_signal_canonical_name(const char *wire_name,
                                             ResmedDeviceModel model) {
    if (!wire_name) return nullptr;
    if (model != ResmedDeviceModel::AirMini) return wire_name;

    for (const AirMiniSelector &selector : AIRMINI_SELECTORS) {
        if (strcmp(wire_name, selector.wire_name) == 0) {
            return selector.canonical_name;
        }
    }
    return nullptr;
}

bool as11_stream_signal_wire_ids(const std::string &canonical_ids_csv,
                                 ResmedDeviceModel model,
                                 std::string &wire_ids_csv) {
    wire_ids_csv.clear();
    const DataIdCsvLimits limits = {
        AC_STREAM_FRAME_SIGNAL_MAX,
        AC_STREAM_FRAME_SIGNAL_NAME_MAX - 1,
        AC_STREAM_FRAME_SIGNAL_MAX * AC_STREAM_FRAME_SIGNAL_NAME_MAX - 1,
    };
    size_t wire_count = 0;

    if (model != ResmedDeviceModel::AirMini) {
        return data_id_csv_merge(wire_ids_csv, wire_count,
                                 canonical_ids_csv.c_str(), limits);
    }

    for (const AirMiniSelector &selector : AIRMINI_SELECTORS) {
        if (!data_id_csv_contains(canonical_ids_csv,
                                  selector.canonical_name,
                                  strlen(selector.canonical_name))) {
            continue;
        }
        if (!data_id_csv_add(wire_ids_csv, wire_count, selector.wire_name,
                             strlen(selector.wire_name), limits)) {
            return false;
        }
    }
    return true;
}

}  // namespace aircannect
