#include "report_manager.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include "debug_log.h"
#include "report_event_dedupe.h"
#include "report_records.h"
#include "report_result_metrics.h"

namespace aircannect {

bool ReportManager::count_result_events_from_chunks() {
    const size_t range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    result_status_.oa_count = 0;
    result_status_.ca_count = 0;
    result_status_.ua_count = 0;
    result_status_.hypopnea_count = 0;
    result_status_.arousal_count = 0;
    ReportSpoolBuffer counted_events;
    counted_events.set_max_size(64 * 1024);
    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (chunk.kind != ReportStoreChunkKind::Events) continue;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
        if (!read_result_chunk_payload(chunk, meta, payload)) {
            fail_result_prepare("event_chunk_read_failed");
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
                const PlotRange &range = result_ranges_[range_index];
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
                fail_result_prepare("event_dedupe_failed");
                return false;
            }
            switch (event.code) {
                case report_event_code_value(ReportEventCode::Hypopnea):
                    result_status_.hypopnea_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::CentralApnea):
                    result_status_.ca_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::ObstructiveApnea):
                    result_status_.oa_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::UnclassifiedApnea):
                    result_status_.ua_count++;
                    break;
                case report_event_code_value(ReportEventCode::Arousal):
                    result_status_.arousal_count++;
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

void ReportManager::apply_result_event_indices_from_counts() {
    if (result_status_.duration_min <= 0) return;
    if (!result_status_.events_available) return;

    const float hours =
        static_cast<float>(result_status_.duration_min) / 60.0f;
    if (hours <= 0.0f) return;

    const float oa_index =
        static_cast<float>(result_status_.oa_count) / hours;
    const float ca_index =
        static_cast<float>(result_status_.ca_count) / hours;
    const float ua_index =
        static_cast<float>(result_status_.ua_count) / hours;
    const float hypopnea_index =
        static_cast<float>(result_status_.hypopnea_count) / hours;
    const float arousal_index =
        static_cast<float>(result_status_.arousal_count) / hours;

    if (!result_status_.oa_index_valid) {
        result_status_.oa_index = oa_index;
        result_status_.oa_index_valid = true;
        result_status_.oa_index_source = ReportMetricSource::Calculated;
    }
    if (!result_status_.ca_index_valid) {
        result_status_.ca_index = ca_index;
        result_status_.ca_index_valid = true;
        result_status_.ca_index_source = ReportMetricSource::Calculated;
    }
    if (!result_status_.ua_index_valid) {
        result_status_.ua_index = ua_index;
        result_status_.ua_index_valid = true;
        result_status_.ua_index_source = ReportMetricSource::Calculated;
    }
    if (!result_status_.hypopnea_index_valid) {
        result_status_.hypopnea_index = hypopnea_index;
        result_status_.hypopnea_index_valid = true;
        result_status_.hypopnea_index_source =
            ReportMetricSource::Calculated;
    }
    if (!result_status_.arousal_index_valid) {
        result_status_.arousal_index = arousal_index;
        result_status_.arousal_index_valid = true;
        result_status_.arousal_index_source = ReportMetricSource::Calculated;
    }
    if (!result_status_.ahi_valid) {
        result_status_.ahi =
            oa_index + ca_index + ua_index + hypopnea_index;
        result_status_.ahi_valid = true;
        result_status_.ahi_source = ReportMetricSource::Calculated;
    }
}

bool ReportManager::result_timestamp_in_ranges(int64_t timestamp_ms) const {
    const size_t range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (range_count == 0) return true;
    for (size_t i = 0; i < range_count; ++i) {
        const PlotRange &range = result_ranges_[i];
        if (timestamp_ms >= range.start_ms && timestamp_ms <= range.end_ms) {
            return true;
        }
    }
    return false;
}

bool ReportManager::apply_result_series_metrics_from_chunks() {
    ReportMetricAverage pressure_average;
    ReportMetricAverage mask_pressure_average;
    ReportMetricAverage leak_average;

    struct MetricSeriesContext {
        ReportManager *manager = nullptr;
        ReportMetricAverage *average = nullptr;
        float multiplier = 1.0f;
    };

    for (uint32_t chunk_index = 0; chunk_index < result_status_.chunk_count;
         ++chunk_index) {
        const ReportResultChunk &chunk = result_chunks_[chunk_index];
        if (chunk.kind != ReportStoreChunkKind::Series) continue;
        for (size_t stream_index = 0; stream_index < result_stream_count_;
             ++stream_index) {
            if (!result_chunk_has_stream(chunk, stream_index)) continue;
            const ReportResultStream &stream = result_streams_[stream_index];
            MetricSeriesContext ctx;
            ctx.manager = this;
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
            const bool ok = for_each_result_series_sample(
                chunk,
                stream_index,
                stats,
                [](void *context, const ReportSeriesSample &sample) -> bool {
                    MetricSeriesContext *ctx =
                        static_cast<MetricSeriesContext *>(context);
                    if (!ctx || !ctx->manager || !ctx->average) return false;
                    if (!ctx->manager->result_timestamp_in_ranges(
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
        result_status_.mask_pressure_50_cm_h2o = value;
        result_status_.mask_pressure_50_valid = true;
        result_status_.mask_pressure_50_source = ReportMetricSource::Calculated;
    }
    if (leak_average.mean(value)) {
        result_status_.leak_50_l_min = value;
        result_status_.leak_50_valid = true;
        result_status_.leak_50_source = ReportMetricSource::Calculated;
    }
    return true;
}

bool ReportManager::finalize_result_prepare(size_t therapy_index) {
    if (result_status_.state == ReportResultState::Error) return false;
    // Build a best-effort plot from whatever is cached: aged-out signals leave
    // missing_streams>0 and not-yet-swept sources leave missing_required>0, but
    // both are reported for the UI to mark - they do not block rendering. Only
    // a night with nothing cached at all (background hasn't reached it) has no
    // plot to show.
    if (result_status_.chunk_count == 0) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "not_cached";
        if (!publish_result_to_slot()) return false;
    } else {
        std::sort(result_chunks_,
                  result_chunks_ + result_status_.chunk_count,
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
        if (!count_result_events_from_chunks()) return false;
        apply_result_event_indices_from_counts();
        if (!apply_result_series_metrics_from_chunks()) return false;
        result_status_.state =
            report_result_settled_state(result_status_.missing_required);
        result_status_.error.clear();
        if (!publish_result_to_slot()) return false;
        if (!start_result_plot_build()) return false;
        if (plot_build_active_) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result prepared index=%lu state=%s chunks=%lu "
                      "records=%lu bytes=%lu plot=building\n",
                      static_cast<unsigned long>(therapy_index),
                      result_state_name(),
                      static_cast<unsigned long>(result_status_.chunk_count),
                      static_cast<unsigned long>(result_status_.record_count),
                      static_cast<unsigned long>(result_status_.payload_bytes));
            return true;
        }
    }
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "Result prepared index=%lu state=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long>(therapy_index),
              result_state_name(),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_status_.record_count),
              static_cast<unsigned long>(result_status_.payload_bytes));
    return true;
}


}  // namespace aircannect
