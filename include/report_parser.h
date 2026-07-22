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

bool report_validate_spool_for_source(const ReportSpoolResult &result,
                                      ReportSourceId source,
                                      char *error,
                                      size_t error_len);

bool report_parse_summary_spool(const ReportSpoolResult &result,
                                ReportSummaryRecordCallback callback,
                                void *context,
                                char *error,
                                size_t error_len);
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
