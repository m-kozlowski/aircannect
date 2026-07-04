#include "report_manager.h"

namespace aircannect {

bool ReportManager::service_cache_writer() {
    return cache_write_service_.service();
}

}  // namespace aircannect
