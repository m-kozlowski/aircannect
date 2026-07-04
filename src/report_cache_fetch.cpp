#include "report_manager.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "edf_report_provider.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_night_index.h"
#include "report_parser.h"
#include "report_resolve_context.h"
#include "report_source_resolver.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

struct ChunkWriteContext {
    ReportManager *manager = nullptr;
    ReportSourceId source = ReportSourceId::Summary;
    char *error = nullptr;
    size_t error_len = 0;
};

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

bool format_utc_ms_iso(uint64_t ms, std::string &out) {
    const time_t seconds = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;

    char buf[32];
    if (!strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv)) {
        return false;
    }

    out = buf;
    out += ".000Z";
    return true;
}

bool source_complete_for_range(const ReportSummaryRecord &night,
                               const ReportSourceDef &source,
                               int64_t from_ms) {
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;

    const int64_t coverage_start = std::max(span_start, from_ms);
    if (span_end <= coverage_start) return true;

    return spool_report_provider().coverage_complete(source,
                                                     coverage_start,
                                                     span_end);
}

bool source_latest_cached_end_for_night(const ReportSourceDef &source,
                                        const ReportSummaryRecord &night,
                                        int64_t &out_end_ms) {
    return spool_report_provider().latest_cached_end(
        source,
        static_cast<int64_t>(night.start_ms),
        static_cast<int64_t>(night.start_ms),
        static_cast<int64_t>(night.end_ms),
        out_end_ms);
}

}  // namespace

bool ReportManager::write_parsed_chunk(void *context,
                                       const ReportParsedChunk &chunk) {
    ChunkWriteContext *ctx = static_cast<ChunkWriteContext *>(context);
    if (!ctx || !ctx->manager || !chunk.name || !chunk.name[0] ||
        !chunk.payload || chunk.payload_schema == 0 ||
        chunk.record_count == 0 ||
        chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        if (ctx && ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_chunk");
        }
        return false;
    }
    if (!report_source_spool_type(chunk.source)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_source");
        }
        return false;
    }
    if (chunk.kind == ReportStoreChunkKind::Series &&
        chunk.payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s",
                     "bad_series_payload_schema");
        }
        return false;
    }
    if (chunk.kind == ReportStoreChunkKind::Events &&
        chunk.payload_schema != REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s",
                     "bad_event_payload_schema");
        }
        return false;
    }
    if (!ctx->manager->buffer_parsed_chunk(chunk)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            const std::string &detail = ctx->manager->cache_status_.error;
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     detail.empty() ? "cache_chunk_store_failed"
                                    : detail.c_str());
        }
        return false;
    }
    return true;
}

bool ReportManager::cache_source_supported(ReportSourceId source) const {
    switch (source) {
        case ReportSourceId::RespiratoryEvents:
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            return true;
        default:
            return false;
    }
}

