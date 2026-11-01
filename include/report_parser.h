#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_proto.h"
#include "report_sources.h"
#include "report_spool_types.h"

namespace aircannect {

enum class ReportParsedChunkKind : uint8_t {
    Series,
    Events,
};

struct ReportParsedChunk {
    ReportSourceId source = ReportSourceId::Summary;
    ReportParsedChunkKind kind = ReportParsedChunkKind::Series;
    const char *name = nullptr;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint32_t payload_schema = 0;
    uint32_t record_count = 0;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
};

using ReportParsedChunkCallback =
    bool (*)(void *context, const ReportParsedChunk &chunk);

enum class ReportSeriesReadResult : uint8_t {
    Chunk,
    Done,
    Failed,
};

// Input is borrowed until reset(); chunk.payload remains valid until output is reused.
class ReportSeriesSpoolReader {
public:
    bool begin(const ReportSpoolResult &result,
               ReportSourceId source,
               char *error,
               size_t error_len);
    ReportSeriesReadResult next(ReportParsedChunk &chunk,
                                ReportSpoolBuffer &output,
                                char *error,
                                size_t error_len);
    void reset();

private:
    ReportSeriesReadResult next_rc03(ReportParsedChunk &chunk,
                                     ReportSpoolBuffer &output,
                                     char *error,
                                     size_t error_len);
    ReportSeriesReadResult next_therapy(ReportParsedChunk &chunk,
                                        ReportSpoolBuffer &output,
                                        char *error,
                                        size_t error_len);

    const ReportSpoolResult *result_ = nullptr;
    ReportSourceId source_ = ReportSourceId::Summary;
    size_t cursor_ = 0;
    uint32_t expected_field_ = 0;
    const char *signal_name_ = nullptr;
    bool ready_ = false;
    bool failed_ = false;
    bool done_ = false;

    bool rc03_record_active_ = false;
    const uint8_t *rc03_body_ = nullptr;
    size_t rc03_body_len_ = 0;
    size_t rc03_bit_body_offset_ = 0;
    size_t rc03_bit_byte_index_ = 0;
    uint8_t rc03_bit_index_ = 0;
    uint64_t rc03_interval_ms_ = 0;
    uint64_t rc03_start_ms_ = 0;
    uint64_t rc03_sample_count_ = 0;
    uint64_t rc03_samples_decoded_ = 0;
    int32_t rc03_rice_m_ = 0;
    double rc03_scale_ = 1.0;
    int64_t rc03_prev2_ = 0;
    int64_t rc03_prev1_ = 0;

    bool therapy_wrapped_ = false;
    bool therapy_record_active_ = false;
    bool therapy_record_initialized_ = false;
    bool therapy_raw_pending_ = false;
    const uint8_t *therapy_record_data_ = nullptr;
    size_t therapy_record_len_ = 0;
    size_t therapy_signal_cursor_ = 0;
    uint64_t therapy_interval_ms_ = 60000;
};

bool report_validate_spool_for_source(const ReportSpoolResult &result,
                                      ReportSourceId source,
                                      char *error,
                                      size_t error_len);

bool report_parse_summary_spool(const ReportSpoolResult &result,
                                ReportSummaryRecordCallback callback,
                                void *context,
                                char *error,
                                size_t error_len,
                                size_t *rejected_records = nullptr);
bool report_parse_event_spool(const ReportSpoolResult &result,
                              ReportSourceId source,
                              ReportParsedChunkCallback callback,
                              void *context,
                              char *error,
                              size_t error_len);
bool report_parse_series_spool(const ReportSpoolResult &result,
                               ReportSourceId source,
                               ReportParsedChunkCallback callback,
                               void *context,
                               char *error,
                               size_t error_len);

}  // namespace aircannect
