#include "report_night_index_lookup.h"

namespace aircannect {

ReportNightIndexLookupResult classify_report_night_index_lookup(
    ReportNightIndexSnapshotResult snapshot_result,
    bool snapshot_available,
    bool found) {
    switch (snapshot_result) {
        case ReportNightIndexSnapshotResult::Busy:
            return ReportNightIndexLookupResult::Busy;
        case ReportNightIndexSnapshotResult::Failed:
            return ReportNightIndexLookupResult::Failed;
        case ReportNightIndexSnapshotResult::Ready:
            break;
    }

    if (!snapshot_available) return ReportNightIndexLookupResult::Failed;
    return found ? ReportNightIndexLookupResult::Ready
                 : ReportNightIndexLookupResult::NotFound;
}

}  // namespace aircannect
