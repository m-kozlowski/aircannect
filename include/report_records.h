#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_spool_types.h"

namespace aircannect {

static constexpr uint32_t REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V1 = 1;
static constexpr uint32_t REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1 = 1;

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

size_t report_series_sample_wire_size();
size_t report_event_record_wire_size();

bool report_append_series_sample(ReportSpoolBuffer &out,
                                 const ReportSeriesSample &sample);
bool report_append_event_record(ReportSpoolBuffer &out,
                                const ReportEventRecord &event);
bool report_read_series_sample(const uint8_t *data,
                               size_t len,
                               size_t index,
                               ReportSeriesSample &sample);
bool report_read_event_record(const uint8_t *data,
                              size_t len,
                              size_t index,
                              ReportEventRecord &event);

}  // namespace aircannect
