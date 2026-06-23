#include "edf_file_reader.h"

#include <string.h>

#include "edf_layout.h"

namespace aircannect {
namespace {

static constexpr size_t EDF_SIGNAL_TRANSDUCER_BLOCK_OFFSET =
    AC_EDF_SIGNAL_LABEL_WIDTH;
static constexpr size_t EDF_SIGNAL_PHYSICAL_DIMENSION_BLOCK_OFFSET =
    EDF_SIGNAL_TRANSDUCER_BLOCK_OFFSET + AC_EDF_SIGNAL_TRANSDUCER_WIDTH;
static constexpr size_t EDF_SIGNAL_PHYSICAL_MIN_BLOCK_OFFSET =
    EDF_SIGNAL_PHYSICAL_DIMENSION_BLOCK_OFFSET +
    AC_EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH;
static constexpr size_t EDF_SIGNAL_PHYSICAL_MAX_BLOCK_OFFSET =
    EDF_SIGNAL_PHYSICAL_MIN_BLOCK_OFFSET + AC_EDF_SIGNAL_PHYSICAL_MIN_WIDTH;
static constexpr size_t EDF_SIGNAL_DIGITAL_MIN_BLOCK_OFFSET =
    EDF_SIGNAL_PHYSICAL_MAX_BLOCK_OFFSET + AC_EDF_SIGNAL_PHYSICAL_MAX_WIDTH;
static constexpr size_t EDF_SIGNAL_DIGITAL_MAX_BLOCK_OFFSET =
    EDF_SIGNAL_DIGITAL_MIN_BLOCK_OFFSET + AC_EDF_SIGNAL_DIGITAL_MIN_WIDTH;
static constexpr size_t EDF_SIGNAL_PREFILTER_BLOCK_OFFSET =
    EDF_SIGNAL_DIGITAL_MAX_BLOCK_OFFSET + AC_EDF_SIGNAL_DIGITAL_MAX_WIDTH;
static constexpr size_t EDF_SIGNAL_SAMPLES_BLOCK_OFFSET =
    EDF_SIGNAL_PREFILTER_BLOCK_OFFSET + AC_EDF_SIGNAL_PREFILTER_WIDTH;
static constexpr size_t EDF_SIGNAL_RESERVED_BLOCK_OFFSET =
    EDF_SIGNAL_SAMPLES_BLOCK_OFFSET + AC_EDF_SIGNAL_SAMPLES_WIDTH;

bool parse_u32_field(const uint8_t *field, size_t width, uint32_t &out) {
    if (!field || width == 0) return false;
    uint32_t value = 0;
    bool any = false;
    bool trailing_space = false;
    for (size_t i = 0; i < width; ++i) {
        const char ch = static_cast<char>(field[i]);
        if (ch == ' ') {
            if (any) trailing_space = true;
            continue;
        }
        if (ch < '0' || ch > '9' || trailing_space) return false;
        any = true;
        const uint32_t digit = static_cast<uint32_t>(ch - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    if (!any) return false;
    out = value;
    return true;
}

void copy_trimmed_field(char *dst,
                        size_t dst_size,
                        const uint8_t *field,
                        size_t width) {
    if (!dst || dst_size == 0) return;
    dst[0] = 0;
    if (!field) return;
    size_t len = width;
    while (len > 0 && field[len - 1] == ' ') --len;
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, field, len);
    dst[len] = 0;
}

size_t signal_samples_offset(uint32_t signal_count) {
    return AC_EDF_HEADER_SIGNAL_HEADER_OFFSET +
           static_cast<size_t>(signal_count) *
               EDF_SIGNAL_SAMPLES_BLOCK_OFFSET;
}

size_t signal_field_offset(uint32_t signal_count,
                           uint32_t signal_index,
                           size_t field_block_offset,
                           size_t field_width) {
    return AC_EDF_HEADER_SIGNAL_HEADER_OFFSET +
           static_cast<size_t>(signal_count) * field_block_offset +
           static_cast<size_t>(signal_index) * field_width;
}

bool parse_total_samples_per_record(const uint8_t *header,
                                    uint32_t signal_count,
                                    uint32_t &out) {
    out = 0;
    const size_t offset = signal_samples_offset(signal_count);
    uint32_t total = 0;
    for (uint32_t i = 0; i < signal_count; ++i) {
        uint32_t samples = 0;
        if (!parse_u32_field(header + offset +
                                 static_cast<size_t>(i) *
                                     AC_EDF_SIGNAL_SAMPLES_WIDTH,
                             AC_EDF_SIGNAL_SAMPLES_WIDTH,
                             samples)) {
            return false;
        }
        if (samples > UINT32_MAX - total) return false;
        total += samples;
    }
    out = total;
    return true;
}

}  // namespace

bool edf_parse_header_declared_size(const uint8_t *header,
                                    size_t available_size,
                                    uint32_t &out) {
    out = 0;
    if (!header || available_size < AC_EDF_HEADER_FIXED_SIZE) return false;
    uint32_t header_size = 0;
    if (!parse_u32_field(header + AC_EDF_HEADER_SIZE_OFFSET,
                         AC_EDF_HEADER_SIZE_WIDTH,
                         header_size)) {
        return false;
    }
    if (header_size < AC_EDF_HEADER_FIXED_SIZE ||
        header_size % AC_EDF_HEADER_FIXED_SIZE != 0) {
        return false;
    }
    out = header_size;
    return true;
}

bool edf_parse_header_summary(const uint8_t *header,
                              size_t available_size,
                              EdfHeaderSummary &out) {
    out = {};
    if (!header || available_size < AC_EDF_HEADER_FIXED_SIZE) return false;

    uint32_t header_size = 0;
    uint32_t record_count = 0;
    uint32_t signal_count = 0;
    if (!edf_parse_header_declared_size(header, available_size, header_size) ||
        !parse_u32_field(header + AC_EDF_HEADER_RECORD_COUNT_OFFSET,
                         AC_EDF_HEADER_RECORD_COUNT_WIDTH,
                         record_count) ||
        !parse_u32_field(header + AC_EDF_HEADER_SIGNAL_COUNT_OFFSET,
                         AC_EDF_HEADER_SIGNAL_COUNT_WIDTH,
                         signal_count)) {
        return false;
    }
    if (header_size < AC_EDF_HEADER_FIXED_SIZE ||
        header_size % AC_EDF_HEADER_FIXED_SIZE != 0 ||
        available_size < header_size || signal_count == 0 ||
        header_size != AC_EDF_HEADER_FIXED_SIZE * (signal_count + 1u)) {
        return false;
    }
    uint32_t samples_per_record = 0;
    if (!parse_total_samples_per_record(header,
                                        signal_count,
                                        samples_per_record) ||
        samples_per_record == 0 || samples_per_record > UINT32_MAX / 2u) {
        return false;
    }

    out.header_size = header_size;
    out.record_count = record_count;
    out.signal_count = signal_count;
    out.samples_per_record = samples_per_record;
    out.record_size = samples_per_record * 2u;
    copy_trimmed_field(out.start_date,
                       sizeof(out.start_date),
                       header + AC_EDF_HEADER_START_DATE_OFFSET,
                       AC_EDF_HEADER_START_DATE_WIDTH);
    copy_trimmed_field(out.start_time,
                       sizeof(out.start_time),
                       header + AC_EDF_HEADER_START_TIME_OFFSET,
                       AC_EDF_HEADER_START_TIME_WIDTH);
    copy_trimmed_field(out.reserved,
                       sizeof(out.reserved),
                       header + AC_EDF_HEADER_RESERVED_OFFSET,
                       AC_EDF_HEADER_RESERVED_WIDTH);
    copy_trimmed_field(out.record_duration,
                       sizeof(out.record_duration),
                       header + AC_EDF_HEADER_RECORD_DURATION_OFFSET,
                       AC_EDF_HEADER_RECORD_DURATION_WIDTH);
    return true;
}

bool edf_parse_signal_header(const uint8_t *header,
                             size_t available_size,
                             uint32_t signal_index,
                             EdfSignalHeader &out) {
    out = {};
    EdfHeaderSummary summary;
    if (!edf_parse_header_summary(header, available_size, summary) ||
        signal_index >= summary.signal_count) {
        return false;
    }

    uint32_t sample_offset = 0;
    uint32_t samples = 0;
    const size_t samples_offset = signal_samples_offset(summary.signal_count);
    for (uint32_t i = 0; i <= signal_index; ++i) {
        if (!parse_u32_field(header + samples_offset +
                                 static_cast<size_t>(i) *
                                     AC_EDF_SIGNAL_SAMPLES_WIDTH,
                             AC_EDF_SIGNAL_SAMPLES_WIDTH,
                             samples)) {
            return false;
        }
        if (i < signal_index) {
            if (samples > UINT32_MAX - sample_offset) return false;
            sample_offset += samples;
        }
    }
    if (sample_offset > UINT32_MAX / 2u) return false;

    out.signal_index = signal_index;
    out.samples_per_record = samples;
    out.sample_offset_in_record = sample_offset;
    out.byte_offset_in_record = sample_offset * 2u;

    copy_trimmed_field(
        out.label,
        sizeof(out.label),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     0,
                                     AC_EDF_SIGNAL_LABEL_WIDTH),
        AC_EDF_SIGNAL_LABEL_WIDTH);
    copy_trimmed_field(
        out.transducer,
        sizeof(out.transducer),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_TRANSDUCER_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_TRANSDUCER_WIDTH),
        AC_EDF_SIGNAL_TRANSDUCER_WIDTH);
    copy_trimmed_field(
        out.physical_dimension,
        sizeof(out.physical_dimension),
        header + signal_field_offset(
                     summary.signal_count,
                     signal_index,
                     EDF_SIGNAL_PHYSICAL_DIMENSION_BLOCK_OFFSET,
                     AC_EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH),
        AC_EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH);
    copy_trimmed_field(
        out.physical_min,
        sizeof(out.physical_min),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_PHYSICAL_MIN_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_PHYSICAL_MIN_WIDTH),
        AC_EDF_SIGNAL_PHYSICAL_MIN_WIDTH);
    copy_trimmed_field(
        out.physical_max,
        sizeof(out.physical_max),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_PHYSICAL_MAX_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_PHYSICAL_MAX_WIDTH),
        AC_EDF_SIGNAL_PHYSICAL_MAX_WIDTH);
    copy_trimmed_field(
        out.digital_min,
        sizeof(out.digital_min),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_DIGITAL_MIN_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_DIGITAL_MIN_WIDTH),
        AC_EDF_SIGNAL_DIGITAL_MIN_WIDTH);
    copy_trimmed_field(
        out.digital_max,
        sizeof(out.digital_max),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_DIGITAL_MAX_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_DIGITAL_MAX_WIDTH),
        AC_EDF_SIGNAL_DIGITAL_MAX_WIDTH);
    copy_trimmed_field(
        out.prefilter,
        sizeof(out.prefilter),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_PREFILTER_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_PREFILTER_WIDTH),
        AC_EDF_SIGNAL_PREFILTER_WIDTH);
    copy_trimmed_field(
        out.samples_per_record_text,
        sizeof(out.samples_per_record_text),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_SAMPLES_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_SAMPLES_WIDTH),
        AC_EDF_SIGNAL_SAMPLES_WIDTH);
    copy_trimmed_field(
        out.reserved,
        sizeof(out.reserved),
        header + signal_field_offset(summary.signal_count,
                                     signal_index,
                                     EDF_SIGNAL_RESERVED_BLOCK_OFFSET,
                                     AC_EDF_SIGNAL_RESERVED_WIDTH),
        AC_EDF_SIGNAL_RESERVED_WIDTH);
    return true;
}

}  // namespace aircannect
