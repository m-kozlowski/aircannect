#include "edf_file_writer.h"

#include <array>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

namespace aircannect {
namespace {

static constexpr int16_t EDF_MISSING_DIGITAL = -1;
static constexpr size_t EDF_PATIENT_ID_OFFSET = 0x08;
static constexpr size_t EDF_PATIENT_ID_WIDTH = 80;
static constexpr size_t EDF_PATIENT_FIRST_CRC_SOURCE_OFFSET = 0x19;
static constexpr size_t EDF_PATIENT_FIRST_CRC_SOURCE_LEN = 0xe7;
static constexpr size_t EDF_SIGNAL_HEADER_OFFSET = 0x100;

const EdfSignalSpec BRP_SIGNALS[] = {
    {"Flow.40ms", "L/s", "-2.00", "3.00", "-1000", "1500",
     -1000, 1500, AC_EDF_BRP_SAMPLES_PER_RECORD, 500.0f, 0.0f},
    {"Press.40ms", "cmH2O", "0.00", "40.00", "0", "2000",
     0, 2000, AC_EDF_BRP_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     -32768, 32767, 1, 1.0f, 0.0f},
};

const EdfSignalSpec PLD_SIGNALS[] = {
    {"MaskPress.2s", "cmH2O", "0.00", "40.00", "0", "2000",
     0, 2000, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"Press.2s", "cmH2O", "0.00", "50.00", "0", "2500",
     0, 2500, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"EprPress.2s", "cmH2O", "0.00", "30.00", "0", "1500",
     0, 1500, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"Leak.2s", "L/s", "0.00", "2.00", "0", "100",
     0, 100, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"RespRate.2s", "bpm", "0.00", "90.00", "0", "450",
     0, 450, AC_EDF_PLD_SAMPLES_PER_RECORD, 5.0f, 0.0f},
    {"TidVol.2s", "L", "0.00", "4.00", "0", "200",
     0, 200, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"MinVent.2s", "L/min", "0.00", "30.00", "0", "240",
     0, 240, AC_EDF_PLD_SAMPLES_PER_RECORD, 8.0f, 0.0f},
    {"TgtVent.2s", "L/min", "0.00", "30.00", "0", "240",
     0, 240, AC_EDF_PLD_SAMPLES_PER_RECORD, 8.0f, 0.0f},
    {"IERatio.2s", "%", "0.00", "400.00", "0", "400",
     0, 400, AC_EDF_PLD_SAMPLES_PER_RECORD, 1.0f, 0.0f},
    {"Snore.2s", "", "0.00", "5.00", "0", "250",
     0, 250, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"FlowLim.2s", "", "0.00", "1.00", "0", "100",
     0, 100, AC_EDF_PLD_SAMPLES_PER_RECORD, 100.0f, 0.0f},
    {"Ti.2s", "seconds", "0.00", "10.00", "0", "500",
     0, 500, AC_EDF_PLD_SAMPLES_PER_RECORD, 50.0f, 0.0f},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     -32768, 32767, 1, 1.0f, 0.0f},
};

const EdfSignalSpec SA2_SIGNALS[] = {
    {"Pulse.1s", "bpm", "0.00", "300.00", "0", "300",
     0, 300, AC_EDF_SA2_SAMPLES_PER_RECORD, 1.0f, 0.0f},
    {"SpO2.1s", "%", "0.00", "100.00", "0", "100",
     0, 100, AC_EDF_SA2_SAMPLES_PER_RECORD, 1.0f, 0.0f},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     -32768, 32767, 1, 1.0f, 0.0f},
};

