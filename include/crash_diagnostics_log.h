#pragma once

#include <string.h>

#include "crash_diagnostics_types.h"

namespace aircannect {
namespace CrashDiagnosticsDetail {

struct AvailableDumpCorrelation {
    const char *occurred_at = "--";
    const char *reason = "panic";
    bool emit_breadcrumb = false;
};

inline AvailableDumpCorrelation correlate_available_dump(
    const CrashDiagnosticsSnapshot &snapshot) {
    AvailableDumpCorrelation result;
    const bool current = snapshot.dump_relation == CrashDumpRelation::Current;
    const bool task_watchdog =
        strstr(snapshot.reason, "Task watchdog") != nullptr ||
        (current && snapshot.rtc_task_watchdog);
    result.occurred_at = current && snapshot.occurred_at[0]
                             ? snapshot.occurred_at
                             : "--";
    result.reason = task_watchdog
                        ? "task_watchdog"
                        : (snapshot.reason[0] ? snapshot.reason : "panic");
    result.emit_breadcrumb = snapshot.rtc_panic_available && !current;
    return result;
}

}  // namespace CrashDiagnosticsDetail
}  // namespace aircannect
