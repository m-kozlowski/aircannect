#include "edf_str_record_reader.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_signal_table.h"

namespace aircannect {
namespace {

constexpr int16_t STR_MISSING_DIGITAL = -1;

bool parse_float_field(const char *text, float &out) {
    if (!text || !text[0]) return false;

    char *end = nullptr;
    const float value = strtof(text, &end);
    if (end == text || !end || *end != '\0' || !isfinite(value)) return false;

    out = value;
    return true;
}

const EdfStrSignalDescriptor *find_descriptor(const char *label,
                                              size_t &signal_index) {
    if (!label || !label[0]) return nullptr;

    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *descriptor =
            edf_str_signal_descriptor(i);
        if (!descriptor || !descriptor->spec.label ||
            strcmp(descriptor->spec.label, label) != 0) {
            continue;
        }

        signal_index = i;
        return descriptor;
    }
    return nullptr;
}

bool decode_physical(const EdfStrSignalDescriptor &descriptor,
                     int16_t digital,
                     float &out) {
    const EdfSignalSpec &spec = descriptor.spec;
    if (digital == STR_MISSING_DIGITAL ||
        digital < spec.digital_min_value ||
        digital > spec.digital_max_value) {
        return false;
    }

    float physical_min = 0.0f;
    float physical_max = 0.0f;
    if (!parse_float_field(spec.physical_min, physical_min) ||
        !parse_float_field(spec.physical_max, physical_max)) {
        return false;
    }

    const float digital_span =
        static_cast<float>(spec.digital_max_value - spec.digital_min_value);
    if (digital_span == 0.0f) return false;

    const float physical_span = physical_max - physical_min;
    out = physical_min +
          static_cast<float>(digital - spec.digital_min_value) *
              physical_span / digital_span;
    return isfinite(out);
}

}  // namespace

bool edf_str_record_read_digital(const uint8_t *record,
                                 size_t len,
                                 const char *label,
                                 size_t sample_index,
                                 int16_t &out) {
    if (!record || len < edf_str_record_size()) return false;

    size_t signal_index = 0;
    const EdfStrSignalDescriptor *descriptor =
        find_descriptor(label, signal_index);
    if (!descriptor || sample_index >= descriptor->spec.samples_per_record) {
        return false;
    }

    const size_t sample_offset =
        edf_str_signal_sample_offset(signal_index) + sample_index;
    if (sample_offset >= AC_EDF_STR_SAMPLES_PER_RECORD) return false;

    out = edf_read_i16_le_sample(record, sample_offset);
    return out != STR_MISSING_DIGITAL;
}

bool edf_str_record_read_physical(const uint8_t *record,
                                  size_t len,
                                  const char *label,
                                  size_t sample_index,
                                  float &out) {
    size_t signal_index = 0;
    const EdfStrSignalDescriptor *descriptor =
        find_descriptor(label, signal_index);
    if (!descriptor) return false;

    int16_t digital = 0;
    if (!edf_str_record_read_digital(record,
                                     len,
                                     label,
                                     sample_index,
                                     digital)) {
        return false;
    }
    return decode_physical(*descriptor, digital, out);
}

}  // namespace aircannect