const uint8_t BRP_SOURCE_INDICES[] = {0, 1};
const uint8_t PLD_SOURCE_INDICES[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t SA2_SOURCE_INDICES[] = {0, 1};

const EdfSignalSpec ANNOTATION_SIGNALS[] = {
    {"EDF Annotations", "", "-32768.0", "32767.00", "-32768", "32767",
     -32768, 32767, 31},
    {"Crc16", "", "-32768.0", "32767.00", "-32768", "32767",
     -32768, 32767, 1},
};

const EdfSignalSpec STR_SIGNALS[] = {
#include "edf_str_signal_table.inc"
};

const EdfFileSchema BRP_SCHEMA = {
    EdfFileKind::Brp,
    EdfSeriesId::Brp,
    "BRP",
    "EDF",
    BRP_SIGNALS,
    BRP_SOURCE_INDICES,
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
    PLD_SOURCE_INDICES,
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
    SA2_SOURCE_INDICES,
    sizeof(SA2_SIGNALS) / sizeof(SA2_SIGNALS[0]),
    AC_EDF_SA2_SIGNAL_COUNT,
    AC_EDF_SA2_SAMPLES_PER_RECORD,
    60,
};

constexpr uint16_t crc16_ccitt_false_table_value(size_t value) {
    uint16_t crc = static_cast<uint16_t>(value << 8);
    for (uint8_t bit = 0; bit < 8; ++bit) {
        crc = (crc & 0x8000)
                  ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                  : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

template <size_t... Index>
constexpr std::array<uint16_t, sizeof...(Index)>
make_crc16_ccitt_false_table(std::index_sequence<Index...>) {
    return {{crc16_ccitt_false_table_value(Index)...}};
}

constexpr auto CRC16_CCITT_FALSE_TABLE =
    make_crc16_ccitt_false_table(std::make_index_sequence<256>{});

uint16_t crc16_ccitt_false_impl(const uint8_t *data, size_t len) {
    uint16_t crc = 0xffff;
    if (!data && len) return crc;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t index =
            static_cast<uint8_t>((crc >> 8) ^ data[i]);
        crc = static_cast<uint16_t>(
            (crc << 8) ^ CRC16_CCITT_FALSE_TABLE[index]);
    }
    return crc;
}

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

int16_t encode_sample(const EdfSignalSpec &spec, float physical_value) {
    const int32_t digital_min = spec.digital_min_value;
    const int32_t digital_max = spec.digital_max_value;
    if (digital_max <= digital_min) {
        return static_cast<int16_t>(digital_min);
    }

    float scaled = physical_value * spec.physical_to_digital_scale +
                   spec.physical_to_digital_offset;
    if (spec.physical_to_digital_scale == 0.0f &&
        spec.physical_to_digital_offset == 0.0f) {
        const float physical_min = strtof(spec.physical_min, nullptr);
        const float physical_max = strtof(spec.physical_max, nullptr);
        if (physical_max <= physical_min) {
            return static_cast<int16_t>(digital_min);
        }
        scaled = static_cast<float>(digital_min) +
                 (physical_value - physical_min) *
                     static_cast<float>(digital_max - digital_min) /
                     (physical_max - physical_min);
    }

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

bool append_bytes(uint8_t *dst,
                  size_t capacity,
                  size_t &offset,
                  const char *text) {
    if (!dst || !text) return false;
    const size_t len = strlen(text);
    if (offset + len > capacity) return false;
    memcpy(dst + offset, text, len);
    offset += len;
    return true;
}

bool append_byte(uint8_t *dst,
                 size_t capacity,
                 size_t &offset,
                 uint8_t value) {
    if (!dst || offset >= capacity) return false;
    dst[offset++] = value;
    return true;
}

void finalize_patient_id(uint8_t *header, size_t header_size) {
    if (!header || header_size < EDF_SIGNAL_HEADER_OFFSET) return;

    memset(header + EDF_PATIENT_ID_OFFSET, ' ', EDF_PATIENT_ID_WIDTH);
    memcpy(header + EDF_PATIENT_ID_OFFSET, "X X X X 0000 0000", 17);

    const uint16_t first = crc16_ccitt_false_impl(
        header + EDF_PATIENT_FIRST_CRC_SOURCE_OFFSET,
        EDF_PATIENT_FIRST_CRC_SOURCE_LEN);
    const uint16_t second = crc16_ccitt_false_impl(
        header + EDF_SIGNAL_HEADER_OFFSET,
        header_size - EDF_SIGNAL_HEADER_OFFSET);

    char patient_id[24] = {};
    snprintf(patient_id, sizeof(patient_id), "X X X X %04X %04X",
             static_cast<unsigned>(first), static_cast<unsigned>(second));
    memcpy(header + EDF_PATIENT_ID_OFFSET, patient_id, strlen(patient_id));
}

bool render_header_common(const char *reserved,
                          const EdfSignalSpec *signals,
                          size_t signal_count,
                          const char *record_duration,
                          const EdfHeaderInfo &info,
                          uint8_t *dst,
                          size_t capacity,
                          size_t &written) {
    written = 0;
    const size_t required = 256 + signal_count * 256;
    if (!dst || capacity < required || !signals || signal_count == 0) {
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
    append_field(dst, capacity, offset, reserved, 44);
    append_u32_field(dst, capacity, offset, info.record_count, 8);
    append_field(dst, capacity, offset, record_duration, 8);
    append_u32_field(dst, capacity, offset,
                     static_cast<uint32_t>(signal_count), 4);

    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].label, 16);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, "", 80);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].physical_dimension, 8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].physical_min, 8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].physical_max, 8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].digital_min, 8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, signals[i].digital_max, 8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, "", 80);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_u32_field(dst, capacity, offset,
                         static_cast<uint32_t>(signals[i].samples_per_record),
                         8);
    }
    for (size_t i = 0; i < signal_count; ++i) {
        append_field(dst, capacity, offset, "", 32);
    }

    if (offset != required) return false;
    finalize_patient_id(dst, required);
    written = offset;
    return true;
}

