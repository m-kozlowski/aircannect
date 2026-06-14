#include "edf_signal_router.h"

namespace aircannect {

bool edf_signal_target_for_stream(StreamSignalId id,
                                  EdfSignalTarget &target) {
    switch (id) {
        case StreamSignalId::PatientFlow:
            target = {EdfSeriesId::Brp, 0, AC_EDF_BRP_SAMPLE_MS};
            return true;
        case StreamSignalId::MaskPressure:
            target = {EdfSeriesId::Brp, 1, AC_EDF_BRP_SAMPLE_MS};
            return true;

        case StreamSignalId::MaskPressureTwoSecond:
            target = {EdfSeriesId::Pld, 0, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::InspiratoryPressureTwoSecond:
            target = {EdfSeriesId::Pld, 1, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::ExpiratoryPressureTwoSecond:
            target = {EdfSeriesId::Pld, 2, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::Leak:
            target = {EdfSeriesId::Pld, 3, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::RespiratoryRate:
            target = {EdfSeriesId::Pld, 4, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::TidalVolume:
            target = {EdfSeriesId::Pld, 5, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::MinuteVentilation:
            target = {EdfSeriesId::Pld, 6, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::TargetMinuteVentilation:
            target = {EdfSeriesId::Pld, 7, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::IeRatio:
            target = {EdfSeriesId::Pld, 8, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::SnoreIndex:
            target = {EdfSeriesId::Pld, 9, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::FlowLimitation:
            target = {EdfSeriesId::Pld, 10, AC_EDF_PLD_SAMPLE_MS};
            return true;
        case StreamSignalId::InspiratoryDuration:
            target = {EdfSeriesId::Pld, 11, AC_EDF_PLD_SAMPLE_MS};
            return true;

        case StreamSignalId::HeartRate:
            target = {EdfSeriesId::Sa2, 0, AC_EDF_SA2_SAMPLE_MS};
            return true;
        case StreamSignalId::SpO2:
            target = {EdfSeriesId::Sa2, 1, AC_EDF_SA2_SAMPLE_MS};
            return true;

        case StreamSignalId::Unknown:
        case StreamSignalId::InspiratoryPressure:
        case StreamSignalId::ExpiratoryPressure:
        default:
            return false;
    }
}

}  // namespace aircannect
