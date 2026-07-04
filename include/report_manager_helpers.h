#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_data_provider.h"
#include "report_manager_limits.h"
#include "report_result_types.h"
#include "report_summary_types.h"

namespace aircannect {
namespace report_manager_internal {

inline bool result_state_materialized_slot_allowed(ReportResultState state) {
    return state != ReportResultState::Preparing;
}

inline bool report_stream_bit(size_t stream_index, uint32_t &bit) {
    static_assert(AC_REPORT_RESULT_STREAM_MAX <= 32,
                  "Report stream masks are uint32_t");
    if (stream_index >= 32) return false;
    bit = 1u << static_cast<uint32_t>(stream_index);
    return true;
}

inline bool source_latest_cached_end_for_night(
    const ReportDataProvider &provider,
    const ReportSourceDef &source,
    const ReportSummaryRecord &night,
    int64_t &out_end_ms) {
    return provider.latest_cached_end(source,
                                      static_cast<int64_t>(night.start_ms),
                                      static_cast<int64_t>(night.start_ms),
                                      static_cast<int64_t>(night.end_ms),
                                      out_end_ms);
}

}  // namespace report_manager_internal
}  // namespace aircannect