const char *record_duration_text(uint32_t seconds) {
    switch (seconds) {
        case 60: return "60.00";
        case 86400: return "86400.00";
        default: return "0.00";
    }
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

const EdfSignalSpec *edf_str_signal_spec(size_t signal_index) {
    const size_t count = sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0]);
    if (signal_index >= count) return nullptr;
    return &STR_SIGNALS[signal_index];
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

size_t edf_str_header_size() {
    return 256 + (sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0])) * 256;
}

size_t edf_str_record_size() {
    size_t samples = 0;
    const size_t count = sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0]);
    for (size_t i = 0; i < count; ++i) {
        samples += STR_SIGNALS[i].samples_per_record;
    }
    return samples * 2;
}

size_t edf_str_signal_sample_offset(size_t signal_index) {
    const size_t count = sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0]);
    if (signal_index >= count) return AC_EDF_STR_SAMPLES_PER_RECORD;
    size_t offset = 0;
    for (size_t i = 0; i < signal_index; ++i) {
        offset += STR_SIGNALS[i].samples_per_record;
    }
    return offset;
}

size_t edf_annotation_header_size() {
    return 256 + sizeof(ANNOTATION_SIGNALS) / sizeof(ANNOTATION_SIGNALS[0]) *
                     256;
}

size_t edf_annotation_record_size() {
    return 64;
}

uint16_t edf_crc16_ccitt_false(const uint8_t *data, size_t len) {
    return crc16_ccitt_false_impl(data, len);
}

int16_t edf_encode_physical_sample(const EdfSignalSpec &spec,
                                   float physical_value) {
    return encode_sample(spec, physical_value);
}

