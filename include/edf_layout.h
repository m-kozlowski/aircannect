#pragma once

#include <stddef.h>

namespace aircannect {

static constexpr size_t AC_EDF_HEADER_VERSION_OFFSET = 0;
static constexpr size_t AC_EDF_HEADER_VERSION_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_PATIENT_ID_OFFSET = 8;
static constexpr size_t AC_EDF_HEADER_PATIENT_ID_WIDTH = 80;
static constexpr size_t AC_EDF_HEADER_RECORDING_ID_WIDTH = 80;
static constexpr size_t AC_EDF_HEADER_START_DATE_OFFSET = 168;
static constexpr size_t AC_EDF_HEADER_START_DATE_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_START_TIME_OFFSET = 176;
static constexpr size_t AC_EDF_HEADER_START_TIME_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_SIZE_OFFSET = 184;
static constexpr size_t AC_EDF_HEADER_SIZE_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_RESERVED_OFFSET = 192;
static constexpr size_t AC_EDF_HEADER_RESERVED_WIDTH = 44;
static constexpr size_t AC_EDF_HEADER_RECORD_COUNT_OFFSET = 236;
static constexpr size_t AC_EDF_HEADER_RECORD_COUNT_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_RECORD_DURATION_OFFSET = 244;
static constexpr size_t AC_EDF_HEADER_RECORD_DURATION_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_SIGNAL_COUNT_OFFSET = 252;
static constexpr size_t AC_EDF_HEADER_SIGNAL_COUNT_WIDTH = 4;

static constexpr size_t AC_EDF_HEADER_FIXED_SIZE = 256;
static constexpr size_t AC_EDF_HEADER_SIGNAL_HEADER_OFFSET =
    AC_EDF_HEADER_FIXED_SIZE;
static constexpr size_t AC_EDF_SIGNAL_HEADER_SIZE = 256;

}  // namespace aircannect
