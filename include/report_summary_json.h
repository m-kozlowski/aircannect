#pragma once

#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
#include "report_night_index.h"
#include "report_night_index_snapshot.h"
#include "report_summary_types.h"

namespace aircannect {

const char *report_summary_state_name(ReportSummaryState state);
uint32_t report_indexed_night_display_duration_min(
    const ReportIndexedNight &night);
bool report_indexed_night_visible_in_summary(
    const ReportIndexedNight &night);
bool report_append_summary_json_from_snapshot(
    LargeTextBuffer &json,
    const ReportSummaryStatus &status,
    uint32_t data_epoch,
    const ReportNightIndexSnapshot &snapshot);

}  // namespace aircannect
