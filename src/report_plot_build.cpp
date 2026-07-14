#include "report_result_plot_builder.h"

#include <algorithm>
#include <stdint.h>

#include "board.h"
#include "debug_log.h"
#include "report_event_dedupe.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_result_cache_runtime.h"
#include "report_result_cache_files.h"
#include "report_result_provider_bridge.h"
#include "report_result_runtime.h"
#include "report_store.h"

namespace aircannect {

ReportResultPlotBuilder::ReportResultPlotBuilder(ReportResultRuntime &result,
                                                 ReportResultCacheRuntime &cache)
    : result_(result),
      cache_(cache) {}

bool ReportResultPlotBuilder::active() const {
    return result_.plot().active;
}

bool ReportResultPlotBuilder::idle_prebuild_active() const {
    return result_.plot().active && result_.plot().idle_prebuild;
}

uint32_t ReportResultPlotBuilder::elapsed_ms(uint32_t now_ms) const {
    return result_.plot().elapsed_ms(now_ms);
}

uint64_t ReportResultPlotBuilder::night_start_ms() const {
    return result_.plot().night_start_ms.load();
}

void ReportResultPlotBuilder::reset() {
    edf_batch_.reset();
    result_.plot().reset();
}

void ReportResultPlotBuilder::abort_idle_prebuild(const char *reason) {
    if (!idle_prebuild_active()) return;

    const uint32_t elapsed = elapsed_ms(millis());

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Idle plot prebuild aborted reason=%s "
              "night=%llu elapsed_ms=%lu\n",
              reason ? reason : "gate",
              static_cast<unsigned long long>(night_start_ms()),
              static_cast<unsigned long>(elapsed));

    reset();
    result_.release_edf_sessions();
}

void ReportResultPlotBuilder::preempt_idle_prebuild() {
    if (!idle_prebuild_active()) return;

    const uint32_t elapsed = elapsed_ms(millis());

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Idle plot prebuild preempted night=%llu elapsed_ms=%lu\n",
              static_cast<unsigned long long>(night_start_ms()),
              static_cast<unsigned long>(elapsed));

    reset();
    result_.release_edf_sessions();
}

void ReportResultPlotBuilder::preempt_for_range(size_t therapy_index,
                                                uint64_t night_start) {
    if (!active()) return;

    const uint32_t elapsed = elapsed_ms(millis());

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Range plot preempted overview index=%lu "
              "night=%llu elapsed_ms=%lu\n",
              static_cast<unsigned long>(therapy_index),
              static_cast<unsigned long long>(night_start),
              static_cast<unsigned long>(elapsed));

    reset();
    result_.release_edf_sessions();
}

bool ReportResultPlotBuilder::start() {
    reset();
    report_build_empty_plot_bin(result_.plot().result_bin);
    if (result_.status().state == ReportResultState::Error ||
        !result_.scratch().chunks() || result_.status().chunk_count == 0) {
        report_build_empty_plot_bin(result_.plot().result_bin);
        return cache_.publish_result(result_);
    }

    const size_t range_count =
        std::min(result_.ranges().count(),
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX));
    if (range_count == 0) {
        report_build_empty_plot_bin(result_.plot().result_bin);
        return cache_.publish_result(result_);
    }

    result_.plot().ranges = result_.ranges().data();
    result_.plot().range_count = range_count;
    result_.plot().start_ms = result_.ranges()[0].start_ms;
    result_.plot().end_ms = result_.ranges()[0].end_ms;
    for (size_t i = 0; i < range_count; ++i) {
        result_.plot().start_ms =
            std::min(result_.plot().start_ms, result_.ranges()[i].start_ms);
        result_.plot().end_ms =
            std::max(result_.plot().end_ms, result_.ranges()[i].end_ms);
    }

    if (result_.plot().start_ms <= 0 ||
        result_.plot().end_ms <= result_.plot().start_ms) {
        report_build_empty_plot_bin(result_.plot().result_bin);
        return cache_.publish_result(result_);
    }

    if (!result_.plot().skip_cache &&
        load_result_plot_cache_for_etag(
            result_.identity().summary().start_ms,
            result_.identity().etag(),
            result_.plot().result_bin)) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache hit index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_.status().therapy_index),
                  static_cast<unsigned long>(result_.plot().result_bin.size()));
        return cache_.publish_result(result_);
    }

    if (result_.plot().skip_cache) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache skipped index=%lu reason=refresh\n",
                  static_cast<unsigned long>(result_.status().therapy_index));
    }

    const int64_t span_ms = result_.plot().end_ms - result_.plot().start_ms;
    result_.plot().bucket_ms = std::max<int64_t>(
        1, span_ms / static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS));
    result_.plot().build_bin.clear();
    result_.plot().build_bin.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    result_.plot().tmp.clear();
    result_.plot().tmp.set_max_size(128 * 1024);
    result_.plot().ok = true;
    if (!result_.plot().build_bin.reserve_capacity(
            AC_REPORT_PLOT_INITIAL_RESERVE)) {
        fail("plot_alloc_failed");
        return false;
    }

    result_.plot().ok &= bin_put_u32(result_.plot().build_bin, PLOT_BIN_MAGIC);
    result_.plot().ok &= bin_put_u16(result_.plot().build_bin, PLOT_BIN_VERSION);
    result_.plot().ok &= bin_put_u16(result_.plot().build_bin, 0);  // flags
    result_.plot().ok &= bin_put_i64(result_.plot().build_bin,
                                     result_.plot().start_ms);  // base_ms
    if (!result_.plot().ok) {
        fail("plot_alloc_failed");
        return false;
    }

    result_.plot().active = true;
    result_.plot().idle_prebuild = result_.plot().active_idle_prebuild;
    result_.plot().started_ms = millis();
    result_.plot().input_chunks = 0;
    result_.plot().input_bytes = 0;
    result_.plot().night_start_ms.store(result_.identity().summary().start_ms);
    result_.plot().phase = ReportPlotBuildPhase::Events;
    return true;
}

