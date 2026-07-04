#pragma once

#include <stddef.h>

#include "large_text_buffer.h"
#include "report_night_index.h"
#include "report_plot_range.h"
#include "report_result_types.h"

namespace aircannect {

void build_report_result_json_from(
    const ReportResultStatus &status,
    const ReportIndexedNight &indexed_night,
    const report_manager_internal::PlotRange *ranges,
    size_t range_count,
    const ReportResultStream *streams,
    size_t stream_count,
    const ReportCacheFetchStatus &cache_status,
    LargeTextBuffer &json);

}  // namespace aircannect
