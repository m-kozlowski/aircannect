#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

enum class EdfStrRecordMergeStatus : uint8_t {
    Ok,
    InvalidArgument,
    RecordSizeMismatch,
    BadDate,
    DateMismatch,
    BadMaskEvents,
    MaskEventOverflow,
};

EdfStrRecordMergeStatus edf_str_merge_existing_record(
    const uint8_t *existing_record,
    size_t existing_len,
    uint8_t *incoming_record,
    size_t incoming_len);

const char *edf_str_record_merge_status_name(
    EdfStrRecordMergeStatus status);

}  // namespace aircannect
