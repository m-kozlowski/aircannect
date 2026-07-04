#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include <Arduino.h>

#include "report_plot_payload.h"
#include "report_cache_fetch_service.h"
#include "report_range_plot_builder.h"
#include "report_range_plot_runtime.h"
#include "report_result_plot_builder.h"
#include "report_result_provider_bridge.h"
#include "report_result_cache_runtime.h"
#include "report_result_build_service.h"
#include "report_result_runtime.h"
#include "report_runtime_service.h"
#include "report_prefetch_service.h"
#include "report_summary_service.h"

namespace aircannect {

void ReportRuntimeService::service_range_plot(bool realtime_active) {
    ReportRangePlotRequest request;
    if (!result_cache_.range_request_snapshot(request)) return;
    if (realtime_active) return;
    if (summary_.active()) return;

    ReportResultPlotBuilder &plot_builder = result_build_.plot_builder();
    if (plot_builder.active()) {
        plot_builder.preempt_for_range(request.index, request.night_start_ms);
    }

    // Yield an idle prefetch so the range builds now; a real foreground fetch is
    // not yielded, so wait for that.
    if (cache_fetch_.active()) {
        prefetch_.yield_to_foreground();
        if (cache_fetch_.active()) return;
    }

    if (range_plot_.active()) {
        if (range_plot_.matches(request.index,
                                request.night_start_ms,
                                request.from_ms,
                                request.to_ms)) {
            return;
        }

        range_plot_.reset(false);
    }

    bool waiting_for_result = false;
    if (!range_plot_.start(request.night_start_ms,
                           request.index,
                           request.from_ms,
                           request.to_ms,
                           waiting_for_result)) {
        if (waiting_for_result) return;

        result_cache_.fail_range_request(request.index,
                                         request.night_start_ms,
                                         request.from_ms,
                                         request.to_ms);
    }
}

void ReportRangePlotBuilder::poll() {
    if (!range_plot_.active()) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();
    auto &range_plot = range_plot_.state();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_RANGE_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_RANGE_PLOT_POLL_BUDGET_MS;
    };

