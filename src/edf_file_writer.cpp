#include "edf_file_writer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace aircannect {
namespace {

static constexpr int16_t EDF_MISSING_DIGITAL = -1;

const EdfSignalSpec BRP_SIGNALS[] = {
    {"Flow.40ms", "L/s", "-2.00", "3.00", "-1000", "1500",
     static_cast<uint16_t>(static_cast<int16_t>(-1000)), 1500,
     AC_EDF_BRP_SAMPLES_PER_RECORD},
    {"Press.40ms", "cmH2O", "0.00", "40.00", "0", "2000",
     0, 2000, AC_EDF_BRP_SAMPLES_PER_RECORD},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     static_cast<uint16_t>(static_cast<int16_t>(-32768)), 32767, 1},
};

const EdfSignalSpec PLD_SIGNALS[] = {
    {"MaskPress.2s", "cmH2O", "0.00", "40.00", "0", "2000",
     0, 2000, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"Press.2s", "cmH2O", "0.00", "50.00", "0", "2500",
     0, 2500, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"EprPress.2s", "cmH2O", "0.00", "30.00", "0", "1500",
     0, 1500, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"Leak.2s", "L/s", "0.00", "2.00", "0", "100",
     0, 100, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"RespRate.2s", "bpm", "0.00", "90.00", "0", "450",
     0, 450, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"TidVol.2s", "L", "0.00", "4.00", "0", "200",
     0, 200, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"MinVent.2s", "L/min", "0.00", "30.00", "0", "240",
     0, 240, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"TgtVent.2s", "L/min", "0.00", "30.00", "0", "240",
     0, 240, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"IERatio.2s", "%", "0.00", "400.00", "0", "400",
     0, 400, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"Snore.2s", "", "0.00", "5.00", "0", "250",
     0, 250, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"FlowLim.2s", "", "0.00", "1.00", "0", "100",
     0, 100, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"Ti.2s", "seconds", "0.00", "10.00", "0", "500",
     0, 500, AC_EDF_PLD_SAMPLES_PER_RECORD},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     static_cast<uint16_t>(static_cast<int16_t>(-32768)), 32767, 1},
};

const EdfSignalSpec SA2_SIGNALS[] = {
    {"Pulse.1s", "bpm", "0.00", "300.00", "0", "300",
     0, 300, AC_EDF_SA2_SAMPLES_PER_RECORD},
    {"SpO2.1s", "%", "0.00", "100.00", "0", "100",
     0, 100, AC_EDF_SA2_SAMPLES_PER_RECORD},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     static_cast<uint16_t>(static_cast<int16_t>(-32768)), 32767, 1},
};

const EdfFileSchema BRP_SCHEMA = {
    EdfFileKind::Brp,
    EdfSeriesId::Brp,
    "BRP",
    "EDF",
    BRP_SIGNALS,
    sizeof(BRP_SIGNALS) / sizeof(BRP_SIGNALS[0]),
    AC_EDF_BRP_SIGNAL_COUNT,
    AC_EDF_BRP_SAMPLES_PER_RECORD,
    60,
};

const EdfFileSchema PLD_SCHEMA = {
    EdfFileKind::Pld,
    EdfSeriesId::Pld,
    "PLD",
    "EDF",
    PLD_SIGNALS,
    sizeof(PLD_SIGNALS) / sizeof(PLD_SIGNALS[0]),
    AC_EDF_PLD_SIGNAL_COUNT,
    AC_EDF_PLD_SAMPLES_PER_RECORD,
    60,
};

const EdfFileSchema SA2_SCHEMA = {
    EdfFileKind::Sa2,
    EdfSeriesId::Sa2,
    "SA2",
    "EDF",
    SA2_SIGNALS,
    sizeof(SA2_SIGNALS) / sizeof(SA2_SIGNALS[0]),
    AC_EDF_SA2_SIGNAL_COUNT,
    AC_EDF_SA2_SAMPLES_PER_RECORD,
    60,
};

void append_field(uint8_t *dst,
                  size_t capacity,
                  size_t &offset,
                  const char *text,
                  size_t width) {
    if (!dst || offset + width > capacity) return;
    memset(dst + offset, ' ', width);
    if (text) {
        const size_t len = strnlen(text, width);
        memcpy(dst + offset, text, len);
    }
    offset += width;
}

void append_u32_field(uint8_t *dst,
                      size_t capacity,
                      size_t &offset,
                      uint32_t value,
                      size_t width) {
    char tmp[16] = {};
    snprintf(tmp, sizeof(tmp), "%lu", static_cast<unsigned long>(value));
    append_field(dst, capacity, offset, tmp, width);
}

int32_t signed_value(uint16_t raw) {
    return static_cast<int16_t>(raw);
}

int16_t encode_sample(const EdfSignalSpec &spec, float physical_value) {
    const float physical_min = strtof(spec.physical_min, nullptr);
    const float physical_max = strtof(spec.physical_max, nullptr);
    const int32_t digital_min = signed_value(spec.digital_min_value);
    const int32_t digital_max = signed_value(spec.digital_max_value);
    if (physical_max <= physical_min || digital_max <= digital_min) {
        return static_cast<int16_t>(digital_min);
    }

    const float scaled =
        static_cast<float>(digital_min) +
        (physical_value - physical_min) *
            static_cast<float>(digital_max - digital_min) /
            (physical_max - physical_min);
    long digital = lroundf(scaled);
    if (digital < digital_min) digital = digital_min;
    if (digital > digital_max) digital = digital_max;
    return static_cast<int16_t>(digital);
}

