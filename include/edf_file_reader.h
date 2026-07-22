#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_layout.h"

namespace aircannect {

static constexpr size_t AC_EDF_SIGNAL_LABEL_TEXT_SIZE =
    AC_EDF_SIGNAL_LABEL_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_TRANSDUCER_TEXT_SIZE =
    AC_EDF_SIGNAL_TRANSDUCER_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_PHYSICAL_DIMENSION_TEXT_SIZE =
    AC_EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_PHYSICAL_MIN_TEXT_SIZE =
    AC_EDF_SIGNAL_PHYSICAL_MIN_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_PHYSICAL_MAX_TEXT_SIZE =
    AC_EDF_SIGNAL_PHYSICAL_MAX_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_DIGITAL_MIN_TEXT_SIZE =
    AC_EDF_SIGNAL_DIGITAL_MIN_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_DIGITAL_MAX_TEXT_SIZE =
    AC_EDF_SIGNAL_DIGITAL_MAX_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_PREFILTER_TEXT_SIZE =
    AC_EDF_SIGNAL_PREFILTER_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_SAMPLES_TEXT_SIZE =
    AC_EDF_SIGNAL_SAMPLES_WIDTH + 1;
static constexpr size_t AC_EDF_SIGNAL_RESERVED_TEXT_SIZE =
    AC_EDF_SIGNAL_RESERVED_WIDTH + 1;

struct EdfHeaderSummary {
    uint32_t header_size = 0;
    uint32_t record_count = 0;
    uint32_t signal_count = 0;
    uint32_t samples_per_record = 0;
    uint32_t record_size = 0;
    char start_date[9] = {};
    char start_time[9] = {};
    char reserved[45] = {};
    char record_duration[9] = {};
};

struct EdfSignalHeader {
    uint32_t signal_index = 0;
    uint32_t samples_per_record = 0;
    uint32_t sample_offset_in_record = 0;
    uint32_t byte_offset_in_record = 0;
    char label[AC_EDF_SIGNAL_LABEL_TEXT_SIZE] = {};
    char transducer[AC_EDF_SIGNAL_TRANSDUCER_TEXT_SIZE] = {};
    char physical_dimension[AC_EDF_SIGNAL_PHYSICAL_DIMENSION_TEXT_SIZE] = {};
    char physical_min[AC_EDF_SIGNAL_PHYSICAL_MIN_TEXT_SIZE] = {};
    char physical_max[AC_EDF_SIGNAL_PHYSICAL_MAX_TEXT_SIZE] = {};
    char digital_min[AC_EDF_SIGNAL_DIGITAL_MIN_TEXT_SIZE] = {};
    char digital_max[AC_EDF_SIGNAL_DIGITAL_MAX_TEXT_SIZE] = {};
    char prefilter[AC_EDF_SIGNAL_PREFILTER_TEXT_SIZE] = {};
    char samples_per_record_text[AC_EDF_SIGNAL_SAMPLES_TEXT_SIZE] = {};
    char reserved[AC_EDF_SIGNAL_RESERVED_TEXT_SIZE] = {};
};

struct EdfSignalScale {
    int16_t digital_min = 0;
    int16_t digital_max = 0;
    float physical_min = 0.0f;
    float physical_max = 0.0f;
    float scale = 0.0f;
    float offset = 0.0f;
};

bool edf_parse_header_declared_size(const uint8_t *header,
                                    size_t available_size,
                                    uint32_t &out);

bool edf_parse_header_summary(const uint8_t *header,
                              size_t available_size,
                              EdfHeaderSummary &out);

bool edf_parse_signal_header(const uint8_t *header,
                             size_t available_size,
                             uint32_t signal_index,
                             EdfSignalHeader &out);

bool edf_parse_header_start_ms(const EdfHeaderSummary &summary,
                               int64_t &out);
bool edf_parse_header_record_duration_ms(const EdfHeaderSummary &summary,
                                         uint32_t &out);
bool edf_parse_signal_scale(const EdfSignalHeader &signal,
                            EdfSignalScale &out);
float edf_scale_digital_sample(const EdfSignalScale &scale,
                               int16_t digital);
bool edf_digital_sample_is_missing(const EdfSignalScale &scale,
                                   int16_t digital);
bool edf_decode_signal_digital_sample(const EdfSignalHeader &signal,
                                      const uint8_t *record,
                                      size_t record_size,
                                      uint32_t sample_index,
                                      int16_t &out);
}  // namespace aircannect
