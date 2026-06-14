#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_writer.h"

namespace aircannect {

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

struct EdfDecodedRecordView {
    EdfSeriesId series = EdfSeriesId::Brp;
    float *values = nullptr;
    uint8_t *present = nullptr;
    uint8_t *valid = nullptr;
    size_t signal_count = 0;
    size_t samples_per_record = 0;
};

bool edf_parse_header_declared_size(const uint8_t *header,
                                    size_t available_size,
                                    uint32_t &out);

bool edf_parse_header_summary(const uint8_t *header,
                              size_t available_size,
                              EdfHeaderSummary &out);

bool edf_digital_minus_one_is_missing(const EdfSignalSpec &spec);

bool edf_decode_digital_sample(const EdfSignalSpec &spec,
                               int16_t digital_value,
                               float &physical_value,
                               bool &valid);

bool edf_decode_numeric_record(const EdfFileSchema &schema,
                               const uint8_t *record,
                               size_t record_size,
                               EdfDecodedRecordView &out);

}  // namespace aircannect
