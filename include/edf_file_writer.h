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

const EdfFileSchema &edf_numeric_schema(EdfFileKind kind);
const EdfFileSchema *edf_numeric_schema_for_series(EdfSeriesId series);

size_t edf_header_size(const EdfFileSchema &schema);
size_t edf_record_size(const EdfFileSchema &schema);

uint16_t edf_crc16_ccitt_false(const uint8_t *data, size_t len);

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

}  // namespace aircannect
