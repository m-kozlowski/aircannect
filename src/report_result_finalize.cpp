#include "report_result_build_service.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include "debug_log.h"
#include "report_event_dedupe.h"
#include "report_records.h"
#include "report_result_metrics.h"
#include "report_result_provider_bridge.h"

namespace aircannect {

bool ReportResultBuildService::count_events_from_chunks() {
    const size_t range_count =
        std::min(runtime_.ranges().count(),
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    runtime_.status().oa_count = 0;
    runtime_.status().ca_count = 0;
    runtime_.status().ua_count = 0;
    runtime_.status().hypopnea_count = 0;
    runtime_.status().arousal_count = 0;
    ReportSpoolBuffer counted_events;
    counted_events.set_max_size(64 * 1024);
    for (uint32_t i = 0; i < runtime_.status().chunk_count; ++i) {
        const ReportResultChunk &chunk = runtime_.scratch().chunks()[i];
        if (chunk.kind != ReportStoreChunkKind::Events) continue;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
        if (!report_read_result_chunk_payload(
                chunk,
                static_cast<int64_t>(
                    runtime_.identity().summary().start_ms),
                runtime_.scratch().edf_sessions(),
                runtime_.scratch().edf_session_count(),
                meta,
                payload)) {
            fail_prepare("event_chunk_read_failed");
            return false;
        }
        const size_t count =
            payload.size() / report_event_record_wire_size();
        for (size_t index = 0; index < count; ++index) {
            ReportEventRecord event;
            if (!report_read_event_record(payload.data(),
                                          payload.size(),
                                          index,
                                          event)) {
                continue;
            }
            bool overlaps_result_range = false;
            for (size_t range_index = 0; range_index < range_count;
                 ++range_index) {
                const PlotRange &range = runtime_.ranges()[range_index];
                if (report_event_overlaps_window(
                        event,
                        range.start_ms,
                        range.end_ms,
                        AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
                    overlaps_result_range = true;
                    break;
                }
            }
            if (!overlaps_result_range) {
                continue;
            }
            if (report_event_seen(counted_events, event)) continue;
            if (!remember_report_event(counted_events, event)) {
                fail_prepare("event_dedupe_failed");
                return false;
            }
            switch (event.code) {
                case report_event_code_value(ReportEventCode::Hypopnea):
                    runtime_.status().hypopnea_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::CentralApnea):
                    runtime_.status().ca_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::ObstructiveApnea):
                    runtime_.status().oa_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::UnclassifiedApnea):
                    runtime_.status().ua_count++;
                    break;
                case report_event_code_value(ReportEventCode::Arousal):
                    runtime_.status().arousal_count++;
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

void ReportResultBuildService::apply_event_indices_from_counts() {
    if (runtime_.status().duration_min <= 0) return;
    if (!runtime_.status().events_available) return;

    const float hours =
        static_cast<float>(runtime_.status().duration_min) / 60.0f;
    if (hours <= 0.0f) return;

    const float oa_index =
        static_cast<float>(runtime_.status().oa_count) / hours;
    const float ca_index =
        static_cast<float>(runtime_.status().ca_count) / hours;
    const float ua_index =
        static_cast<float>(runtime_.status().ua_count) / hours;
    const float hypopnea_index =
        static_cast<float>(runtime_.status().hypopnea_count) / hours;
    const float arousal_index =
        static_cast<float>(runtime_.status().arousal_count) / hours;

    if (!runtime_.status().oa_index_valid) {
        runtime_.status().oa_index = oa_index;
        runtime_.status().oa_index_valid = true;
        runtime_.status().oa_index_source = ReportMetricSource::Calculated;
    }
    if (!runtime_.status().ca_index_valid) {
        runtime_.status().ca_index = ca_index;
        runtime_.status().ca_index_valid = true;
        runtime_.status().ca_index_source = ReportMetricSource::Calculated;
    }
    if (!runtime_.status().ua_index_valid) {
        runtime_.status().ua_index = ua_index;
        runtime_.status().ua_index_valid = true;
        runtime_.status().ua_index_source = ReportMetricSource::Calculated;
    }
    if (!runtime_.status().hypopnea_index_valid) {
        runtime_.status().hypopnea_index = hypopnea_index;
        runtime_.status().hypopnea_index_valid = true;
        runtime_.status().hypopnea_index_source =
            ReportMetricSource::Calculated;
    }
    if (!runtime_.status().arousal_index_valid) {
        runtime_.status().arousal_index = arousal_index;
        runtime_.status().arousal_index_valid = true;
        runtime_.status().arousal_index_source = ReportMetricSource::Calculated;
    }
    if (!runtime_.status().ahi_valid) {
        runtime_.status().ahi =
            oa_index + ca_index + ua_index + hypopnea_index;
        runtime_.status().ahi_valid = true;
        runtime_.status().ahi_source = ReportMetricSource::Calculated;
    }
}

bool ReportResultBuildService::apply_series_metrics_from_chunks() {
    ReportMetricAverage pressure_average;
    ReportMetricAverage mask_pressure_average;
    ReportMetricAverage leak_average;

    struct MetricSeriesContext {
        ReportResultBuildService *builder = nullptr;
        ReportMetricAverage *average = nullptr;
        float multiplier = 1.0f;
    };

    for (uint32_t chunk_index = 0; chunk_index < runtime_.status().chunk_count;
         ++chunk_index) {
        const ReportResultChunk &chunk = runtime_.scratch().chunks()[chunk_index];
        if (chunk.kind != ReportStoreChunkKind::Series) continue;
        for (size_t stream_index = 0; stream_index < runtime_.streams().count();
             ++stream_index) {
            if (!report_result_chunk_has_stream(chunk, stream_index)) continue;
            const ReportResultStream &stream = runtime_.streams()[stream_index];
            MetricSeriesContext ctx;
            ctx.builder = this;
            if (stream.signal == ReportSignalId::InspiratoryPressure) {
                ctx.average = &pressure_average;
                ctx.multiplier = 1.0f;
            } else if (stream.signal == ReportSignalId::MaskPressure) {
                ctx.average = &mask_pressure_average;
                ctx.multiplier = 1.0f;
            } else if (stream.signal == ReportSignalId::Leak) {
                ctx.average = &leak_average;
                ctx.multiplier = 60.0f;
            } else {
                continue;
            }

            ReportProviderSeriesReadStats stats;
            const bool ok = report_for_each_result_series_sample(
                chunk,
                stream_index,
                runtime_.streams().data(),
                runtime_.streams().count(),
                runtime_.scratch().edf_sessions(),
                runtime_.scratch().edf_session_count(),
                static_cast<int64_t>(
                    runtime_.identity().summary().start_ms),
                stats,
                [](void *context, const ReportSeriesSample &sample) -> bool {
                    MetricSeriesContext *ctx =
                        static_cast<MetricSeriesContext *>(context);
                    if (!ctx || !ctx->builder || !ctx->average) return false;
                    if (!ctx->builder->runtime_.ranges().contains_timestamp(
                            sample.timestamp_ms)) {
                        return true;
                    }
                    const float value =
                        (static_cast<float>(sample.value_milli) / 1000.0f) *
                        ctx->multiplier;
                    ctx->average->add(value);
                    return true;
                },
                &ctx);
            (void)stats;
            if (!ok) continue;
        }
    }

    float value = 0.0f;
    if (!pressure_average.mean(value)) {
        (void)mask_pressure_average.mean(value);
    }
    if (isfinite(value) &&
        (pressure_average.count > 0 || mask_pressure_average.count > 0)) {
        runtime_.status().mask_pressure_50_cm_h2o = value;
        runtime_.status().mask_pressure_50_valid = true;
        runtime_.status().mask_pressure_50_source = ReportMetricSource::Calculated;
    }
    if (leak_average.mean(value)) {
        runtime_.status().leak_50_l_min = value;
        runtime_.status().leak_50_valid = true;
        runtime_.status().leak_50_source = ReportMetricSource::Calculated;
    }
    return true;
}

bool ReportResultBuildService::finalize_prepare(size_t therapy_index) {
    ReportResultRuntime &result = runtime_;
    ReportResultStatus &status = result.status();

    if (status.state == ReportResultState::Error) return false;

    // Build a best-effort plot from whatever is cached: aged-out signals leave
    // missing_streams>0 and not-yet-swept sources leave missing_required>0, but
    // both are reported for the UI to mark - they do not block rendering. Only
    // a night with nothing cached at all (background hasn't reached it) has no
    // plot to show.
    if (status.chunk_count == 0) {
        status.state = ReportResultState::Incomplete;
        status.error = "not_cached";
        if (!cache_.publish_result(result)) return false;
    } else {
        std::sort(result.scratch().chunks(),
                  result.scratch().chunks() + status.chunk_count,
                  [](const ReportResultChunk &a,
                     const ReportResultChunk &b) {
                      if (a.stream_index != b.stream_index) {
                          return a.stream_index < b.stream_index;
                      }
                      if (a.kind != b.kind) return a.kind < b.kind;
                      if (a.start_ms != b.start_ms) {
                          return a.start_ms < b.start_ms;
                      }
                      if (a.end_ms != b.end_ms) return a.end_ms < b.end_ms;
                      if (a.source != b.source) return a.source < b.source;
                      const int name_cmp = strcmp(a.name ? a.name : "",
                                                  b.name ? b.name : "");
                      if (name_cmp != 0) return name_cmp < 0;
                      return a.payload_len < b.payload_len;
                  });
        if (!count_events_from_chunks()) return false;
        apply_event_indices_from_counts();
        if (!apply_series_metrics_from_chunks()) return false;
        status.state = report_result_settled_state(status.missing_required);
        status.error.clear();
        if (!cache_.publish_result(result)) return false;
        if (!plot_builder_.start()) return false;
        if (plot_builder_.active()) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result prepared index=%lu state=%s chunks=%lu "
                      "records=%lu bytes=%lu plot=building\n",
                      static_cast<unsigned long>(therapy_index),
                      result.state_name(),
                      static_cast<unsigned long>(status.chunk_count),
                      static_cast<unsigned long>(status.record_count),
                      static_cast<unsigned long>(status.payload_bytes));
            return true;
        }
    }
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "Result prepared index=%lu state=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long>(therapy_index),
              result.state_name(),
              static_cast<unsigned long>(status.chunk_count),
              static_cast<unsigned long>(status.record_count),
              static_cast<unsigned long>(status.payload_bytes));
    return true;
}


}  // namespace aircannect
