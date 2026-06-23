#include "edf_file_reader.h"

#include <stdlib.h>
#include <string.h>

#include "calendar_utils.h"
#include "edf_bytes.h"
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

bool parse_i16_text(const char *text, int16_t &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (!end || *end != '\0' || value < INT16_MIN || value > INT16_MAX) {
        return false;
    }
    out = static_cast<int16_t>(value);
    return true;
}

bool parse_float_text(const char *text, float &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (!end || *end != '\0') return false;
    out = value;
    return true;
}

bool parse_two_digits(const char *text, int &out) {
    if (!text || text[0] < '0' || text[0] > '9' ||
        text[1] < '0' || text[1] > '9') {
        return false;
    }
    out = (text[0] - '0') * 10 + (text[1] - '0');
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

bool edf_parse_header_start_ms(const EdfHeaderSummary &summary,
                               int64_t &out) {
    out = 0;
    const char *date = summary.start_date;
    const char *time = summary.start_time;
    if (strlen(date) != 8 || strlen(time) != 8 ||
        date[2] != '.' || date[5] != '.' ||
        time[2] != '.' || time[5] != '.') {
        return false;
    }

    int day = 0;
    int month = 0;
    int year2 = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!parse_two_digits(date, day) ||
        !parse_two_digits(date + 3, month) ||
        !parse_two_digits(date + 6, year2) ||
        !parse_two_digits(time, hour) ||
        !parse_two_digits(time + 3, minute) ||
        !parse_two_digits(time + 6, second)) {
        return false;
    }

    const int year = year2 >= 85 ? 1900 + year2 : 2000 + year2;
    if (month < 1 || month > 12 || day < 1 ||
        day > calendar_days_in_month(year, month) ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59) {
        return false;
    }
    const int64_t days =
        calendar_days_from_civil(year,
                                 static_cast<unsigned>(month),
                                 static_cast<unsigned>(day));
    out = (days * 86400 + static_cast<int64_t>(hour) * 3600 +
           static_cast<int64_t>(minute) * 60 + second) * 1000;
    return true;
}

bool edf_parse_header_record_duration_ms(const EdfHeaderSummary &summary,
                                         uint32_t &out) {
    out = 0;
    const char *text = summary.record_duration;
    if (!text || !text[0]) return false;

    uint64_t seconds = 0;
    size_t i = 0;
    bool any = false;
    while (text[i] >= '0' && text[i] <= '9') {
        any = true;
        const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
        if (seconds > (UINT32_MAX / 1000u - digit) / 10u) return false;
        seconds = seconds * 10u + digit;
        ++i;
    }
    if (!any) return false;

    uint32_t fractional_ms = 0;
    if (text[i] == '.') {
        ++i;
        uint32_t scale = 100;
        if (text[i] < '0' || text[i] > '9') return false;
        while (text[i] >= '0' && text[i] <= '9') {
            if (scale > 0) {
                fractional_ms +=
                    static_cast<uint32_t>(text[i] - '0') * scale;
                scale /= 10;
            }
            ++i;
        }
    }
    if (text[i] != '\0') return false;
    const uint64_t total = seconds * 1000u + fractional_ms;
    if (total > UINT32_MAX) return false;
    out = static_cast<uint32_t>(total);
    return true;
}

bool edf_parse_signal_scale(const EdfSignalHeader &signal,
                            EdfSignalScale &out) {
    out = EdfSignalScale();
    if (!parse_i16_text(signal.digital_min, out.digital_min) ||
        !parse_i16_text(signal.digital_max, out.digital_max) ||
        !parse_float_text(signal.physical_min, out.physical_min) ||
        !parse_float_text(signal.physical_max, out.physical_max) ||
        out.digital_max == out.digital_min) {
        return false;
    }
    out.scale = (out.physical_max - out.physical_min) /
                static_cast<float>(out.digital_max - out.digital_min);
    out.offset = out.physical_min -
                 static_cast<float>(out.digital_min) * out.scale;
    return true;
}

float edf_scale_digital_sample(const EdfSignalScale &scale,
                               int16_t digital) {
    return static_cast<float>(digital) * scale.scale + scale.offset;
}

bool edf_digital_sample_is_missing(const EdfSignalScale &scale,
                                   int16_t digital) {
    return digital == -1 &&
           (digital < scale.digital_min || digital > scale.digital_max);
}

bool edf_decode_signal_digital_sample(const EdfSignalHeader &signal,
                                      const uint8_t *record,
                                      size_t record_size,
                                      uint32_t sample_index,
                                      int16_t &out) {
    out = 0;
    if (!record || sample_index >= signal.samples_per_record) return false;
    const size_t offset = static_cast<size_t>(signal.byte_offset_in_record) +
                          static_cast<size_t>(sample_index) * 2u;
    if (offset > record_size || record_size - offset < 2u) return false;
    out = edf_read_i16_le(record + offset);
    return true;
}

bool edf_decode_signal_physical_sample(const EdfSignalHeader &signal,
                                       const EdfSignalScale &scale,
                                       const uint8_t *record,
                                       size_t record_size,
                                       uint32_t sample_index,
                                       float &out) {
    out = 0.0f;
    int16_t digital = 0;
    if (!edf_decode_signal_digital_sample(signal,
                                          record,
                                          record_size,
                                          sample_index,
                                          digital)) {
        return false;
    }
    out = edf_scale_digital_sample(scale, digital);
    return true;
}

}  // namespace aircannect
