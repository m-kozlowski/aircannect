#include "edf_stream_signal_table.h"

namespace aircannect {
namespace {

const EdfStreamSignalDescriptor EDF_STREAM_SIGNALS[] = {
    {"_RFL", StreamSignalId::PatientFlow, EdfFileKind::Brp,
     EdfSeriesId::Brp, 0, AC_EDF_BRP_SAMPLE_MS},
    {"_MKP", StreamSignalId::MaskPressure, EdfFileKind::Brp,
     EdfSeriesId::Brp, 1, AC_EDF_BRP_SAMPLE_MS},
    {"_MKF", StreamSignalId::MaskPressureTwoSecond, EdfFileKind::Pld,
     EdfSeriesId::Pld, 0, AC_EDF_PLD_SAMPLE_MS},
    {"_MKI", StreamSignalId::InspiratoryPressureTwoSecond, EdfFileKind::Pld,
     EdfSeriesId::Pld, 1, AC_EDF_PLD_SAMPLE_MS},
    {"_MKE", StreamSignalId::ExpiratoryPressureTwoSecond, EdfFileKind::Pld,
     EdfSeriesId::Pld, 2, AC_EDF_PLD_SAMPLE_MS},
    {"_LKF", StreamSignalId::Leak, EdfFileKind::Pld,
     EdfSeriesId::Pld, 3, AC_EDF_PLD_SAMPLE_MS},
    {"_RR2", StreamSignalId::RespiratoryRate, EdfFileKind::Pld,
     EdfSeriesId::Pld, 4, AC_EDF_PLD_SAMPLE_MS},
    {"_TD2", StreamSignalId::TidalVolume, EdfFileKind::Pld,
     EdfSeriesId::Pld, 5, AC_EDF_PLD_SAMPLE_MS},
    {"_MV2", StreamSignalId::MinuteVentilation, EdfFileKind::Pld,
     EdfSeriesId::Pld, 6, AC_EDF_PLD_SAMPLE_MS},
    {"_TGT", StreamSignalId::TargetMinuteVentilation, EdfFileKind::Pld,
     EdfSeriesId::Pld, 7, AC_EDF_PLD_SAMPLE_MS},
    {"_IE2", StreamSignalId::IeRatio, EdfFileKind::Pld,
     EdfSeriesId::Pld, 8, AC_EDF_PLD_SAMPLE_MS},
    {"_SNI", StreamSignalId::SnoreIndex, EdfFileKind::Pld,
     EdfSeriesId::Pld, 9, AC_EDF_PLD_SAMPLE_MS},
    {"_FFL", StreamSignalId::FlowLimitation, EdfFileKind::Pld,
     EdfSeriesId::Pld, 10, AC_EDF_PLD_SAMPLE_MS},
    {"_INT", StreamSignalId::InspiratoryDuration, EdfFileKind::Pld,
     EdfSeriesId::Pld, 11, AC_EDF_PLD_SAMPLE_MS},
    {"_HRT", StreamSignalId::HeartRate, EdfFileKind::Sa2,
     EdfSeriesId::Sa2, 0, AC_EDF_SA2_SAMPLE_MS},
    {"_SAO", StreamSignalId::SpO2, EdfFileKind::Sa2,
     EdfSeriesId::Sa2, 1, AC_EDF_SA2_SAMPLE_MS},
    {"_BYV", StreamSignalId::TriggerCycleEvent, EdfFileKind::Tcv,
     EdfSeriesId::Tcv, 0, AC_EDF_TCV_SAMPLE_MS},
};

}  // namespace

const char *const DEFAULT_EDF_STREAM_IDS =
    "_RFL,"
    "_MKP,"
    "_MKF,"
    "_MKI,"
    "_MKE,"
    "_LKF,"
    "_RR2,"
    "_TD2,"
    "_MV2,"
    "_TGT,"
    "_IE2,"
    "_SNI,"
    "_FFL,"
    "_INT,"
    "_HRT,"
    "_SAO,"
    "_BYV";

const char *const REQUIRED_EDF_STREAM_IDS =
    "_RFL,"
    "_MKP,"
    "_MKF,"
    "_MKI,"
    "_MKE,"
    "_LKF,"
    "_RR2,"
    "_TD2,"
    "_MV2,"
    "_TGT,"
    "_IE2,"
    "_SNI,"
    "_FFL,"
    "_INT,"
    "_HRT,"
    "_SAO";

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

}  // namespace aircannect
