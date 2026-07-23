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
};

}  // namespace aircannect
