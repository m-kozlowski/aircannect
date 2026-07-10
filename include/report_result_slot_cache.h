#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "edf_report_catalog.h"
#include "large_text_buffer.h"
#include "report_manager_internal_types.h"
#include "report_night_index.h"
#include "report_result_types.h"
#include "report_spool_types.h"

namespace aircannect {

enum class ReportResultSlotRead : uint8_t {
    NotFound,
    NotModified,
    Ready,
};

enum class ReportCachedPlotRead : uint8_t {
    NotFound,
    Error,
    Ready,
    ResultWithoutPlot,
};

enum class ReportRangePlotRead : uint8_t {
    Building,
    Empty,
    Error,
    Ready,
};

struct ReportRangePlotRequest {
    bool active = false;
    size_t index = 0;
    uint64_t night_start_ms = 0;
    int64_t from_ms = 0;
    int64_t to_ms = 0;
};

class ReportResultSlotCache {
public:
    using MaterializedResult = report_manager_internal::MaterializedResult;
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ~ReportResultSlotCache();

    bool ensure_slots();
    void apply_diagnostics(ReportResultStatus &status) const;

    bool publish(const ReportResultStatus &status,
                 const ReportIndexedNight &night,
                 const char *etag,
                 const ReportSessionRange *ranges,
                 size_t range_count,
                 const ReportResultStream *streams,
                 size_t stream_count,
                 const ReportResultChunk *chunks,
                 size_t chunk_count,
                 const EdfReportSessionDescriptor *edf_sessions,
                 size_t edf_session_count,
                 const std::shared_ptr<ReportSpoolBuffer> &plot);

    ReportResultSlotRead read_result(uint64_t night_start_ms,
                                     const char *etag,
                                     const char *if_none_match,
                                     LargeTextBuffer &json_out);

    ReportCachedPlotRead read_plot(
        uint64_t night_start_ms,
        const char *etag,
        std::shared_ptr<ReportSpoolBuffer> &out);
    bool attach_plot(uint64_t night_start_ms,
                     const char *etag,
                     const std::shared_ptr<ReportSpoolBuffer> &plot);

    ReportRangePlotRead read_or_request_range(
        size_t index,
        uint64_t night_start_ms,
        int64_t from_ms,
        int64_t to_ms,
        std::shared_ptr<ReportSpoolBuffer> &out);
    bool range_request_snapshot(ReportRangePlotRequest &out) const;
    void finish_range_request(size_t index,
                              uint64_t night_start_ms,
                              int64_t from_ms,
                              int64_t to_ms,
                              const std::shared_ptr<ReportSpoolBuffer> &plot);
    void fail_range_request(size_t index,
                            uint64_t night_start_ms,
                            int64_t from_ms,
                            int64_t to_ms);
    void reset_range(bool clear_ready);

    void invalidate(uint64_t night_start_ms, bool all);

private:
    bool ensure_lock();
    void clear_slot_locked(MaterializedResult &slot);
    void update_counts_locked();
    void clear_range_locked(uint64_t night_start_ms, bool all);

    MaterializedResult *slots_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
    uint32_t tick_ = 0;
    uint32_t materialized_slots_ = 0;
    uint32_t materialized_plot_slots_ = 0;

    bool range_req_active_ = false;
    size_t range_req_index_ = 0;
    uint64_t range_req_night_start_ms_ = 0;
    int64_t range_req_from_ = 0;
    int64_t range_req_to_ = 0;

    std::shared_ptr<ReportSpoolBuffer> range_plot_bytes_;
    size_t range_plot_index_ = 0;
    uint64_t range_plot_night_start_ms_ = 0;
    int64_t range_plot_from_ = 0;
    int64_t range_plot_to_ = 0;
};

}  // namespace aircannect
