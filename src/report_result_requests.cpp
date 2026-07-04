#include "report_manager.h"

namespace aircannect {

bool ReportManager::request_result_prepare_by_therapy_index(
    size_t therapy_index,
    bool refresh_cache) {
    return build_queue_service_.request_prepare_by_therapy_index(
        therapy_index,
        refresh_cache);
}

bool ReportManager::request_result_prepare_by_start(uint64_t night_start_ms,
                                                    bool refresh_cache) {
    return build_queue_service_.request_prepare_by_start(night_start_ms,
                                                         refresh_cache);
}

}  // namespace aircannect
