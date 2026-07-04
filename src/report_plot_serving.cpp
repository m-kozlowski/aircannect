#include "report_manager.h"

namespace aircannect {

ReportManager::PlotRead ReportManager::read_plot(
    size_t therapy_index,
    const char *version,
    char *etag_out,
    size_t etag_out_size,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    return result_serving_.read_plot(therapy_index,
                                     version,
                                     etag_out,
                                     etag_out_size,
                                     out);
}

ReportManager::PlotRead ReportManager::read_plot_range(
    size_t therapy_index,
    const char *version,
    char *etag_out,
    size_t etag_out_size,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    return result_serving_.read_plot_range(therapy_index,
                                           version,
                                           etag_out,
                                           etag_out_size,
                                           from_ms,
                                           to_ms,
                                           out);
}

}  // namespace aircannect