bool ReportManager::build_cache_plan(const ReportIndexedNight &indexed_night,
                                     bool force,
                                     bool latest_tail_refresh) {
    const ReportSummaryRecord &night = indexed_night.summary;
    cache_night_ = night;
    cache_source_count_ = 0;
    cache_source_index_ = 0;
    cache_status_ = {};
    cache_status_.night_start_ms = night.start_ms;
    cache_status_.night_end_ms = night.end_ms;
    cache_status_.active = true;

    auto add_plan_source = [&](ReportSourceId source,
                               int64_t from_ms) -> bool {
        if (!cache_source_supported(source)) return true;
        for (size_t i = 0; i < cache_source_count_; ++i) {
            ReportCacheSourcePlan &existing = cache_plan_[i];
            if (existing.source != source) continue;
            if (from_ms < existing.from_ms) existing.from_ms = from_ms;
            return true;
        }
        if (cache_source_count_ >= AC_REPORT_CACHE_SOURCE_MAX) {
            fail_cache_fetch("cache_plan_full");
            return false;
        }
        ReportCacheSourcePlan &plan = cache_plan_[cache_source_count_++];
        plan.source = source;
        plan.from_ms = from_ms;
        return true;
    };

    int64_t latest_tail_start_ms = static_cast<int64_t>(night.start_ms);
    int64_t latest_tail_end_ms = static_cast<int64_t>(night.end_ms);
    if (latest_tail_refresh) {
        bool found_latest_range = false;
        auto maybe_use_latest_range = [&](const ReportSessionRange &range) {
            if (range.end_ms <= range.start_ms) return;
            if (!found_latest_range ||
                range.start_ms > latest_tail_start_ms) {
                latest_tail_start_ms = range.start_ms;
                latest_tail_end_ms = range.end_ms;
                found_latest_range = true;
            }
        };
        ReportSessionRange data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
        const size_t data_count =
            collect_indexed_night_data_ranges(indexed_night,
                                              data_ranges,
                                              AC_REPORT_SUMMARY_SESSION_MAX);
        if (indexed_night.has_edf && data_count > 0) {
            for (size_t i = 0; i < data_count; ++i) {
                maybe_use_latest_range(data_ranges[i]);
            }

            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                const ReportSessionRange &display = indexed_night.ranges[i];
                bool overlaps_edf = false;
                for (size_t k = 0; k < data_count; ++k) {
                    if (ranges_overlap(display.start_ms,
                                       display.end_ms,
                                       data_ranges[k].start_ms,
                                       data_ranges[k].end_ms)) {
                        overlaps_edf = true;
                        break;
                    }
                }
                if (!overlaps_edf) maybe_use_latest_range(display);
            }
        } else {
            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                maybe_use_latest_range(indexed_night.ranges[i]);
            }
        }
    }

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);

    if (force) {
        for (size_t i = 0; i < source_count; ++i) {
            const ReportSourceDef &source = sources[i];
            if (source.id == ReportSourceId::Summary) continue;
            if (!add_plan_source(source.id,
                                 static_cast<int64_t>(night.start_ms))) {
                return false;
            }
        }
    } else {
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (latest_tail_refresh) {
            span_start_ms = latest_tail_start_ms;
            span_end_ms = latest_tail_end_ms;
        } else if (!indexed_night_data_span(indexed_night,
                                            span_start_ms,
                                            span_end_ms)) {
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
            return true;
        }

        ScopedReportResolveContext resolve("cache_plan_resolver");
        if (!resolve) {
            fail_cache_fetch("cache_plan_alloc_failed");
            return false;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        const bool have_edf =
            collect_edf_sessions_for_night(night,
                                           span_start_ms,
                                           span_end_ms,
                                           resolve.sessions(),
                                           AC_REPORT_EDF_SESSION_MAX,
                                           session_count,
                                           &edf_pending);
        if (edf_pending) {
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
            return true;
        }

        EdfReportDataProvider edf_provider(have_edf ? resolve.sessions()
                                                    : nullptr,
                                           have_edf ? session_count : 0);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());
        ReportResolvedPlan &resolved = resolve.plan();
        if (!resolver.build_plan(indexed_night,
                                 span_start_ms,
                                 span_end_ms,
                                 resolved)) {
            fail_cache_fetch("cache_source_resolve_failed");
            return false;
        }

        for (size_t i = 0; i < resolved.segment_count; ++i) {
            const ReportResolvedSegment &segment = resolved.segments[i];
            if (segment.provider != ReportResolvedProvider::Spool ||
                segment.complete ||
                !segment.required ||
                segment.end_ms <= segment.start_ms) {
                continue;
            }
            const ReportSourceDef *source = report_source_def(segment.source);
            if (!source || !cache_source_supported(source->id)) continue;
            int64_t from_ms = segment.start_ms;
            if (latest_tail_refresh) {
                int64_t cached_end_ms = 0;
                if (source_latest_cached_end_for_night(*source,
                                                       night,
                                                       cached_end_ms) &&
                    cached_end_ms > latest_tail_start_ms) {
                    from_ms =
                        cached_end_ms - AC_REPORT_LATEST_TAIL_OVERLAP_MS;
                    if (from_ms < latest_tail_start_ms) {
                        from_ms = latest_tail_start_ms;
                    }
                    if (from_ms > segment.start_ms) {
                        from_ms = segment.start_ms;
                    }
                }
            }
            if (!add_plan_source(source->id, from_ms)) return false;
        }
    }

    if (cache_source_count_ == 0) {
        cache_status_.active = false;
        cache_status_.revision++;
        cache_status_.error.clear();
        return true;
    }
    return activate_cache_plan_for_night(night);
}

bool ReportManager::start_next_cache_source() {
    if (!cache_fetch_active_) return false;
    if (cache_source_index_ >= cache_source_count_) {
        finish_cache_fetch();
        return true;
    }

    const ReportCacheSourcePlan &plan = cache_plan_[cache_source_index_];
    const ReportSourceId source = plan.source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    std::string from_dt;
    if (!format_utc_ms_iso(static_cast<uint64_t>(plan.from_ms), from_dt)) {
        fail_cache_fetch("bad_cache_from_time");
        return false;
    }

    SpoolClientRequest request;
    request.spool_type = def->spool_type;
    request.from_dt = from_dt;
    request.max_size = AC_REPORT_CACHE_SPOOL_ROUND_BYTES;
    request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
    request.max_notifications = AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
    request.max_rounds = 128;
    request.pace_on_backpressure = true;
    request.stream_rounds = true;
    if (!spool_.begin(request)) {
        fail_cache_fetch("cache_spool_start_failed");
        return false;
    }

    if (!reset_cache_source_coverage_marks()) {
        fail_cache_fetch("coverage_extent_alloc_failed");
        return false;
    }
    cache_status_.active_source = source;
    cache_status_.source_index = static_cast<uint32_t>(cache_source_index_);
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "Cache source queued source=%s from=%s night=%llu\n",
              def->spool_type,
              from_dt.c_str(),
              static_cast<unsigned long long>(cache_night_.start_ms));
    return true;
}

