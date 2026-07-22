#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_spool_types.h"

namespace aircannect {

static constexpr uint32_t REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 = 2;
static constexpr uint32_t REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1 = 1;

enum class ReportEventCode : uint16_t {
    Hypopnea = 2,
    CentralApnea = 3,
    ObstructiveApnea = 4,
    UnclassifiedApnea = 5,
    Arousal = 6,
    Csr = 7,
};

constexpr uint16_t report_event_code_value(ReportEventCode code) {
    return static_cast<uint16_t>(code);
}

struct ReportSeriesSample {
    int64_t timestamp_ms = 0;
    int32_t value_milli = 0;
};

struct ReportEventRecord {
    int64_t start_ms = 0;
    int32_t duration_ms = 0;
    uint16_t code = 0;
    uint16_t flags = 0;
};

struct ReportSeriesV2UniformView {
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    const uint8_t *missing_bitmap = nullptr;
    size_t missing_bitmap_bytes = 0;
    const uint8_t *values_milli_le = nullptr;
    size_t values_milli_bytes = 0;
};

using ReportSeriesSampleCallback =
    bool (*)(void *context, const ReportSeriesSample &sample);

size_t report_series_v2_uniform_wire_size(uint32_t sample_count,
                                          size_t missing_bitmap_bytes);
size_t report_event_record_wire_size();

uint8_t report_event_source_mask(const ReportEventRecord &event);

bool report_append_event_record(ReportSpoolBuffer &out,
                                const ReportEventRecord &event);
bool report_event_overlaps_window(const ReportEventRecord &event,
                                  int64_t window_start_ms,
                                  int64_t window_end_ms,
                                  int64_t edge_tolerance_ms = 0);

bool report_build_series_payload_v2_uniform_values_le(
    ReportSpoolBuffer &out,
    uint32_t interval_ms,
    const uint8_t *values_milli_le,
    uint32_t sample_count,
    const uint8_t *missing_bitmap = nullptr,
    size_t missing_bitmap_bytes = 0);
bool report_series_payload_v2_uniform_view(
    const uint8_t *data,
    size_t len,
    uint32_t record_count,
    ReportSeriesV2UniformView &view);
size_t report_series_payload_v2_uniform_slice_size(
    const ReportSeriesV2UniformView &view,
    uint32_t first_sample,
    uint32_t sample_count);
bool report_write_series_payload_v2_uniform_slice(
    const ReportSeriesV2UniformView &view,
    uint32_t first_sample,
    uint32_t sample_count,
    uint8_t *out,
    size_t out_size);
bool report_for_each_series_sample(uint32_t payload_schema,
                                   int64_t chunk_start_ms,
                                   const uint8_t *data,
                                   size_t len,
                                   uint32_t record_count,
                                   ReportSeriesSampleCallback callback,
                                   void *context);
bool report_read_event_record(const uint8_t *data,
                              size_t len,
                              size_t index,
                              ReportEventRecord &event);

}  // namespace aircannect
