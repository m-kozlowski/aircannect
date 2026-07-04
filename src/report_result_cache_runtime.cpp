#include "report_result_cache_runtime.h"

#include <memory>

#include "debug_log.h"
#include "report_manager_helpers.h"
#include "report_result_response_json.h"
#include "report_result_runtime.h"

namespace aircannect {

bool ReportResultCacheRuntime::begin() {
    return writer_.begin();
}

bool ReportResultCacheRuntime::ensure_slots() {
    return slots_.ensure_slots();
}

void ReportResultCacheRuntime::apply_diagnostics(
    ReportResultStatus &status) const {
    slots_.apply_diagnostics(status);
}

bool ReportResultCacheRuntime::publish_result(
    const ReportResultRuntime &result,
    bool cache_plot) {
    const ReportResultStatus &status = result.status();

    if (!ensure_slots()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=slot_alloc_failed "
                  "index=%lu night=%llu state=%s error=%s\n",
                  static_cast<unsigned long>(status.therapy_index),
                  static_cast<unsigned long long>(
                      result.identity().indexed_night().summary.start_ms),
                  result.state_name(),
                  status.error.length() ? status.error.c_str() : "--");
        return false;
    }

    if (!report_manager_internal::result_state_materialized_slot_allowed(
            status.state)) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result publish skipped reason=state index=%lu night=%llu "
                  "state=%s error=%s\n",
                  static_cast<unsigned long>(status.therapy_index),
                  static_cast<unsigned long long>(
                      result.identity().indexed_night().summary.start_ms),
                  result.state_name(),
                  status.error.length() ? status.error.c_str() : "--");
        return false;
    }

    if (!result.identity().valid()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=%s index=%lu night=%llu "
                  "state=%s error=%s\n",
                  !result.identity().etag()[0] ? "missing_etag" :
                                                 "missing_night",
                  static_cast<unsigned long>(status.therapy_index),
                  static_cast<unsigned long long>(
                      result.identity().indexed_night().summary.start_ms),
                  result.state_name(),
                  status.error.length() ? status.error.c_str() : "--");
        return false;
    }

    std::shared_ptr<ReportSpoolBuffer> plot;
    if (result.plot().result_bin.size() > 0) {
        plot = std::make_shared<ReportSpoolBuffer>();
        plot->set_max_size(result.plot().result_bin.size());

        if (!plot->reserve_capacity(result.plot().result_bin.size()) ||
            !plot->append(result.plot().result_bin.data(),
                          result.plot().result_bin.size())) {
            plot.reset();
        }
    }

    if (result.plot().result_bin.size() > 0 && !plot) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped: plot snapshot failed "
                  "index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(status.therapy_index),
                  static_cast<unsigned long>(result.plot().result_bin.size()));
        return false;
    }

    if (!publish(status,
                 result.identity().indexed_night(),
                 result.identity().etag(),
                 result.ranges().data(),
                 result.ranges().count(),
                 result.streams().data(),
                 result.streams().count(),
                 result.scratch().chunks(),
                 status.chunk_count,
                 result.scratch().edf_sessions(),
                 result.scratch().edf_session_count(),
                 plot)) {
        return false;
    }

    if (cache_plot && plot) {
        LargeTextBuffer result_json_text;
        result_json_text.reserve(8192);

        const ReportCacheFetchStatus inactive_cache{};
        build_report_result_json_from(status,
                                      result.identity().indexed_night(),
                                      result.ranges().data(),
                                      result.ranges().count(),
                                      result.streams().data(),
                                      result.streams().count(),
                                      inactive_cache,
                                      result_json_text);

        std::shared_ptr<ReportSpoolBuffer> result_json;
        if (!result_json_text.overflowed() && result_json_text.length() > 0) {
            result_json = std::make_shared<ReportSpoolBuffer>();
            if (result_json) {
                result_json->set_max_size(result_json_text.length());
                if (!result_json->reserve_capacity(result_json_text.length()) ||
                    !result_json->append(
                        reinterpret_cast<const uint8_t *>(
                            result_json_text.c_str()),
                        result_json_text.length())) {
                    result_json.reset();
                }
            }
        }

        if (result_json) {
            enqueue_write(result.identity().indexed_night(),
                          result.identity().etag(),
                          result_json,
                          plot);
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result cache write skipped: result JSON snapshot "
                      "failed index=%lu night=%llu\n",
                      static_cast<unsigned long>(status.therapy_index),
                      static_cast<unsigned long long>(
                          result.identity().indexed_night().summary.start_ms));
        }
    }

    return true;
}

bool ReportResultCacheRuntime::publish(
    const ReportResultStatus &status,
    const ReportIndexedNight &night,
    const char *etag,
    const ReportResultSlotCache::PlotRange *ranges,
    size_t range_count,
    const ReportResultStream *streams,
    size_t stream_count,
    const ReportResultSlotCache::ReportResultChunk *chunks,
    size_t chunk_count,
    const EdfReportSessionDescriptor *edf_sessions,
    size_t edf_session_count,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    return slots_.publish(status,
                          night,
                          etag,
                          ranges,
                          range_count,
                          streams,
                          stream_count,
                          chunks,
                          chunk_count,
                          edf_sessions,
                          edf_session_count,
                          plot);
}

ReportResultSlotRead ReportResultCacheRuntime::read_result(
    uint64_t night_start_ms,
    const char *etag,
    const char *if_none_match,
    LargeTextBuffer &json_out) {
    return slots_.read_result(night_start_ms, etag, if_none_match, json_out);
}

ReportCachedPlotRead ReportResultCacheRuntime::read_plot(
    uint64_t night_start_ms,
    const char *etag,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    return slots_.read_plot(night_start_ms, etag, out);
}

bool ReportResultCacheRuntime::attach_plot(
    uint64_t night_start_ms,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    return slots_.attach_plot(night_start_ms, etag, plot);
}

ReportRangePlotRead ReportResultCacheRuntime::read_or_request_range(
    size_t index,
    uint64_t night_start_ms,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    return slots_.read_or_request_range(index,
                                        night_start_ms,
                                        from_ms,
                                        to_ms,
                                        out);
}

bool ReportResultCacheRuntime::range_request_snapshot(
    ReportRangePlotRequest &out) const {
    return slots_.range_request_snapshot(out);
}

void ReportResultCacheRuntime::finish_range_request(
    size_t index,
    uint64_t night_start_ms,
    int64_t from_ms,
    int64_t to_ms,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    slots_.finish_range_request(index, night_start_ms, from_ms, to_ms, plot);
}

void ReportResultCacheRuntime::fail_range_request(size_t index,
                                                  uint64_t night_start_ms,
                                                  int64_t from_ms,
                                                  int64_t to_ms) {
    slots_.fail_range_request(index, night_start_ms, from_ms, to_ms);
}

void ReportResultCacheRuntime::reset_range(bool clear_ready) {
    slots_.reset_range(clear_ready);
}

void ReportResultCacheRuntime::invalidate(uint64_t night_start_ms, bool all) {
    slots_.invalidate(night_start_ms, all);
}

bool ReportResultCacheRuntime::enqueue_write(
    const ReportIndexedNight &night,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &result_json,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    return writer_.enqueue(night, etag, result_json, plot);
}

bool ReportResultCacheRuntime::writer_active() const {
    return writer_.active();
}

bool ReportResultCacheRuntime::service_writer() {
    return writer_.service();
}

}  // namespace aircannect
