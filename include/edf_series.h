#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr uint32_t AC_EDF_RECORD_MS = 60000;
static constexpr uint32_t AC_EDF_BRP_SAMPLE_MS = 40;
static constexpr uint32_t AC_EDF_PLD_SAMPLE_MS = 2000;
static constexpr uint32_t AC_EDF_SA2_SAMPLE_MS = 1000;
static constexpr uint32_t AC_EDF_TCV_SAMPLE_MS = 40;
static constexpr size_t AC_EDF_BRP_SIGNAL_COUNT = 2;
static constexpr size_t AC_EDF_PLD_SIGNAL_COUNT = 12;
static constexpr size_t AC_EDF_SA2_SIGNAL_COUNT = 2;
static constexpr size_t AC_EDF_TCV_SIGNAL_COUNT = 1;
static constexpr size_t AC_EDF_BRP_SAMPLES_PER_RECORD =
    AC_EDF_RECORD_MS / AC_EDF_BRP_SAMPLE_MS;
static constexpr size_t AC_EDF_PLD_SAMPLES_PER_RECORD =
    AC_EDF_RECORD_MS / AC_EDF_PLD_SAMPLE_MS;
static constexpr size_t AC_EDF_SA2_SAMPLES_PER_RECORD =
    AC_EDF_RECORD_MS / AC_EDF_SA2_SAMPLE_MS;
static constexpr size_t AC_EDF_TCV_SAMPLES_PER_RECORD =
    AC_EDF_RECORD_MS / AC_EDF_TCV_SAMPLE_MS;

enum class EdfSeriesId : uint8_t {
    Brp,
    Pld,
    Sa2,
    Tcv,
    Count,
};

static constexpr size_t AC_EDF_NUMERIC_SERIES_COUNT =
    static_cast<size_t>(EdfSeriesId::Count);

constexpr size_t edf_series_index(EdfSeriesId series) {
    return static_cast<size_t>(series);
}

constexpr bool edf_series_id_valid(EdfSeriesId series) {
    return edf_series_index(series) < AC_EDF_NUMERIC_SERIES_COUNT;
}

struct EdfCompletedRecordView {
    EdfSeriesId series = EdfSeriesId::Brp;
    uint32_t record_index = 0;
    size_t signal_count = 0;
    size_t samples_per_record = 0;
    const float *values = nullptr;
    const uint8_t *present = nullptr;
    const uint8_t *valid = nullptr;
};

}  // namespace aircannect
