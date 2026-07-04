#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
#include "report_build_runtime.h"
#include "report_night_index.h"
#include "report_night_index_service.h"
#include "report_result_cache_runtime.h"
#include "report_result_runtime.h"
#include "report_spool_types.h"

namespace aircannect {

enum class ReportResultRead : uint8_t {
    NotFound,
    NotModified,
    Ready,
    Building,
    QueueFull,
    Unavailable,
    Busy,
};

enum class ReportPlotRead : uint8_t {
    NotFound,
    Ready,
    Error,
    Building,
    Stale,
    Empty,
    QueueFull,
    Unavailable,
    Busy,
};

class ReportResultServingService {
public:
    ReportResultServingService(ReportNightIndexService &night_index,
                               ReportBuildRuntime &build,
                               ReportResultCacheRuntime &cache,
                               ReportResultRuntime &result);

    ReportResultRead read_result(size_t therapy_index,
                                 const char *if_none_match,
                                 char *etag_out,
                                 size_t etag_out_size,
                                 LargeTextBuffer &json_out);
    ReportResultRead read_result_by_start(uint64_t night_start_ms,
                                          const char *if_none_match,
                                          char *etag_out,
                                          size_t etag_out_size,
                                          LargeTextBuffer &json_out);

    ReportPlotRead read_plot(size_t therapy_index,
                             const char *version,
                             char *etag_out,
                             size_t etag_out_size,
                             std::shared_ptr<ReportSpoolBuffer> &out);
    ReportPlotRead read_plot_range(size_t therapy_index,
                                   const char *version,
                                   char *etag_out,
                                   size_t etag_out_size,
                                   int64_t from_ms,
                                   int64_t to_ms,
                                   std::shared_ptr<ReportSpoolBuffer> &out);

private:
    ReportResultRead read_result_for_indexed_night(
        size_t therapy_index,
        const ReportIndexedNight &indexed_night,
        const char *if_none_match,
        char *etag_out,
        size_t etag_out_size,
        LargeTextBuffer &json_out);
    bool resolve_plot_night(size_t therapy_index,
                            const char *version,
                            ReportIndexedNight &indexed_night,
                            size_t &resolved_therapy_index,
                            char *etag_out,
                            size_t etag_out_size);

    ReportNightIndexService &night_index_;
    ReportBuildRuntime &build_;
    ReportResultCacheRuntime &cache_;
    ReportResultRuntime &result_;
};

}  // namespace aircannect
