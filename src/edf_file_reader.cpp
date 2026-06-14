#include "edf_file_reader.h"

#include <stdlib.h>
#include <string.h>

namespace aircannect {
namespace {

static constexpr size_t EDF_HEADER_FIXED_SIZE = 256;
static constexpr size_t EDF_HEADER_START_DATE_OFFSET = 168;
static constexpr size_t EDF_HEADER_START_TIME_OFFSET = 176;
static constexpr size_t EDF_HEADER_SIZE_OFFSET = 184;
static constexpr size_t EDF_HEADER_RESERVED_OFFSET = 192;
static constexpr size_t EDF_HEADER_RECORD_COUNT_OFFSET = 236;
static constexpr size_t EDF_HEADER_RECORD_DURATION_OFFSET = 244;
static constexpr size_t EDF_HEADER_SIGNAL_COUNT_OFFSET = 252;
static constexpr size_t EDF_SIGNAL_LABEL_WIDTH = 16;
static constexpr size_t EDF_SIGNAL_TRANSDUCER_WIDTH = 80;
static constexpr size_t EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH = 8;
static constexpr size_t EDF_SIGNAL_PHYSICAL_MIN_WIDTH = 8;
static constexpr size_t EDF_SIGNAL_PHYSICAL_MAX_WIDTH = 8;
static constexpr size_t EDF_SIGNAL_DIGITAL_MIN_WIDTH = 8;
static constexpr size_t EDF_SIGNAL_DIGITAL_MAX_WIDTH = 8;
static constexpr size_t EDF_SIGNAL_PREFILTER_WIDTH = 80;
static constexpr size_t EDF_SIGNAL_SAMPLES_WIDTH = 8;

size_t bitset_size(size_t bits) {
    return (bits + 7) / 8;
}

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
    return EDF_HEADER_FIXED_SIZE +
           static_cast<size_t>(signal_count) *
               (EDF_SIGNAL_LABEL_WIDTH +
                EDF_SIGNAL_TRANSDUCER_WIDTH +
                EDF_SIGNAL_PHYSICAL_DIMENSION_WIDTH +
                EDF_SIGNAL_PHYSICAL_MIN_WIDTH +
                EDF_SIGNAL_PHYSICAL_MAX_WIDTH +
                EDF_SIGNAL_DIGITAL_MIN_WIDTH +
                EDF_SIGNAL_DIGITAL_MAX_WIDTH +
                EDF_SIGNAL_PREFILTER_WIDTH);
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
                                     EDF_SIGNAL_SAMPLES_WIDTH,
                             EDF_SIGNAL_SAMPLES_WIDTH,
                             samples)) {
            return false;
        }
        if (samples > UINT32_MAX - total) return false;
        total += samples;
    }
    out = total;
    return true;
}

