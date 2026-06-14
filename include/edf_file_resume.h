#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AC_EDF_HEADER_RECORD_COUNT_OFFSET = 236;
static constexpr size_t AC_EDF_HEADER_RECORD_COUNT_WIDTH = 8;
static constexpr size_t AC_EDF_HEADER_SIGNAL_HEADER_OFFSET = 256;

enum class EdfResumeStatus : uint8_t {
    Ok,
    InvalidArgument,
    FileTooSmall,
    PartialRecord,
    TooManyRecords,
    HeaderMismatch,
    BadRecordCount,
};

struct EdfResumeDecision {
    EdfResumeStatus status = EdfResumeStatus::InvalidArgument;
    uint32_t record_count = 0;
    uint32_t header_record_count = 0;
    bool header_record_count_matches = false;
};

bool edf_resume_parse_record_count_field(const uint8_t *header,
                                         size_t header_size,
                                         uint32_t &record_count);

bool edf_resume_header_matches(const uint8_t *actual,
                               const uint8_t *expected,
                               size_t header_size);

EdfResumeDecision edf_resume_check_file(const uint8_t *actual_header,
                                        const uint8_t *expected_header,
                                        size_t header_size,
                                        size_t file_size,
                                        size_t record_size);

}  // namespace aircannect
