#include "report_manager.h"

#include <algorithm>
#include <stdint.h>
#include <string.h>

#include "edf_report_data_plan.h"
#include "edf_report_provider.h"
#include "report_data_provider.h"
#include "report_event_dedupe.h"
#include "report_plot_payload.h"
#include "report_records.h"
#include "report_source_resolver.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

struct RangeChunkContext {
    ReportManager *manager = nullptr;
    size_t stream_index = SIZE_MAX;
    const char *name = nullptr;
};

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

bool report_stream_bit(size_t stream_index, uint32_t &bit) {
    if (stream_index >= 32) return false;
    bit = 1u << static_cast<uint32_t>(stream_index);
    return true;
}

}  // namespace

bool ReportManager::process_range_event_chunk(
    const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!read_range_chunk_payload(chunk, meta, payload)) {
        fail_range_plot_build("range_event_read_failed");
        return false;
    }
    const size_t wire = report_event_record_wire_size();
    const size_t count = wire ? payload.size() / wire : 0;
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        if (!report_event_overlaps_window(event,
                                          range_build_from_,
                                          range_build_to_)) {
            continue;
        }

        bool in_session_range = false;
        for (size_t i = 0; i < range_range_count_; ++i) {
            if (report_event_overlaps_window(
                    event,
                    range_ranges_[i].start_ms,
                    range_ranges_[i].end_ms,
                    AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
                in_session_range = true;
                break;
            }
        }
        if (!in_session_range) continue;

        if (report_event_seen(range_seen_events_, event)) continue;
        if (!remember_report_event(range_seen_events_, event)) {
            fail_range_plot_build("range_event_dedupe_failed");
            return false;
        }
        range_build_ok_ &=
            bin_put_i32(range_tmp_,
                        static_cast<int32_t>(event.start_ms -
                                             range_build_from_));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.duration_ms));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.code));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.flags));
        if (!range_build_ok_) {
            fail_range_plot_build("range_overflow");
            return false;
        }
        ++range_event_count_;
    }
    return true;
}

bool ReportManager::open_range_series(const ReportResultStream &stream) {
    const size_t name_len = stream.name ? strlen(stream.name) : 0;
    if (!range_build_bytes_ || name_len > UINT16_MAX) return false;
    range_tmp_.clear();
    range_series_points_ = 0;
    range_current_bucket_ = -1;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    range_last_range_index_ = -1;
    range_bucket_.clear();
    range_series_open_ = true;
    return range_build_ok_;
}

int ReportManager::range_plot_range_index(int64_t timestamp_ms) const {
    for (size_t i = 0; i < range_range_count_; ++i) {
        if (timestamp_ms >= range_ranges_[i].start_ms &&
            timestamp_ms < range_ranges_[i].end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReportManager::result_chunk_matches_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream &stream) const {
    if (!result_chunk_has_stream(chunk, stream_index)) return false;
    if (chunk.stream_mask != 0) {
        return chunk.kind == stream.kind;
    }
    return chunk.kind == stream.kind && chunk.signal == stream.signal &&
           chunk.name && stream.name && strcmp(chunk.name, stream.name) == 0;
}

bool ReportManager::collect_range_chunk(void *context,
                                        const ReportProviderChunk &info) {
    RangeChunkContext *ctx = static_cast<RangeChunkContext *>(context);
    if (!ctx || !ctx->manager || !info.name || !info.name[0]) return false;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }
    return ctx->manager->add_range_provider_chunk(info, ctx->stream_index);
}

bool ReportManager::add_range_provider_chunk(
    const ReportProviderChunk &provider_chunk,
    size_t stream_index) {
    if (stream_index >= range_stream_count_ || stream_index > UINT8_MAX) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    uint32_t stream_bit = 0;
    if (!report_stream_bit(stream_index, stream_bit)) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    if (!range_chunks_) {
        fail_range_plot_build("range_chunks_missing");
        return false;
    }

    ReportResultStream &stream = range_streams_[stream_index];
    if (stream.kind != provider_chunk.kind ||
        stream.signal != provider_chunk.signal ||
        !stream.name ||
        strcmp(stream.name, provider_chunk.name) != 0) {
        fail_range_plot_build("range_stream_mismatch");
        return false;
    }
    auto account_stream = [&]() {
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
    };

    for (size_t i = 0; i < range_chunk_count_; ++i) {
        ReportResultChunk &existing = range_chunks_[i];
        const bool same_physical =
            result_chunk_same_physical_edf(existing, provider_chunk);
        const bool same_logical =
            existing.kind == provider_chunk.kind &&
            existing.source == provider_chunk.source &&
            existing.name && provider_chunk.name &&
            strcmp(existing.name, provider_chunk.name) == 0 &&
            existing.start_ms == provider_chunk.start_ms &&
            existing.end_ms == provider_chunk.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            provider_chunk.ref);
        if (!same_physical && !same_logical) continue;
        if ((existing.stream_mask & stream_bit) != 0) return true;
        existing.stream_mask |= stream_bit;
        account_stream();
        return true;
    }

    if (range_chunk_count_ >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail_range_plot_build("range_chunks_full");
        return false;
    }

    ReportResultChunk &chunk = range_chunks_[range_chunk_count_++];
    chunk.provider_ref = provider_chunk.ref;
    chunk.kind = provider_chunk.kind;
    chunk.source = provider_chunk.source;
    chunk.signal = provider_chunk.signal;
    chunk.name = provider_chunk.name;
    chunk.stream_index = static_cast<uint8_t>(stream_index);
    chunk.stream_mask = stream_bit;
    chunk.start_ms = provider_chunk.start_ms;
    chunk.end_ms = provider_chunk.end_ms;
    chunk.payload_schema = provider_chunk.payload_schema;
    chunk.record_count = provider_chunk.record_count;
    chunk.payload_len = provider_chunk.payload_len;

    account_stream();
    return true;
}