bool ReportResultPlotBuilder::process_event_chunk(const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!report_read_result_chunk_payload(
            chunk,
            static_cast<int64_t>(
                result_.identity().summary().start_ms),
            result_.scratch().edf_sessions(),
            result_.scratch().edf_session_count(),
            meta,
            payload)) {
        fail("plot_event_read_failed");
        return false;
    }
    const size_t count = payload.size() / report_event_record_wire_size();
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        bool in_range = false;
        for (size_t i = 0; i < result_.plot().range_count; ++i) {
            if (report_event_overlaps_window(
                    event,
                    result_.plot().ranges[i].start_ms,
                    result_.plot().ranges[i].end_ms,
                    AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
                in_range = true;
                break;
            }
        }
        if (!in_range) continue;

        if (report_event_seen(result_.plot().seen_events, event)) continue;
        if (!remember_report_event(result_.plot().seen_events, event)) {
            fail("plot_event_dedupe_failed");
            return false;
        }

        result_.plot().ok &= bin_put_i32(
            result_.plot().tmp,
            static_cast<int32_t>(event.start_ms - result_.plot().start_ms));
        result_.plot().ok &= bin_put_i32(
            result_.plot().tmp, static_cast<int32_t>(event.duration_ms));
        result_.plot().ok &= bin_put_i32(result_.plot().tmp,
                                         static_cast<int32_t>(event.code));
        result_.plot().ok &= bin_put_i32(result_.plot().tmp,
                                         static_cast<int32_t>(event.flags));
    }
    return true;
}

bool ReportResultPlotBuilder::finish() {
    if (!result_.plot().ok || result_.plot().build_bin.size() == 0) {
        fail("plot_overflow");
        return false;
    }

    const size_t len = result_.plot().build_bin.size();
    result_.plot().result_bin.clear();
    result_.plot().result_bin.set_max_size(len);
    if (!result_.plot().result_bin.reserve_capacity(len) ||
        !result_.plot().result_bin.append(result_.plot().build_bin.data(), len)) {
        fail("plot_publish_failed");
        return false;
    }

    result_.status().state =
        report_result_settled_state(result_.status().missing_required);
    result_.status().error.clear();
    if (!cache_.publish_result(result_, true)) {
        reset();
        result_.release_edf_sessions();
        return false;
    }

    const uint32_t elapsed = result_.plot().elapsed_ms(millis());
    const uint32_t input_chunks = result_.plot().input_chunks;
    const uint32_t input_bytes = result_.plot().input_bytes;

    reset();
    result_.release_edf_sessions();

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result plot ready index=%lu chunks=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(result_.status().therapy_index),
              static_cast<unsigned long>(result_.status().chunk_count),
              static_cast<unsigned long>(input_chunks),
              static_cast<unsigned long>(input_bytes),
              static_cast<unsigned long>(result_.plot().result_bin.size()),
              static_cast<unsigned long>(elapsed));
    return true;
}

void ReportResultPlotBuilder::fail(const char *message) {
    reset();
    result_.mark_error(message);

    if (result_.identity().night_start_ms() != 0 &&
        result_.status().night_start_ms != 0) {
        cache_.publish_result(result_);
    }

    result_.release_edf_sessions();
    Log::logf(CAT_REPORT, LOG_WARN, "Result prepare failed: %s\n",
              result_.status().error.c_str());
}

}  // namespace aircannect