bool edf_render_header(const EdfFileSchema &schema,
                       const EdfHeaderInfo &info,
                       uint8_t *dst,
                       size_t capacity,
                       size_t &written) {
    return render_header_common(
        schema.reserved,
        schema.signals,
        schema.signal_count,
        record_duration_text(schema.record_duration_seconds),
        info,
        dst,
        capacity,
        written);
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
        record.signal_count == 0 ||
        record.samples_per_record != schema.source_samples_per_record ||
        schema.signal_count != schema.source_signal_count + 1) {
        return false;
    }

    size_t offset = 0;
    for (size_t signal = 0; signal < schema.source_signal_count; ++signal) {
        const EdfSignalSpec &spec = schema.signals[signal];
        const size_t record_signal =
            schema.source_signal_indices ? schema.source_signal_indices[signal]
                                         : signal;
        if (record_signal >= record.signal_count) return false;
        for (size_t sample = 0; sample < schema.source_samples_per_record;
             ++sample) {
            const size_t source_index =
                record_signal * schema.source_samples_per_record + sample;
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

bool edf_render_str_header(const EdfHeaderInfo &info,
                           uint8_t *dst,
                           size_t capacity,
                           size_t &written) {
    return render_header_common("EDF",
                                STR_SIGNALS,
                                sizeof(STR_SIGNALS) / sizeof(STR_SIGNALS[0]),
                                "86400.00",
                                info,
                                dst,
                                capacity,
                                written);
}

bool edf_render_str_record(const EdfStrRecordView &record,
                           uint8_t *dst,
                           size_t capacity,
                           size_t &written) {
    written = 0;
    const size_t required = edf_str_record_size();
    if (!dst || capacity < required || !record.digital_samples ||
        record.sample_count != AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        return false;
    }

    size_t offset = 0;
    for (size_t i = 0; i < record.sample_count; ++i) {
        append_i16_le(dst, capacity, offset, record.digital_samples[i]);
    }

    const uint16_t crc = edf_crc16_ccitt_false(dst, offset);
    append_i16_le(dst, capacity, offset, static_cast<int16_t>(crc));
    if (offset != required) return false;
    written = offset;
    return true;
}

bool edf_render_annotation_header(const EdfHeaderInfo &info,
                                  uint8_t *dst,
                                  size_t capacity,
                                  size_t &written) {
    return render_header_common("EDF+D",
                                ANNOTATION_SIGNALS,
                                sizeof(ANNOTATION_SIGNALS) /
                                    sizeof(ANNOTATION_SIGNALS[0]),
                                "0.00",
                                info,
                                dst,
                                capacity,
                                written);
}

bool edf_render_annotation_record(const EdfAnnotationRecord &record,
                                  uint8_t *dst,
                                  size_t capacity,
                                  size_t &written) {
    written = 0;
    if (!dst || capacity < edf_annotation_record_size() || !record.label ||
        record.onset_seconds < 0 || record.duration_seconds < 0) {
        return false;
    }

    uint8_t payload[62] = {};
    size_t offset = 0;
    char tmp[20] = {};
    if (!append_bytes(payload, sizeof(payload), offset, "+0") ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_byte(payload, sizeof(payload), offset, 0x00)) {
        return false;
    }

    snprintf(tmp, sizeof(tmp), "+%ld", static_cast<long>(record.onset_seconds));
    if (!append_bytes(payload, sizeof(payload), offset, tmp) ||
        !append_byte(payload, sizeof(payload), offset, 0x15)) {
        return false;
    }
    snprintf(tmp, sizeof(tmp), "%ld", static_cast<long>(record.duration_seconds));
    if (!append_bytes(payload, sizeof(payload), offset, tmp) ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_bytes(payload, sizeof(payload), offset, record.label) ||
        !append_byte(payload, sizeof(payload), offset, 0x14) ||
        !append_byte(payload, sizeof(payload), offset, 0x00)) {
        return false;
    }

    memcpy(dst, payload, sizeof(payload));
    size_t out = sizeof(payload);
    const uint16_t crc = edf_crc16_ccitt_false(payload, sizeof(payload));
    append_i16_le(dst, capacity, out, static_cast<int16_t>(crc));
    if (out != edf_annotation_record_size()) return false;
    written = out;
    return true;
}

bool edf_render_recording_start_annotation(uint8_t *dst,
                                           size_t capacity,
                                           size_t &written) {
    EdfAnnotationRecord record;
    record.onset_seconds = 0;
    record.duration_seconds = 0;
    record.label = "Recording starts";
    return edf_render_annotation_record(record, dst, capacity, written);
}

bool edf_annotation_label_for_event(EdfAnnotationKind kind,
                                    const char *event_name,
                                    const char *&label) {
    label = nullptr;
    if (!event_name) return false;
    if (kind == EdfAnnotationKind::Eve) {
        if (strcmp(event_name, "Hypopnea") == 0) {
            label = "Hypopnea";
        } else if (strcmp(event_name, "CentralApnea") == 0) {
            label = "Central Apnea";
        } else if (strcmp(event_name, "ObstructiveApnea") == 0) {
            label = "Obstructive Apnea";
        } else if (strcmp(event_name, "Apnea") == 0) {
            label = "Apnea";
        } else if (strcmp(event_name, "Arousal") == 0 ||
                   strcmp(event_name, "Rera") == 0) {
            label = "Arousal";
        }
    } else if (kind == EdfAnnotationKind::Csl) {
        if (strcmp(event_name, "CsrStart") == 0) {
            label = "CSR Start";
        } else if (strcmp(event_name, "CsrEnd") == 0) {
            label = "CSR End";
        }
    }
    return label != nullptr;
}

}  // namespace aircannect
