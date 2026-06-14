#include "edf_str_file_layout.h"

#include <stdio.h>
#include <string.h>

#include "edf_bytes.h"

namespace aircannect {
namespace {

bool header_range_matches(const uint8_t *actual,
                          const uint8_t *expected,
                          size_t header_size,
                          size_t begin,
                          size_t end) {
    return actual && expected && begin <= end && end <= header_size &&
           memcmp(actual + begin, expected + begin, end - begin) == 0;
}

}  // namespace

bool edf_str_file_layout_from_size(size_t file_size,
                                   EdfStrFileLayout &layout) {
    layout = {};
    const size_t header_size = edf_str_header_size();
    const size_t record_size = edf_str_record_size();
    if (file_size < header_size || record_size == 0) {
        layout.status = EdfStrFileSizeStatus::MissingHeader;
        return false;
    }

    const size_t payload_size = file_size - header_size;
    if ((payload_size % record_size) != 0) {
        layout.status = EdfStrFileSizeStatus::PartialTail;
        return false;
    }

    layout.status = EdfStrFileSizeStatus::Ok;
    layout.record_count = static_cast<uint32_t>(payload_size / record_size);
    layout.append_offset = file_size;
    return true;
}

bool edf_str_header_schema_matches(const uint8_t *actual,
                                   const uint8_t *expected,
                                   size_t header_size) {
    if (!actual || !expected ||
        header_size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET) {
        return false;
    }

    return header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_VERSION_OFFSET,
                                AC_EDF_HEADER_VERSION_OFFSET +
                                    AC_EDF_HEADER_VERSION_WIDTH) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_SIZE_OFFSET,
                                AC_EDF_HEADER_SIZE_OFFSET +
                                    AC_EDF_HEADER_SIZE_WIDTH) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_RESERVED_OFFSET,
                                AC_EDF_HEADER_RESERVED_OFFSET +
                                    AC_EDF_HEADER_RESERVED_WIDTH) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_RECORD_DURATION_OFFSET,
                                AC_EDF_HEADER_RECORD_DURATION_OFFSET +
                                    AC_EDF_HEADER_RECORD_DURATION_WIDTH) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_SIGNAL_COUNT_OFFSET,
                                AC_EDF_HEADER_SIGNAL_COUNT_OFFSET +
                                    AC_EDF_HEADER_SIGNAL_COUNT_WIDTH) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_SIGNAL_HEADER_OFFSET,
                                header_size);
}

bool edf_str_format_record_count_field(uint32_t record_count,
                                       char *field,
                                       size_t field_size) {
    if (!field || field_size != AC_EDF_HEADER_RECORD_COUNT_WIDTH) {
        return false;
    }
    memset(field, ' ', field_size);
    char text[16] = {};
    snprintf(text, sizeof(text), "%lu",
             static_cast<unsigned long>(record_count));
    const size_t len = strlen(text);
    if (len > field_size) return false;
    memcpy(field, text, len);
    return true;
}

size_t edf_str_record_offset(uint32_t record_index) {
    return edf_str_header_size() +
           static_cast<size_t>(record_index) * edf_str_record_size();
}

size_t edf_str_append_offset(uint32_t record_count) {
    return edf_str_record_offset(record_count);
}

int16_t edf_str_record_date_sample(const uint8_t *record, size_t len) {
    if (!record || len < 2) return -1;
    return edf_read_i16_le(record);
}

bool edf_str_date_sample_valid(int16_t date_sample) {
    return date_sample >= 0;
}

bool edf_str_resolve_record_location(uint32_t record_count,
                                     const EdfStrRecordMatch &match,
                                     EdfStrRecordLocation &location) {
    location = {};
    if (match.found) {
        if (match.index >= record_count) return false;
        location.index = match.index;
        location.offset = edf_str_record_offset(match.index);
        location.appending = false;
        return true;
    }

    location.index = record_count;
    location.offset = edf_str_append_offset(record_count);
    location.appending = true;
    return true;
}

}  // namespace aircannect
