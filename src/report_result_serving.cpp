#include "report_manager.h"

namespace aircannect {

ReportManager::ResultRead ReportManager::read_result(
    size_t therapy_index,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    return result_serving_.read_result(therapy_index,
                                       if_none_match,
                                       etag_out,
                                       etag_out_size,
                                       json_out);
}

ReportManager::ResultRead ReportManager::read_result_by_start(
    uint64_t night_start_ms,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    return result_serving_.read_result_by_start(night_start_ms,
                                                if_none_match,
                                                etag_out,
                                                etag_out_size,
                                                json_out);
}

}  // namespace aircannect