void bit_set(uint8_t *bits, size_t index) {
    if (!bits) return;
    bits[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

int16_t read_i16_le(const uint8_t *src) {
    const uint16_t raw =
        static_cast<uint16_t>(src[0]) |
        (static_cast<uint16_t>(src[1]) << 8);
    return static_cast<int16_t>(raw);
}

bool decode_scaled_sample(const EdfSignalSpec &spec,
                          int16_t digital_value,
                          float &physical_value) {
    if (spec.physical_to_digital_scale != 0.0f) {
        physical_value =
            (static_cast<float>(digital_value) -
             spec.physical_to_digital_offset) /
            spec.physical_to_digital_scale;
        return true;
    }

    const int32_t digital_min = spec.digital_min_value;
    const int32_t digital_max = spec.digital_max_value;
    if (digital_max <= digital_min) return false;

    const float physical_min = strtof(spec.physical_min, nullptr);
    const float physical_max = strtof(spec.physical_max, nullptr);
    if (physical_max <= physical_min) return false;

    physical_value =
        physical_min +
        (static_cast<float>(digital_value) - static_cast<float>(digital_min)) *
            (physical_max - physical_min) /
            static_cast<float>(digital_max - digital_min);
    return true;
}

}  // namespace

bool edf_parse_header_declared_size(const uint8_t *header,
                                    size_t available_size,
                                    uint32_t &out) {
    out = 0;
    if (!header || available_size < EDF_HEADER_FIXED_SIZE) return false;
    uint32_t header_size = 0;
    if (!parse_u32_field(header + EDF_HEADER_SIZE_OFFSET, 8, header_size)) {
        return false;
    }
    if (header_size < EDF_HEADER_FIXED_SIZE ||
        header_size % EDF_HEADER_FIXED_SIZE != 0) {
        return false;
    }
    out = header_size;
    return true;
}

bool edf_parse_header_summary(const uint8_t *header,
                              size_t available_size,
                              EdfHeaderSummary &out) {
    out = {};
    if (!header || available_size < EDF_HEADER_FIXED_SIZE) return false;

    uint32_t header_size = 0;
    uint32_t record_count = 0;
    uint32_t signal_count = 0;
    if (!edf_parse_header_declared_size(header, available_size, header_size) ||
        !parse_u32_field(header + EDF_HEADER_RECORD_COUNT_OFFSET,
                         8,
                         record_count) ||
        !parse_u32_field(header + EDF_HEADER_SIGNAL_COUNT_OFFSET,
                         4,
                         signal_count)) {
        return false;
    }
    if (header_size < EDF_HEADER_FIXED_SIZE ||
        header_size % EDF_HEADER_FIXED_SIZE != 0 ||
        available_size < header_size || signal_count == 0 ||
        header_size != EDF_HEADER_FIXED_SIZE * (signal_count + 1u)) {
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
                       header + EDF_HEADER_START_DATE_OFFSET,
                       8);
    copy_trimmed_field(out.start_time,
                       sizeof(out.start_time),
                       header + EDF_HEADER_START_TIME_OFFSET,
                       8);
    copy_trimmed_field(out.reserved,
                       sizeof(out.reserved),
                       header + EDF_HEADER_RESERVED_OFFSET,
                       44);
    copy_trimmed_field(out.record_duration,
                       sizeof(out.record_duration),
                       header + EDF_HEADER_RECORD_DURATION_OFFSET,
                       8);
    return true;
}

bool edf_digital_minus_one_is_missing(const EdfSignalSpec &spec) {
    return spec.digital_min_value >= 0;
}

bool edf_decode_digital_sample(const EdfSignalSpec &spec,
                               int16_t digital_value,
                               float &physical_value,
                               bool &valid) {
    physical_value = 0.0f;
    valid = false;
    if (digital_value == -1 && edf_digital_minus_one_is_missing(spec)) {
        return true;
    }
    if (digital_value < spec.digital_min_value ||
        digital_value > spec.digital_max_value) {
        return false;
    }
    if (!decode_scaled_sample(spec, digital_value, physical_value)) {
        return false;
    }
    valid = true;
    return true;
}

bool edf_decode_numeric_record(const EdfFileSchema &schema,
                               const uint8_t *record,
                               size_t record_size,
                               EdfDecodedRecordView &out) {
    const size_t required = edf_record_size(schema);
    if (!record || record_size != required || !schema.signals ||
        schema.signal_count == 0 ||
        schema.signal_count != schema.source_signal_count + 1 ||
        out.series != schema.series || !out.values ||
        out.signal_count != schema.source_signal_count ||
        out.samples_per_record != schema.source_samples_per_record) {
        return false;
    }

    const uint16_t expected_crc =
        edf_crc16_ccitt_false(record, record_size - 2);
    const uint16_t actual_crc =
        static_cast<uint16_t>(read_i16_le(record + record_size - 2));
    if (actual_crc != expected_crc) return false;

    const size_t sample_count = out.signal_count * out.samples_per_record;
    if (out.present) memset(out.present, 0, bitset_size(sample_count));
    if (out.valid) memset(out.valid, 0, bitset_size(sample_count));

    for (size_t signal = 0; signal < schema.source_signal_count; ++signal) {
        const EdfSignalSpec &spec = schema.signals[signal];
        for (size_t sample = 0; sample < schema.source_samples_per_record;
             ++sample) {
            const size_t index =
                signal * schema.source_samples_per_record + sample;
            const size_t offset = index * 2;
            float physical = 0.0f;
            bool valid = false;
            if (!edf_decode_digital_sample(spec,
                                           read_i16_le(record + offset),
                                           physical,
                                           valid)) {
                return false;
            }
            out.values[index] = physical;
            bit_set(out.present, index);
            if (valid) bit_set(out.valid, index);
        }
    }

    return true;
}

}  // namespace aircannect