bool ReportManager::materialize_range_plan(const ReportIndexedNight &night,
                                           const ReportResolvedPlan &plan) {
    range_night_start_ms_ = night.summary.start_ms;
    range_chunk_count_ = 0;
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        range_chunks_[i] = ReportResultChunk{};
    }
    range_stream_count_ =
        std::min(plan.stream_count,
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        range_streams_[i] = ReportResultStream{};
    }
    for (size_t i = 0; i < range_stream_count_; ++i) {
        const ReportResolvedStream &resolved = plan.streams[i];
        ReportResultStream &stream = range_streams_[i];
        stream.kind = resolved.kind;
        stream.source = resolved.selected_source;
        stream.signal = resolved.signal;
        stream.name = resolved.name;
        stream.required = resolved.required;
        stream.complete = resolved.complete;
        stream.has_edf_segment = resolved.has_edf_segment;
        stream.has_spool_segment = resolved.has_spool_segment;
    }
    range_range_count_ =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        if (i < range_range_count_) {
            range_ranges_[i].start_ms = plan.ranges[i].start_ms;
            range_ranges_[i].end_ms = plan.ranges[i].end_ms;
        } else {
            range_ranges_[i] = PlotRange{};
        }
    }

    EdfReportDataProvider edf_provider(range_edf_sessions_,
                                       range_edf_session_count_);
    for (size_t i = 0; i < plan.segment_count; ++i) {
        const ReportResolvedSegment &segment = plan.segments[i];
        if (segment.stream_index >= range_stream_count_) {
            fail_range_plot_build("range_bad_segment");
            return false;
        }
        if (!segment.complete ||
            segment.provider == ReportResolvedProvider::None) {
            continue;
        }
        const ReportSourceDef *source_def = report_source_def(segment.source);
        if (!source_def || !source_def->spool_type ||
            !source_def->spool_type[0]) {
            fail_range_plot_build("range_bad_source");
            return false;
        }
        const ReportDataProvider *provider = nullptr;
        if (segment.provider == ReportResolvedProvider::Edf) {
            provider = &edf_provider;
        } else if (segment.provider == ReportResolvedProvider::Spool) {
            provider = &spool_report_provider();
        }
        if (!provider) {
            fail_range_plot_build("range_bad_provider");
            return false;
        }
        RangeChunkContext context;
        context.manager = this;
        context.stream_index = segment.stream_index;
        context.name = segment.name;
        if (!provider->for_each_chunk(segment.kind,
                                      *source_def,
                                      segment.signal,
                                      segment.name,
                                      static_cast<int64_t>(
                                          night.summary.start_ms),
                                      segment.start_ms,
                                      segment.end_ms,
                                      collect_range_chunk,
                                      &context)) {
            fail_range_plot_build("range_chunk_list_failed");
            return false;
        }
    }
    return true;
}


}  // namespace aircannect
