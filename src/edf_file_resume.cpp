#include "edf_file_resume.h"

#include <string.h>

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

bool edf_resume_parse_record_count_field(const uint8_t *header,
                                         size_t header_size,
                                         uint32_t &record_count) {
    if (!header || header_size < AC_EDF_HEADER_RECORD_COUNT_OFFSET +
                                    AC_EDF_HEADER_RECORD_COUNT_WIDTH) {
        return false;
    }
    uint32_t value = 0;
    bool any = false;
    bool trailing_space = false;
    const uint8_t *field = header + AC_EDF_HEADER_RECORD_COUNT_OFFSET;
    for (size_t i = 0; i < AC_EDF_HEADER_RECORD_COUNT_WIDTH; ++i) {
        const char c = static_cast<char>(field[i]);
        if (c == ' ') {
            if (any) trailing_space = true;
            continue;
        }
        if (c < '0' || c > '9') return false;
        if (trailing_space) return false;
        any = true;
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (!any) return false;
    record_count = value;
    return true;
}

bool edf_resume_header_matches(const uint8_t *actual,
                               const uint8_t *expected,
                               size_t header_size) {
    if (header_size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET) return false;

    // Patient ID can legitimately vary across firmware/user revisions. The
    // recording ID and signal header carry the session/device/schema identity.
    return header_range_matches(actual, expected, header_size, 0, 8) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                88,
                                AC_EDF_HEADER_RECORD_COUNT_OFFSET) &&
           header_range_matches(actual,
                                expected,
                                header_size,
                                AC_EDF_HEADER_RECORD_COUNT_OFFSET +
                                    AC_EDF_HEADER_RECORD_COUNT_WIDTH,
                                header_size);
}

EdfResumeDecision edf_resume_check_file(const uint8_t *actual_header,
                                        const uint8_t *expected_header,
                                        size_t header_size,
                                        size_t file_size,
                                        size_t record_size) {
    EdfResumeDecision decision;
    if (!actual_header || !expected_header ||
        header_size < AC_EDF_HEADER_SIGNAL_HEADER_OFFSET ||
        record_size == 0) {
        return decision;
    }
    if (file_size < header_size) {
        decision.status = EdfResumeStatus::FileTooSmall;
        return decision;
    }
    const size_t data_size = file_size - header_size;
    if (data_size % record_size != 0) {
        decision.status = EdfResumeStatus::PartialRecord;
        return decision;
    }
    const size_t records_from_size = data_size / record_size;
    if (records_from_size > UINT32_MAX) {
        decision.status = EdfResumeStatus::TooManyRecords;
        return decision;
    }
    if (!edf_resume_header_matches(actual_header, expected_header,
                                   header_size)) {
        decision.status = EdfResumeStatus::HeaderMismatch;
        return decision;
    }

    uint32_t header_record_count = 0;
    if (!edf_resume_parse_record_count_field(actual_header,
                                             header_size,
                                             header_record_count)) {
        decision.status = EdfResumeStatus::BadRecordCount;
        return decision;
    }

    decision.status = EdfResumeStatus::Ok;
    decision.record_count = static_cast<uint32_t>(records_from_size);
    decision.header_record_count = header_record_count;
    decision.header_record_count_matches =
        header_record_count == decision.record_count;
    return decision;
}

}  // namespace aircannect
