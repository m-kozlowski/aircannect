#pragma once

#include <stddef.h>
#include <stdint.h>

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

bool edf_parse_header_declared_size(const uint8_t *header,
                                    size_t available_size,
                                    uint32_t &out);

bool edf_parse_header_summary(const uint8_t *header,
                              size_t available_size,
                              EdfHeaderSummary &out);

}  // namespace aircannect
