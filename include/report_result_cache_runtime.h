#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
#include "report_result_cache_loader.h"
#include "report_result_cache_writer.h"
#include "report_result_slot_cache.h"

namespace aircannect {

class ReportResultRuntime;

class ReportResultCacheRuntime {
public:
    ReportResultCacheRuntime();

    bool begin();

    void apply_diagnostics(ReportResultStatus &status) const;

    bool publish_result(const ReportResultRuntime &result,
                        bool cache_plot = false);

    ReportResultSlotRead read_result(uint64_t night_start_ms,
                                     const char *etag,
                                     const char *if_none_match,
                                     LargeTextBuffer &json_out);

    ReportCachedPlotRead read_plot(
        uint64_t night_start_ms,
        const char *etag,
        std::shared_ptr<ReportSpoolBuffer> &out);

    ReportRangePlotRead read_or_request_range(
        size_t index,
        uint64_t night_start_ms,
        const char *etag,
        int64_t from_ms,
        int64_t to_ms,
        std::shared_ptr<ReportSpoolBuffer> &out);
    bool range_request_snapshot(ReportRangePlotRequest &out) const;
    void finish_range_request(size_t index,
                              uint64_t night_start_ms,
                              const char *etag,
                              int64_t from_ms,
                              int64_t to_ms,
                              const std::shared_ptr<ReportSpoolBuffer> &plot);
    void fail_range_request(size_t index,
                            uint64_t night_start_ms,
                            const char *etag,
                            int64_t from_ms,
                            int64_t to_ms);
    void reset_range(bool clear_ready);

    void invalidate(uint64_t night_start_ms, bool all);

    ReportCacheLoadRequest request_load(uint64_t night_start_ms,
                                        const char *etag,
                                        ReportCacheArtifact artifact);
    bool loader_active() const;
    bool service_loader();

    bool enqueue_write(const ReportIndexedNight &night,
                       const char *etag,
                       const std::shared_ptr<ReportSpoolBuffer> &result_json,
                       const std::shared_ptr<ReportSpoolBuffer> &plot);
    bool writer_active() const;
    bool service_writer();

private:
    bool ensure_slots();

    ReportResultSlotCache slots_;
    ReportResultCacheLoader loader_;
    ReportResultCacheWriter writer_;
};

}  // namespace aircannect