    while (range_plot.active && !budget_spent()) {
        // Event chunks
        if (range_plot.phase == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (range_plot.chunk_index < range_plot.chunk_count) {
                const ReportResultChunk &chunk =
                    range_plot.chunks[range_plot.chunk_index++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (chunk.end_ms <= range_plot.from_ms ||
                    chunk.start_ms >= range_plot.to_ms) {
                    continue;
                }

                if (!process_event_chunk(chunk)) return;

                reads++;
                range_plot.input_chunks++;
                range_plot.input_bytes += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            if (!range_plot.bytes) {
                fail("range_bad_state");
                return;
            }

            range_plot.ok &= bin_put_u32(*range_plot.bytes,
                                         range_plot.event_count);

            if (range_plot.tmp.size()) {
                range_plot.ok &=
                    range_plot.bytes->append(range_plot.tmp.data(),
                                             range_plot.tmp.size());
            }

            range_plot.tmp.clear();

            if (!range_plot.ok) {
                fail("range_overflow");
                return;
            }

            range_plot.phase = ReportPlotBuildPhase::Series;
            range_plot.chunk_index = 0;
            range_plot.stream_index = 0;

            continue;
        }

        // Series chunks
        if (range_plot.phase == ReportPlotBuildPhase::Series) {
            if (range_plot.stream_index >= range_plot.stream_count) {
                finish();
                return;
            }

            const ReportResultStream &stream =
                range_plot.streams[range_plot.stream_index];

            if (stream.kind != ReportStoreChunkKind::Series ||
                !stream.name || !stream.name[0] ||
                stream.chunk_count == 0) {
                range_plot.stream_index++;
                range_plot.chunk_index = 0;

                continue;
            }

            if (!range_plot.open_series(range_plot.stream_index)) {
                fail("range_series_open_failed");
                return;
            }

            bool processed = false;

            while (range_plot.chunk_index < range_plot.chunk_count) {
                const size_t chunk_index = range_plot.chunk_index++;
                const ReportResultChunk &chunk =
                    range_plot.chunks[chunk_index];

                if (range_plot.chunk_done[chunk_index]) continue;

                if (!report_result_chunk_matches_stream(chunk,
                                                        range_plot.stream_index,
                                                        stream)) {
                    continue;
                }

                if (chunk.end_ms <= range_plot.from_ms ||
                    chunk.start_ms >= range_plot.to_ms) {
                    continue;
                }

                if (chunk.provider_ref.provider == ReportProviderId::Edf) {
                    if (!process_edf_series_batch(chunk_index, processed)) {
                        return;
                    }

                    if (processed) {
                        reads++;
                        break;
                    }

                    continue;
                }

                if (!process_series_chunk(chunk, range_plot.stream_index)) {
                    return;
                }

                range_plot.chunk_done[chunk_index] = true;
                reads++;
                range_plot.input_chunks++;
                range_plot.input_bytes += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            const char *finish_error = nullptr;
            if (!range_plot.finish_series(range_plot.stream_index,
                                          &finish_error)) {
                fail(finish_error ? finish_error : "range_overflow");
                return;
            }

            range_plot.stream_index++;
            range_plot.chunk_index = 0;

            continue;
        }

        fail("range_bad_state");
        return;
    }
}

void ReportResultPlotBuilder::poll() {
    if (!result_.plot().active) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_PLOT_POLL_BUDGET_MS;
    };

    while (result_.plot().active && !budget_spent()) {
        // Event chunks
        if (result_.plot().phase == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (result_.plot().chunk_index < result_.status().chunk_count) {
                const ReportResultChunk &chunk =
                    result_.scratch().chunks()[result_.plot().chunk_index++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (!process_event_chunk(chunk)) return;

                processed = true;
                reads++;
                result_.plot().input_chunks++;
                result_.plot().input_bytes += chunk.payload_len;

                break;
            }

            if (processed) continue;

            // Event phase footer
            const uint32_t event_count =
                static_cast<uint32_t>(result_.plot().tmp.size() / 16);

            result_.plot().ok &=
                bin_put_u32(result_.plot().build_bin, event_count);

            if (result_.plot().tmp.size()) {
                result_.plot().ok &=
                    result_.plot().build_bin.append(result_.plot().tmp.data(),
                                                    result_.plot().tmp.size());
            }

            result_.plot().tmp.clear();
            result_.plot().phase = ReportPlotBuildPhase::Series;
            result_.plot().chunk_index = 0;
            memset(result_.plot().chunk_done, 0,
                   sizeof(result_.plot().chunk_done));

            continue;
        }

        // Series chunks
        if (result_.plot().phase == ReportPlotBuildPhase::Series) {
            bool processed = false;

            const size_t max_chunks = std::min(
                static_cast<size_t>(result_.status().chunk_count),
                static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));

            while (result_.plot().chunk_index < max_chunks) {
                const size_t chunk_index = result_.plot().chunk_index++;
                if (result_.plot().chunk_done[chunk_index]) continue;

                const ReportResultChunk &chunk =
                    result_.scratch().chunks()[chunk_index];

                if (chunk.kind != ReportStoreChunkKind::Series) {
                    result_.plot().chunk_done[chunk_index] = true;
                    continue;
                }

                if (chunk.stream_index >= result_.streams().count() ||
                    chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
                    result_.plot().chunk_done[chunk_index] = true;
                    continue;
                }

                if (chunk.provider_ref.provider == ReportProviderId::Edf) {
                    if (!process_edf_series_batch(chunk_index, processed)) {
                        return;
                    }

                    if (processed) {
                        reads++;
                        break;
                    }

                    continue;
                }

                if (!process_series_chunk(chunk_index)) return;

                result_.plot().chunk_done[chunk_index] = true;
                result_.plot().input_chunks++;
                result_.plot().input_bytes += chunk.payload_len;
                processed = true;
                reads++;

                break;
            }

            if (processed) continue;

            for (size_t i = 0; i < result_.streams().count() &&
                               i < AC_REPORT_RESULT_STREAM_MAX;
                 ++i) {
                if (!result_.plot().finish_series(
                        i,
                        result_.streams().data(),
                        result_.streams().count())) {
                    fail("plot_overflow");
                    return;
                }
            }

            if (!result_.plot().ok) {
                fail("plot_overflow");
                return;
            }

            if (result_.plot().chunk_index >= max_chunks) {
                finish();
                return;
            }

            continue;
        }

        fail("plot_bad_state");
        return;
    }
}

}  // namespace aircannect