bool ReportManager::store_cache_round(ReportSpoolResult &result) {
    const ReportSourceId source = cache_plan_[cache_source_index_].source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    ChunkWriteContext context;
    context.manager = this;
    context.source = source;
    char error[64] = {};
    context.error = error;
    context.error_len = sizeof(error);
    bool parsed = false;
    switch (source) {
        case ReportSourceId::UsageEvents:
        case ReportSourceId::RespiratoryEvents:
            parsed = report_parse_event_spool(result,
                                              source,
                                              write_parsed_chunk,
                                              &context,
                                              error,
                                              sizeof(error));
            break;
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            parsed = report_parse_series_spool(result,
                                               source,
                                               write_parsed_chunk,
                                               &context,
                                               error,
                                               sizeof(error));
            break;
        default:
            snprintf(error, sizeof(error), "%s", "unsupported_cache_source");
            parsed = false;
            break;
    }
    if (!parsed) {
        // An empty source spool is not a fetch failure: a session can hold zero
        // events, or a sampled source can have aged out
        if (strcmp(error, "spool_empty") == 0) return true;
        fail_cache_fetch(error[0] ? error : "cache_parse_failed");
        return false;
    }

    return true;
}

bool ReportManager::reset_cache_source_coverage_marks() {
    if (!ensure_cache_source_night_extents()) return false;
    memset(cache_source_night_extent_ms_,
           0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
    return true;
}

void ReportManager::note_cache_chunk_coverage(const ReportParsedChunk &chunk) {
    // Track every night the chunk touches, series AND events, recording how far
    // (max end_ms) real data reached so write_cache_source_coverage can bound
    // its coverage claims to the actually-delivered extent.
    if (chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        return;
    }

    if (!cache_source_night_extent_ms_) return;
    if (!take_summary_lock(portMAX_DELAY)) return;
    for (size_t record_index = 0;
         records_ &&
         record_index < record_count_ &&
         record_index < AC_REPORT_SUMMARY_RECORD_MAX;
         ++record_index) {
        const ReportSummaryRecord &record = records_[record_index];
        if (!record.valid || !record.duration_min) continue;

        if (ranges_overlap(chunk.start_ms,
                           chunk.end_ms,
                           static_cast<int64_t>(record.start_ms),
                           static_cast<int64_t>(record.end_ms))) {
            if (chunk.end_ms > cache_source_night_extent_ms_[record_index]) {
                cache_source_night_extent_ms_[record_index] = chunk.end_ms;
            }
        }
    }
    give_summary_lock();
}

bool ReportManager::write_cache_source_coverage(ReportSourceId source,
                                                int64_t from_ms) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    // Build coverage for every night this sweep delivered, then persist them all
    // in ONE load+coalesce+rewrite (write_coverage_batch). Writing per night
    // re-read+rewrote the whole coverage file O(nights) times on the spool path,
    // starving the CAN RX (dropped frames -> framing CRC). This runs only after
    // a source's spool completed the [from_ms, now] sweep. Per night:
    // - start: a tail refresh (from_ms past the session start) claims only from
    //   where it fetched, never back-claiming the earlier span; a full sweep
    //   (from_ms <= span_start) claims from the span start.
    // - end: the full session span. A sampled source that delivered nothing is
    //   skipped on a partial sweep but settled covered on a full sweep (the
    //   device no longer retains it -- aged out -- so it stops re-fetching).
    //   Events are sparse, so a covered span can legitimately hold zero events.
    const bool sampled = report_source_is_sampled(*def);
    static ReportStoreCoverageRecord *cov_batch = nullptr;
    if (!cov_batch) {
        cov_batch = static_cast<ReportStoreCoverageRecord *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportStoreCoverageRecord),
            false));
    }
    if (!cov_batch) {
        log_report_alloc_failed(
            "coverage_batch",
            AC_REPORT_SUMMARY_RECORD_MAX *
                sizeof(ReportStoreCoverageRecord));
        fail_cache_fetch("coverage_alloc_failed");
        return false;
    }

    ReportSummaryRecord *summary_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, summary_batch)) {
        fail_cache_fetch("coverage_summary_alloc_failed");
        return false;
    }

    size_t summary_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        fail_cache_fetch("coverage_summary_busy");
        return false;
    }
    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        summary_batch[summary_count++] = records_[i];
    }
    give_summary_lock();

    size_t batch_count = 0;
    for (size_t i = 0; i < summary_count; ++i) {
        const ReportSummaryRecord &record = summary_batch[i];
        if (!record.valid || !record.duration_min) continue;
        int64_t span_start = 0;
        int64_t span_end = 0;
        if (!night_data_span(record, span_start, span_end)) continue;
        if (span_end <= from_ms) continue;
        const int64_t extent = cache_source_night_extent_ms_[i];
        if (sampled && extent <= span_start && from_ms > span_start) continue;
        if (batch_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;

        ReportStoreCoverageRecord &coverage = cov_batch[batch_count];
        coverage = {};
        coverage.start_ms = from_ms > span_start ? from_ms : span_start;
        coverage.end_ms = span_end;
        coverage.parser_schema = def->parser_schema;
        coverage.state = ReportStoreCoverageState::Complete;
        coverage.origin = ReportStoreChunkOrigin::Spool;
        ++batch_count;
    }
    if (batch_count == 0) {
        give_summary_scratch();
        fail_cache_fetch("coverage_empty");
        return false;
    }
    give_summary_scratch();
    if (!ReportStore::write_coverage_batch(def->spool_type, cov_batch,
                                           batch_count)) {
        fail_cache_fetch("coverage_write_failed");
        return false;
    }
    if (!source_complete_for_range(cache_night_, *def, from_ms)) {
        fail_cache_fetch("coverage_incomplete");
        return false;
    }
    int64_t cached_end_ms = 0;
    if (report_source_is_sparse_event(*def) &&
        !source_latest_cached_end_for_night(*def, cache_night_, cached_end_ms)) {
        note_sparse_event_confirmed_empty(cache_night_, *def);
    }
    return true;
}

