#include "report_trash_cleanup.h"

#include <Arduino.h>

#include "report_store.h"

namespace aircannect {

void ReportTrashCleanup::service(bool realtime_active, bool report_busy) {
    if (realtime_active || report_busy) return;
    if (static_cast<int32_t>(millis() - next_cleanup_ms_) < 0) return;

    next_cleanup_ms_ = millis() + 250;

    uint32_t removed = 0;
    ReportStore::cleanup_trash_step(4, removed);
}

}  // namespace aircannect
