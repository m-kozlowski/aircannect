#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_writer.h"
#include "edf_layout.h"

namespace aircannect {

enum class EdfStrFileSizeStatus : uint8_t {
    Ok,
    MissingHeader,
    PartialTail,
};

struct EdfStrFileLayout {
    EdfStrFileSizeStatus status = EdfStrFileSizeStatus::MissingHeader;
    uint32_t record_count = 0;
    size_t append_offset = 0;
};

struct EdfStrRecordMatch {
    bool found = false;
    uint32_t index = 0;
};

struct EdfStrRecordLocation {
    uint32_t index = 0;
    size_t offset = 0;
    bool appending = false;
};

bool edf_str_file_layout_from_size(size_t file_size,
                                   EdfStrFileLayout &layout);
bool edf_str_header_schema_matches(const uint8_t *actual,
                                   const uint8_t *expected,
                                   size_t header_size);
bool edf_str_format_record_count_field(uint32_t record_count,
                                       char *field,
                                       size_t field_size);
size_t edf_str_record_offset(uint32_t record_index);
size_t edf_str_append_offset(uint32_t record_count);
int16_t edf_str_record_date_sample(const uint8_t *record, size_t len);
bool edf_str_date_sample_valid(int16_t date_sample);
bool edf_str_resolve_record_location(uint32_t record_count,
                                     const EdfStrRecordMatch &match,
                                     EdfStrRecordLocation &location);

}  // namespace aircannect
