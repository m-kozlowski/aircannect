#include "report_cache_fetch_service.h"

#include <algorithm>
#include <stdio.h>
#include <string>
#include <string.h>
#include <time.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_diagnostics.h"
#include "report_manager_helpers.h"
#include "report_parser.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {
namespace {

struct ChunkWriteContext {
    ReportCacheFetchService *service = nullptr;
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

}  // namespace

ReportCacheFetchService::ReportCacheFetchService(
    ReportFetchRuntime &fetch,
    ReportSummaryRuntime &summary,
    ReportCacheStorageRuntime &storage,
    ReportCacheWriteSink &write_sink,
    ReportEdfCatalogContext &edf_catalog)
    : fetch_(fetch),
      summary_(summary),
      storage_(storage),
      write_sink_(write_sink),
      edf_catalog_(edf_catalog) {}

void ReportCacheFetchService::set_pending_prepare(size_t therapy_index,
                                                  bool refresh_cache) {
    fetch_.set_pending_prepare(therapy_index, refresh_cache);
}

bool ReportCacheFetchService::take_pending_prepare(
    ReportPendingResultPrepare &out) {
    return fetch_.take_pending_prepare(out);
}

bool ReportCacheFetchService::activate_plan() {
    storage_.discard_coalesced();
    storage_.begin_write_fetch();
    fetch_.cache().activate();
    return true;
}

ReportCacheFetchEvent ReportCacheFetchService::start_next_source() {
    if (!fetch_.cache_active()) return ReportCacheFetchEvent::Failed;

    const ReportCacheSourcePlan *plan = fetch_.cache().current_source();
    if (!plan) return finish();

    const ReportSourceId source = plan->source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        return fail("bad_cache_source");
    }

    std::string from_dt;
    if (!format_utc_ms_iso(static_cast<uint64_t>(plan->from_ms), from_dt)) {
        return fail("bad_cache_from_time");
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
    if (!fetch_.begin_spool(request)) {
        return fail("cache_spool_start_failed");
    }

    if (!reset_source_coverage_marks()) {
        return fail("coverage_extent_alloc_failed");
    }

    fetch_.update_cache_active_source(source);
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Cache source queued source=%s from=%s night=%llu\n",
              def->spool_type,
              from_dt.c_str(),
              static_cast<unsigned long long>(
                  fetch_.cache().night().start_ms));
    return ReportCacheFetchEvent::None;
}

bool ReportCacheFetchService::reset_source_coverage_marks() {
    return summary_.reset_coverage_marks();
}

bool ReportCacheFetchService::write_source_coverage(ReportSourceId source,
                                                    int64_t from_ms) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail("bad_cache_source");
        return false;
    }

    // Build coverage for every night this sweep delivered, then persist them all
    // in ONE load+coalesce+rewrite. Writing per night rewrote the whole coverage
    // file O(nights) times on the spool path, starving CAN RX.
    const bool sampled = report_source_is_sampled(*def);
    static ReportStoreCoverageRecord *cov_batch = nullptr;
    if (!cov_batch) {
        cov_batch = static_cast<ReportStoreCoverageRecord *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportStoreCoverageRecord),
                                 false));
    }
    if (!cov_batch) {
        log_report_alloc_failed(
            "coverage_batch",
            AC_REPORT_SUMMARY_RECORD_MAX *
                sizeof(ReportStoreCoverageRecord));
        fail("coverage_alloc_failed");
        return false;
    }

    ReportSummaryRecord *summary_batch = nullptr;
    if (!summary_.take_scratch(portMAX_DELAY, summary_batch)) {
        fail("coverage_summary_alloc_failed");
        return false;
    }

    size_t summary_count = 0;
    if (!summary_.take(portMAX_DELAY)) {
        summary_.give_scratch();
        fail("coverage_summary_busy");
        return false;
    }

    const ReportSummaryRecord *records = summary_.records();
    const size_t record_count = summary_.record_count();
    for (size_t i = 0;
         records && i < record_count && i < AC_REPORT_SUMMARY_RECORD_MAX;
         ++i) {
        summary_batch[summary_count++] = records[i];
    }
    summary_.give();

    size_t batch_count = 0;
    for (size_t i = 0; i < summary_count; ++i) {
        const ReportSummaryRecord &record = summary_batch[i];
        if (!record.valid || !record.duration_min) continue;

        int64_t span_start = 0;
        int64_t span_end = 0;
        if (!night_data_span(record, span_start, span_end)) continue;
        if (span_end <= from_ms) continue;

        const int64_t extent = summary_.coverage_extent(i);
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
        summary_.give_scratch();
        fail("coverage_empty");
        return false;
    }
    summary_.give_scratch();

    if (!ReportStore::write_coverage_batch(def->spool_type,
                                           cov_batch,
                                           batch_count)) {
        fail("coverage_write_failed");
        return false;
    }

    if (!source_complete_for_range(fetch_.cache().night(), *def, from_ms)) {
        fail("coverage_incomplete");
        return false;
    }
    return true;
}

bool ReportCacheFetchService::fail_if_write_failed() {
    std::string write_error;
    if (!storage_.write_failed_for_active_fetch(write_error)) return false;

    fail(write_error.empty() ? "cache_write_failed" : write_error.c_str());
    return true;
}

