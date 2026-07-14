#include "report_result_cache_runtime.h"

#include <memory>

#include "debug_log.h"
#include "report_manager_helpers.h"
#include "report_result_response_json.h"
#include "report_result_runtime.h"

namespace aircannect {
namespace {

std::shared_ptr<ReportSpoolBuffer> make_result_json_snapshot(
    const ReportResultRuntime &result) {
    LargeTextBuffer text;
    if (!text.reserve(8192)) return nullptr;

    const ReportCacheFetchStatus inactive_cache{};
    build_report_result_json_from(result.status(),
                                  result.identity().indexed_night(),
                                  result.ranges().data(),
                                  result.ranges().count(),
                                  result.streams().data(),
                                  result.streams().count(),
                                  inactive_cache,
                                  text);
    if (text.overflowed() || text.length() == 0) return nullptr;

    auto snapshot = std::make_shared<ReportSpoolBuffer>();
    if (!snapshot) return nullptr;

    snapshot->set_max_size(text.length());
    if (!snapshot->reserve_capacity(text.length()) ||
        !snapshot->append(
            reinterpret_cast<const uint8_t *>(text.c_str()),
            text.length())) {
        return nullptr;
    }

    return snapshot;
}

}  // namespace

bool ReportResultCacheRuntime::begin() {
    return ensure_slots() && writer_.begin();
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

    const std::shared_ptr<ReportSpoolBuffer> result_json =
        make_result_json_snapshot(result);
    if (!result_json) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped: result JSON snapshot failed "
                  "index=%lu night=%llu\n",
                  static_cast<unsigned long>(status.therapy_index),
                  static_cast<unsigned long long>(
                      result.identity().indexed_night().summary.start_ms));
        return false;
    }

    if (!slots_.publish(status.state,
                        result.identity().indexed_night().summary.start_ms,
                        result.identity().etag(),
                        result_json,
                        plot)) {
        return false;
    }

    if (cache_plot && plot) {
        enqueue_write(result.identity().indexed_night(),
                      result.identity().etag(),
                      result_json,
                      plot);
    }

    return true;
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
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    return slots_.read_or_request_range(index,
                                        night_start_ms,
                                        etag,
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
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    slots_.finish_range_request(index,
                                night_start_ms,
                                etag,
                                from_ms,
                                to_ms,
                                plot);
}

void ReportResultCacheRuntime::fail_range_request(size_t index,
                                                  uint64_t night_start_ms,
                                                  const char *etag,
                                                  int64_t from_ms,
                                                  int64_t to_ms) {
    slots_.fail_range_request(index,
                              night_start_ms,
                              etag,
                              from_ms,
                              to_ms);
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
