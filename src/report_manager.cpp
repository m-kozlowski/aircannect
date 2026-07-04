#include "report_manager.h"

namespace aircannect {

bool ReportManager::request_summary_refresh(bool force) {
    return summary_service_.request_refresh(force, cache_fetch_.active());
}

}  // namespace aircannect
