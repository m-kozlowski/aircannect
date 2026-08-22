#include "edf_stream_signal_table.h"

namespace aircannect {
namespace {

const EdfStreamSignalDescriptor EDF_STREAM_SIGNALS[] = {
    {"_RFL", StreamSignalId::PatientFlow, EdfSeriesId::Brp, 0},
    {"_MKP", StreamSignalId::MaskPressure, EdfSeriesId::Brp, 1},
    {"_MKF", StreamSignalId::MaskPressureTwoSecond, EdfSeriesId::Pld, 0},
    {"_MKI", StreamSignalId::InspiratoryPressureTwoSecond,
     EdfSeriesId::Pld, 1},
    {"_MKE", StreamSignalId::ExpiratoryPressureTwoSecond,
     EdfSeriesId::Pld, 2},
    {"_LKF", StreamSignalId::Leak, EdfSeriesId::Pld, 3},
    {"_RR2", StreamSignalId::RespiratoryRate, EdfSeriesId::Pld, 4},
    {"_TD2", StreamSignalId::TidalVolume, EdfSeriesId::Pld, 5},
    {"_MV2", StreamSignalId::MinuteVentilation, EdfSeriesId::Pld, 6},
    {"_TGT", StreamSignalId::TargetMinuteVentilation,
     EdfSeriesId::Pld, 7},
    {"_IE2", StreamSignalId::IeRatio, EdfSeriesId::Pld, 8},
    {"_SNI", StreamSignalId::SnoreIndex, EdfSeriesId::Pld, 9},
    {"_FFL", StreamSignalId::FlowLimitation, EdfSeriesId::Pld, 10},
    {"_INT", StreamSignalId::InspiratoryDuration, EdfSeriesId::Pld, 11},
    {"_HRT", StreamSignalId::HeartRate, EdfSeriesId::Sa2, 0},
    {"_SAO", StreamSignalId::SpO2, EdfSeriesId::Sa2, 1},
    {"_BYV", StreamSignalId::TriggerCycleEvent, EdfSeriesId::Tcv, 0},
};

}  // namespace

const EdfStreamSignalDescriptor *edf_stream_signal_descriptors(
    size_t &count) {
    count = sizeof(EDF_STREAM_SIGNALS) / sizeof(EDF_STREAM_SIGNALS[0]);
    return EDF_STREAM_SIGNALS;
}

const EdfStreamSignalDescriptor *edf_stream_signal_descriptor_for_stream(
    StreamSignalId id) {
    size_t count = 0;
    const EdfStreamSignalDescriptor *signals =
        edf_stream_signal_descriptors(count);
    for (size_t i = 0; i < count; ++i) {
        if (signals[i].stream_id == id) return &signals[i];
    }
    return nullptr;
}

static std::string stream_ids_csv(EdfSeriesId excluded_series,
                                  bool required_only) {
    std::string out;
    size_t count = 0;
    const EdfStreamSignalDescriptor *signals =
        edf_stream_signal_descriptors(count);
    for (size_t i = 0; i < count; ++i) {
        if (signals[i].series == excluded_series) continue;

        const EdfFileSchema *schema =
            edf_numeric_schema_for_series(signals[i].series);
        if (!schema || (required_only && !schema->required)) continue;

        if (!out.empty()) out.push_back(',');
        out += signals[i].short_tag;
    }
    return out;
}

std::string edf_stream_ids_csv(bool required_only) {
    return stream_ids_csv(EdfSeriesId::Count, required_only);
}

std::string edf_stream_ids_csv_excluding(EdfSeriesId excluded_series,
                                         bool required_only) {
    return stream_ids_csv(excluded_series, required_only);
}

}  // namespace aircannect
