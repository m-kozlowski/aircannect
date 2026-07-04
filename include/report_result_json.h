#pragma once

#include <stddef.h>

#include "large_text_buffer.h"
#include "report_result_types.h"

namespace aircannect {

const char *report_result_state_name(ReportResultState state);

void report_append_result_cache_json(
    LargeTextBuffer &json,
    const ReportCacheFetchStatus &cache_status);

void report_append_result_metrics_json(
    LargeTextBuffer &json,
    const ReportResultStatus &status);

void report_append_result_stream_details_json(
    LargeTextBuffer &json,
    const ReportResultStream *streams,
    size_t stream_count);

}  // namespace aircannect