void append_i16_le(uint8_t *dst, size_t capacity, size_t &offset, int16_t value) {
    if (!dst || offset + 2 > capacity) return;
    const uint16_t raw = static_cast<uint16_t>(value);
    dst[offset++] = static_cast<uint8_t>(raw & 0xff);
    dst[offset++] = static_cast<uint8_t>((raw >> 8) & 0xff);
}

bool bit_get(const uint8_t *bits, size_t index) {
    if (!bits) return false;
    return (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

}  // namespace

const EdfFileSchema &edf_numeric_schema(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp:
            return BRP_SCHEMA;
        case EdfFileKind::Pld:
            return PLD_SCHEMA;
        case EdfFileKind::Sa2:
        default:
            return SA2_SCHEMA;
    }
}

const EdfFileSchema *edf_numeric_schema_for_series(EdfSeriesId series) {
    switch (series) {
        case EdfSeriesId::Brp:
            return &BRP_SCHEMA;
        case EdfSeriesId::Pld:
            return &PLD_SCHEMA;
        case EdfSeriesId::Sa2:
            return &SA2_SCHEMA;
        default:
            return nullptr;
    }
}

size_t edf_header_size(const EdfFileSchema &schema) {
    return 256 + schema.signal_count * 256;
}

size_t edf_record_size(const EdfFileSchema &schema) {
    size_t samples = 0;
    for (size_t i = 0; i < schema.signal_count; ++i) {
        samples += schema.signals[i].samples_per_record;
    }
    return samples * 2;
}

uint16_t edf_crc16_ccitt_false(const uint8_t *data, size_t len) {
    uint16_t crc = 0xffff;
    if (!data && len) return crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (uint8_t bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000)
                      ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                      : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool edf_render_header(const EdfFileSchema &schema,
                       const EdfHeaderInfo &info,
                       uint8_t *dst,
                       size_t capacity,
                       size_t &written) {
    written = 0;
    const size_t required = edf_header_size(schema);
    if (!dst || capacity < required || !schema.signals ||
        schema.signal_count == 0) {
        return false;
    }

    memset(dst, ' ', required);
    size_t offset = 0;
    append_field(dst, capacity, offset, "0", 8);
    append_field(dst, capacity, offset, info.patient_id, 80);
    append_field(dst, capacity, offset, info.recording_id, 80);
    append_field(dst, capacity, offset, info.start_date, 8);
    append_field(dst, capacity, offset, info.start_time, 8);
    append_u32_field(dst, capacity, offset, static_cast<uint32_t>(required), 8);
    append_field(dst, capacity, offset, schema.reserved, 44);
    append_u32_field(dst, capacity, offset, info.record_count, 8);
    append_field(dst, capacity, offset,
                 schema.record_duration_seconds == 60 ? "60.00" : "0.00", 8);
    append_u32_field(dst, capacity, offset,
                     static_cast<uint32_t>(schema.signal_count), 4);

    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, schema.signals[i].label, 16);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, "", 80);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset,
                     schema.signals[i].physical_dimension, 8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, schema.signals[i].physical_min, 8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, schema.signals[i].physical_max, 8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, schema.signals[i].digital_min, 8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, schema.signals[i].digital_max, 8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, "", 80);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_u32_field(dst, capacity, offset,
                         static_cast<uint32_t>(
                             schema.signals[i].samples_per_record),
                         8);
    }
    for (size_t i = 0; i < schema.signal_count; ++i) {
        append_field(dst, capacity, offset, "", 32);
    }

    if (offset != required) return false;
    written = offset;
    return true;
}

bool edf_render_numeric_record(const EdfFileSchema &schema,
                               const EdfCompletedRecordView &record,
                               uint8_t *dst,
                               size_t capacity,
                               size_t &written) {
    written = 0;
    const size_t required = edf_record_size(schema);
    if (!dst || capacity < required || !schema.signals ||
        schema.signal_count == 0 || !record.values || !record.present ||
        !record.valid || record.series != schema.series ||
        record.signal_count != schema.source_signal_count ||
        record.samples_per_record != schema.source_samples_per_record ||
        schema.signal_count != schema.source_signal_count + 1) {
        return false;
    }

    size_t offset = 0;
    for (size_t signal = 0; signal < schema.source_signal_count; ++signal) {
        const EdfSignalSpec &spec = schema.signals[signal];
        for (size_t sample = 0; sample < schema.source_samples_per_record;
             ++sample) {
            const size_t source_index =
                signal * schema.source_samples_per_record + sample;
            const bool present = bit_get(record.present, source_index);
            const bool valid = bit_get(record.valid, source_index);
            const int16_t digital =
                (present && valid) ? encode_sample(spec,
                                                   record.values[source_index])
                                   : EDF_MISSING_DIGITAL;
            append_i16_le(dst, capacity, offset, digital);
        }
    }

    const uint16_t crc = edf_crc16_ccitt_false(dst, offset);
    append_i16_le(dst, capacity, offset, static_cast<int16_t>(crc));
    if (offset != required) return false;
    written = offset;
    return true;
}

}  // namespace aircannect