ReportCacheFetchEvent ReportCacheFetchService::finalize_source_if_ready() {
    if (fail_if_write_failed()) return ReportCacheFetchEvent::Failed;

    const report_manager_internal::CacheFlushResult flush =
        storage_.flush_coalesced(write_sink_);
    if (flush == report_manager_internal::CacheFlushResult::Blocked) {
        return ReportCacheFetchEvent::None;
    }
    if (flush == report_manager_internal::CacheFlushResult::Failed) {
        return fail("cache_flush_failed");
    }
    if (storage_.writes_pending_for_active_fetch()) {
        return ReportCacheFetchEvent::None;
    }

    const ReportCacheSourcePlan &plan = fetch_.cache().finalizing_plan();
    if (!write_source_coverage(plan.source, plan.from_ms)) {
        return ReportCacheFetchEvent::Failed;
    }

    fetch_.cache().complete_finalizing_source();
    return start_next_source();
}

bool ReportCacheFetchService::buffer_parsed_chunk(
    const ReportParsedChunk &chunk) {
    // Map a timestamp to its summary-night bucket start (the partition key).
    // Bucket boundaries sit around local noon, so a chunk straddling one is
    // filed whole by its start timestamp.
    const int64_t night = summary_.night_start_for_timestamp(chunk.start_ms);

    const ReportCacheCoalesceResult result =
        storage_.buffer_chunk(chunk, night, write_sink_);
    if (result == ReportCacheCoalesceResult::Buffered) return true;

    const char *error = report_cache_coalesce_error(result);
    if (result == ReportCacheCoalesceResult::Backpressure ||
        result == ReportCacheCoalesceResult::FlushFailed) {
        fetch_.cache().set_error(error);
        return false;
    }

    fail(error);
    return false;
}

bool ReportCacheFetchService::write_parsed_chunk(
    void *context,
    const ReportParsedChunk &chunk) {
    ChunkWriteContext *ctx = static_cast<ChunkWriteContext *>(context);
    if (!ctx || !ctx->service || !chunk.name || !chunk.name[0] ||
        !chunk.payload || chunk.payload_schema == 0 ||
        chunk.record_count == 0 || chunk.start_ms < 0 ||
        chunk.end_ms <= chunk.start_ms) {
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
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     "bad_series_payload_schema");
        }
        return false;
    }

    if (chunk.kind == ReportStoreChunkKind::Events &&
        chunk.payload_schema != REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     "bad_event_payload_schema");
        }
        return false;
    }

    if (!ctx->service->buffer_parsed_chunk(chunk)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            const std::string &detail = ctx->service->status().error;
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

bool ReportCacheFetchService::store_cache_round(ReportSpoolResult &result) {
    const ReportCacheSourcePlan *plan = fetch_.cache().current_source();
    if (!plan) {
        fail("bad_cache_source");
        return false;
    }

    const ReportSourceId source = plan->source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def) {
        fail("bad_cache_source");
        return false;
    }

    ChunkWriteContext context;
    context.service = this;
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
        // events, or a sampled source can have aged out.
        if (strcmp(error, "spool_empty") == 0) return true;

        fail(error[0] ? error : "cache_parse_failed");
        return false;
    }

    return true;
}

bool ReportCacheFetchService::drain_spool_rounds() {
    ReportSpoolResult round;
    while (fetch_.take_completed_round(round)) {
        if (!store_cache_round(round)) return false;

        round.clear();
        fetch_.update_cache_spool();
        if (storage_.write_backpressure_active()) return false;
    }
    return true;
}

ReportCacheFetchEvent ReportCacheFetchService::finish_spool_if_terminal() {
    if (fetch_.spool_complete()) {
        ReportSpoolResult final_result;
        fetch_.move_spool_result_to(final_result);
        if (final_result.truncated) return fail("source_truncated");

        // Flush before claiming coverage, so coverage never marks unpersisted data.
        fetch_.cache().begin_finalizing_current_source();
        return finalize_source_if_ready();
    }

    if (fetch_.spool_failed()) {
        return fail(fetch_.spool_status().error.c_str());
    }

    return ReportCacheFetchEvent::None;
}

ReportCacheFetchEvent ReportCacheFetchService::poll(RpcArbiter &arbiter) {
    if (!fetch_.cache_active()) return ReportCacheFetchEvent::None;

    if (fetch_.cache().finalizing_source()) {
        return finalize_source_if_ready();
    }
    if (fail_if_write_failed()) return ReportCacheFetchEvent::Failed;

    fetch_.poll_spool(arbiter);
    fetch_.update_cache_spool();
    if (storage_.write_backpressure_active()) return ReportCacheFetchEvent::None;

    if (!drain_spool_rounds()) return ReportCacheFetchEvent::Failed;
    return finish_spool_if_terminal();
}

ReportCacheFetchEvent ReportCacheFetchService::finish() {
    fetch_.finish_cache_fetch();

    const ReportCacheFetchStatus &current_status = fetch_.cache_status();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Cache fetch complete night=%llu chunks=%lu\n",
              static_cast<unsigned long long>(
                  current_status.night_start_ms),
              static_cast<unsigned long>(current_status.chunks_written));
    return ReportCacheFetchEvent::Completed;
}

ReportCacheFetchEvent ReportCacheFetchService::fail(const char *message) {
    storage_.discard_coalesced();
    storage_.abort_write_fetch();
    fetch_.fail_cache_fetch(message);

    const ReportCacheFetchStatus &current_status = fetch_.cache_status();
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "Cache fetch failed: %s\n",
              current_status.error.c_str());
    return ReportCacheFetchEvent::Failed;
}

ReportCacheFetchEvent ReportCacheFetchService::cancel(const char *message) {
    fetch_.reset_spool();
    storage_.abort_write_fetch();
    fetch_.cancel_cache_fetch(message);

    const ReportCacheFetchStatus &current_status = fetch_.cache_status();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Cache fetch cancelled night=%llu source=%s\n",
              static_cast<unsigned long long>(
                  current_status.night_start_ms),
              report_source_spool_type(current_status.active_source));
    return ReportCacheFetchEvent::Failed;
}

}  // namespace aircannect
