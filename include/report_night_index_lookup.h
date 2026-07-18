#pragma once

#include <stdint.h>

namespace aircannect {

enum class ReportNightIndexSnapshotResult : uint8_t {
    Ready,
    Busy,
    Failed,
};

enum class ReportNightIndexLookupResult : uint8_t {
    Ready,
    NotFound,
    Busy,
    Failed,
};

ReportNightIndexLookupResult classify_report_night_index_lookup(
    ReportNightIndexSnapshotResult snapshot_result,
    bool snapshot_available,
    bool found);

}  // namespace aircannect