bool ReportManager::finalize_cache_source_if_ready() {
    if (fail_cache_fetch_if_write_failed()) return false;
    const CacheFlushResult flush = flush_all_cache_coalesce_buffers();
    if (flush == CacheFlushResult::Blocked) return true;
    if (flush == CacheFlushResult::Failed) {
        fail_cache_fetch("cache_flush_failed");
        return false;
    }
    if (cache_writes_pending_for_active_fetch()) return true;

    if (!write_cache_source_coverage(cache_finalizing_plan_.source,
                                     cache_finalizing_plan_.from_ms)) {
        return false;
    }
    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
    cache_source_index_++;
    return start_next_cache_source();
}

bool ReportManager::fail_cache_fetch_if_write_failed() {
    std::string write_error;
    if (cache_write_failed_for_active_fetch(write_error)) {
        fail_cache_fetch(write_error.empty() ? "cache_write_failed"
                                             : write_error.c_str());
        return true;
    }
    return false;
}

bool ReportManager::drain_cache_spool_rounds() {
    ReportSpoolResult round;
    while (spool_.take_completed_round(round)) {
        if (!store_cache_round(round)) return false;
        round.clear();
        cache_status_.spool = spool_.status();
        if (cache_write_backpressure_active()) return false;
    }
    return true;
}

void ReportManager::finish_cache_spool_if_terminal() {
    if (spool_.complete()) {
        ReportSpoolResult final_result;
        spool_.move_result_to(final_result);
        if (final_result.truncated) {
            fail_cache_fetch("source_truncated");
            return;
        }
        // Flush before claiming coverage, so coverage never marks unpersisted data.
        cache_finalizing_plan_ = cache_plan_[cache_source_index_];
        cache_source_finalizing_ = true;
        (void)finalize_cache_source_if_ready();
    } else if (spool_.failed()) {
        fail_cache_fetch(spool_.status().error.c_str());
    }
}

void ReportManager::poll_cache_fetch(RpcArbiter &arbiter) {
    if (!cache_fetch_active_) return;

    if (cache_source_finalizing_) {
        (void)finalize_cache_source_if_ready();
        return;
    }
    if (fail_cache_fetch_if_write_failed()) return;

    spool_.poll(arbiter);
    log_spool_can_pressure(arbiter);
    cache_status_.spool = spool_.status();
    if (cache_write_backpressure_active()) return;

    if (!drain_cache_spool_rounds()) return;
    finish_cache_spool_if_terminal();
}

void ReportManager::finish_cache_fetch() {
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error.clear();
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_INFO,
              "Cache fetch complete night=%llu chunks=%lu\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              static_cast<unsigned long>(cache_status_.chunks_written));
    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index, false);
    }
}

void ReportManager::fail_cache_fetch(const char *message) {
    discard_cache_coalesce_buffers();
    abort_cache_write_fetch();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = message ? message : "cache_fetch_failed";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_WARN, "Cache fetch failed: %s\n",
              cache_status_.error.c_str());
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(cache_status_.error.c_str());
    }
}

}  // namespace aircannect
