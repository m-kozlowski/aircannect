#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_stream_assembler.h"

namespace aircannect {

enum class EdfFileKind : uint8_t {
    Brp,
    Pld,
    Sa2,
};

enum class EdfAnnotationKind : uint8_t {
    Eve,
    Csl,
};

static constexpr size_t AC_EDF_STR_SIGNAL_COUNT = 134;
static constexpr size_t AC_EDF_STR_SAMPLES_PER_RECORD = 172;
static constexpr size_t AC_EDF_STR_DATA_SAMPLES_PER_RECORD =
    AC_EDF_STR_SAMPLES_PER_RECORD - 1;
static constexpr size_t AC_EDF_STR_DATE_SIGNAL = 0;
static constexpr size_t AC_EDF_STR_MASK_ON_SIGNAL = 1;
static constexpr size_t AC_EDF_STR_MASK_OFF_SIGNAL = 2;
static constexpr size_t AC_EDF_STR_MASK_EVENTS_SIGNAL = 3;
static constexpr size_t AC_EDF_STR_DURATION_SIGNAL = 4;
static constexpr size_t AC_EDF_STR_CRC_SIGNAL = 133;
static constexpr size_t AC_EDF_STR_MASK_EVENT_CAPACITY = 20;
static constexpr size_t AC_EDF_NUMERIC_SIGNAL_MAX = AC_EDF_PLD_SIGNAL_COUNT;

struct EdfSignalSpec {
    const char *label = "";
    const char *physical_dimension = "";
    const char *physical_min = "";
    const char *physical_max = "";
    const char *digital_min = "";
    const char *digital_max = "";
    uint16_t digital_min_value = 0;
    uint16_t digital_max_value = 0;
    size_t samples_per_record = 0;
};

struct EdfFileSchema {
    EdfFileKind kind = EdfFileKind::Brp;
    EdfSeriesId series = EdfSeriesId::Brp;
    const char *suffix = "";
    const char *reserved = "EDF";
    const EdfSignalSpec *signals = nullptr;
    const uint8_t *source_signal_indices = nullptr;
    size_t signal_count = 0;
    size_t source_signal_count = 0;
    size_t source_samples_per_record = 0;
    uint32_t record_duration_seconds = 60;
};

struct EdfHeaderInfo {
    const char *patient_id = "";
    const char *recording_id = "";
    const char *start_date = "";
    const char *start_time = "";
    uint32_t record_count = 0;
};

struct EdfAnnotationRecord {
    int32_t onset_seconds = 0;
    int32_t duration_seconds = 0;
    const char *label = "";
};

struct EdfStrRecordView {
    const int16_t *digital_samples = nullptr;
    size_t sample_count = 0;
};

const EdfFileSchema &edf_numeric_schema(EdfFileKind kind);
const EdfFileSchema *edf_numeric_schema_for_series(EdfSeriesId series);
const EdfSignalSpec *edf_str_signal_spec(size_t signal_index);

size_t edf_header_size(const EdfFileSchema &schema);
size_t edf_record_size(const EdfFileSchema &schema);
size_t edf_str_header_size();
size_t edf_str_record_size();
size_t edf_str_signal_sample_offset(size_t signal_index);
size_t edf_annotation_header_size();
size_t edf_annotation_record_size();

uint16_t edf_crc16_ccitt_false(const uint8_t *data, size_t len);
int16_t edf_encode_physical_sample(const EdfSignalSpec &spec,
                                   float physical_value);

bool edf_render_header(const EdfFileSchema &schema,
                       const EdfHeaderInfo &info,
                       uint8_t *dst,
                       size_t capacity,
                       size_t &written);

bool edf_render_numeric_record(const EdfFileSchema &schema,
                               const EdfCompletedRecordView &record,
                               uint8_t *dst,
                               size_t capacity,
                               size_t &written);

bool edf_render_str_header(const EdfHeaderInfo &info,
                           uint8_t *dst,
                           size_t capacity,
                           size_t &written);

bool edf_render_str_record(const EdfStrRecordView &record,
                           uint8_t *dst,
                           size_t capacity,
                           size_t &written);

bool edf_render_annotation_header(const EdfHeaderInfo &info,
                                  uint8_t *dst,
                                  size_t capacity,
                                  size_t &written);

bool edf_render_annotation_record(const EdfAnnotationRecord &record,
                                  uint8_t *dst,
                                  size_t capacity,
                                  size_t &written);

bool edf_render_recording_start_annotation(uint8_t *dst,
                                           size_t capacity,
                                           size_t &written);

bool edf_annotation_label_for_event(EdfAnnotationKind kind,
                                    const char *event_name,
                                    const char *&label);

}  // namespace aircannect
