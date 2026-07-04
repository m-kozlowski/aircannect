#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include <Arduino.h>

#include "debug_log.h"
#include "edf_report_provider.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_plot_payload.h"
#include "report_resolve_context.h"
#include "report_source_resolver.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

const EdfReportProvider &edf_report_provider() {
    static EdfReportProvider provider;
    return provider;
}

}  // namespace

void ReportManager::reset_range_plot_build(bool clear_ready) {
    range_build_active_ = false;
    range_build_phase_ = ReportPlotBuildPhase::Idle;
    range_build_index_ = 0;
    range_build_from_ = 0;
    range_build_to_ = 0;
    range_night_start_ms_ = 0;
    range_chunk_count_ = 0;
    range_stream_count_ = 0;
    range_edf_session_count_ = 0;
    range_range_count_ = 0;
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        range_ranges_[i] = PlotRange{};
    }

    range_build_bytes_.reset();
    range_tmp_.clear();
    range_seen_events_.clear();
    range_event_count_ = 0;
    range_chunk_index_ = 0;
    range_stream_index_ = 0;
    range_series_open_ = false;
    range_series_points_ = 0;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    range_last_range_index_ = -1;
    range_bucket_ms_ = 1;
    range_current_bucket_ = -1;
    range_bucket_.clear();
    range_build_ok_ = true;
    range_build_started_ms_ = 0;
    range_build_input_chunks_ = 0;
    range_build_input_bytes_ = 0;

    if (!clear_ready) return;

    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
        xSemaphoreGive(result_slots_lock_);
    } else {
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }
}

bool ReportManager::ensure_range_build_buffers() {
    if (!range_indexed_night_) {
        range_indexed_night_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
        if (!range_indexed_night_) {
            log_report_alloc_failed("range_indexed_night",
                                    sizeof(ReportIndexedNight));
            return false;
        }
    }

    if (!range_chunks_) {
        range_chunks_ = static_cast<ReportResultChunk *>(
            Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                                 sizeof(ReportResultChunk),
                                 false));
        if (!range_chunks_) {
            log_report_alloc_failed(
                "range_chunks",
                AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
            return false;
        }
    }

    if (!range_edf_sessions_) {
        range_edf_sessions_ = static_cast<EdfReportSessionDescriptor *>(
            Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                                 sizeof(EdfReportSessionDescriptor),
                                 false));
        if (!range_edf_sessions_) {
            log_report_alloc_failed(
                "range_edf_sessions",
                AC_REPORT_EDF_SESSION_MAX *
                    sizeof(EdfReportSessionDescriptor));
            return false;
        }
    }

    return true;
}

bool ReportManager::read_range_chunk_payload(
    const ReportResultChunk &chunk,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) {
    ReportProviderChunk provider_chunk;
    provider_chunk.ref = chunk.provider_ref;
    provider_chunk.kind = chunk.kind;
    provider_chunk.source = chunk.source;
    provider_chunk.signal = chunk.signal;
    provider_chunk.name = chunk.name;
    provider_chunk.start_ms = chunk.start_ms;
    provider_chunk.end_ms = chunk.end_ms;
    provider_chunk.payload_schema = chunk.payload_schema;
    provider_chunk.record_count = chunk.record_count;
    provider_chunk.payload_len = chunk.payload_len;

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().read_chunk(
                provider_chunk,
                static_cast<int64_t>(range_night_start_ms_),
                meta,
                payload);
        case ReportProviderId::Edf:
            return edf_report_provider().read_chunk(provider_chunk,
                                                   range_edf_sessions_,
                                                   range_edf_session_count_,
                                                   meta,
                                                   payload);
        default:
            return false;
    }
}

bool ReportManager::for_each_range_series_sample(
    const ReportResultChunk &chunk,
    size_t stream_index,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }

    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           range_streams_,
                                           range_stream_count_,
                                           range_edf_sessions_,
                                           range_edf_session_count_,
                                           provider_chunk)) {
        return false;
    }

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().for_each_series_sample(
                provider_chunk,
                static_cast<int64_t>(range_night_start_ms_),
                stats,
                callback,
                context);
        case ReportProviderId::Edf:
            return edf_report_provider().for_each_series_sample(
                provider_chunk,
                range_edf_sessions_,
                range_edf_session_count_,
                stats,
                callback,
                context);
        default:
            return false;
    }
}

bool ReportManager::start_range_plot_build(uint64_t night_start_ms,
                                           size_t therapy_index_hint,
                                           int64_t from_ms,
                                           int64_t to_ms,
                                           bool &waiting_for_result) {
    waiting_for_result = false;
    reset_range_plot_build(false);
    if (to_ms <= from_ms) return false;
    if (!ensure_range_build_buffers()) return false;

    size_t therapy_index = therapy_index_hint;
    memset(range_indexed_night_, 0, sizeof(*range_indexed_night_));
    if (!indexed_night_by_start(night_start_ms,
                                *range_indexed_night_,
                                &therapy_index)) {
        return false;
    }
    ReportIndexedNight &indexed_night = *range_indexed_night_;

    range_edf_session_count_ = 0;
    bool edf_pending = false;
    memset(range_edf_sessions_,
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    bool have_edf =
        collect_edf_sessions_for_night(indexed_night.summary,
                                       from_ms,
                                       to_ms,
                                       range_edf_sessions_,
                                       AC_REPORT_EDF_SESSION_MAX,
                                       range_edf_session_count_,
                                       &edf_pending);
    if (edf_pending) {
        range_edf_session_count_ = 0;
        waiting_for_result = true;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Range plot waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<long long>(from_ms),
                  static_cast<long long>(to_ms));
        return false;
    }

    ScopedReportResolveContext resolve("range_resolver", false);
    if (!resolve) return false;

    EdfReportDataProvider edf_provider(have_edf ? range_edf_sessions_
                                                : nullptr,
                                       have_edf ? range_edf_session_count_
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    ReportResolvedPlan &plan = resolve.plan();
    if (!resolver.build_plan(indexed_night, from_ms, to_ms, plan)) {
        return false;
    }
    if (!materialize_range_plan(indexed_night, plan)) {
        return false;
    }

    range_build_index_ = therapy_index;
    range_build_from_ = from_ms;
    range_build_to_ = to_ms;
    range_bucket_ms_ = std::max<int64_t>(
        1,
        (to_ms - from_ms) /
            static_cast<int64_t>(AC_REPORT_RANGE_PLOT_BUCKETS));
    range_build_bytes_ = std::make_shared<ReportSpoolBuffer>();
    if (!range_build_bytes_) {
        fail_range_plot_build("range_alloc_failed");
        return false;
    }

    ReportSpoolBuffer &out = *range_build_bytes_;
    out.set_max_size(AC_REPORT_RANGE_PLOT_MAX_BYTES);
    range_tmp_.set_max_size(768 * 1024);
    range_seen_events_.set_max_size(16 * 1024);

    range_build_ok_ =
        out.reserve_capacity(64 * 1024) &&
        range_seen_events_.reserve_capacity(2 * 1024) &&
        bin_put_u32(out, PLOT_BIN_MAGIC) &&
        bin_put_u16(out, PLOT_BIN_VERSION) &&
        bin_put_u16(out, 0) &&
        bin_put_i64(out, from_ms);
    if (!range_build_ok_) {
        fail_range_plot_build("range_alloc_failed");
        return false;
    }

    range_build_active_ = true;
    range_build_started_ms_ = millis();
    range_build_input_chunks_ = 0;
    range_build_input_bytes_ = 0;
    range_build_phase_ = ReportPlotBuildPhase::Events;
    return true;
}

}  // namespace aircannect
