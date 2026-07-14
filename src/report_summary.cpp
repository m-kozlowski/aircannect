#include "report_manager.h"
#include "report_summary_service.h"

#include <string.h>
#include <string>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_records.h"
#include "report_store.h"
#include "report_summary_json.h"

namespace aircannect {
namespace {

const char *const REPORT_SUMMARY_FROM = "2000-01-01T00:00:00.000Z";

struct SummaryRecordBufferContext {
    ReportSummaryRecord *records = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    uint32_t nights_with_therapy = 0;
};

bool store_summary_record_to_buffer(void *context,
                                    const ReportSummaryRecord &record) {
    SummaryRecordBufferContext *ctx =
        static_cast<SummaryRecordBufferContext *>(context);
    if (!ctx || !ctx->records || ctx->count >= ctx->capacity) {
        return false;
    }

    ctx->records[ctx->count++] = record;
    if (record.duration_min > 0) ctx->nights_with_therapy++;
    return true;
}

}  // namespace

ReportSummaryService::ReportSummaryService(
    ReportSummaryRuntime &summary,
    ReportFetchRuntime &fetch,
    ReportNightIndexRuntime &night_index,
    ReportNightIndexService &night_index_service,
    ReportResultCacheRuntime &result_cache)
    : summary_(summary),
      fetch_(fetch),
      night_index_(night_index),
      night_index_service_(night_index_service),
      result_cache_(result_cache) {}

void ReportSummaryService::begin() {
    summary_.begin();
}

void ReportSummaryService::load_initial_snapshot() {
    summary_.clear_records();
    summary_.reset_status();

    if (!load_from_store()) {
        publish_json_snapshot();
    }
}

bool ReportSummaryService::request_refresh(bool force,
                                           bool cache_fetch_active) {
    if (fetch_.summary_active() && !force) return true;
    if (cache_fetch_active) return false;

    SpoolClientRequest request;
    request.spool_type = "Summary";
    request.from_dt = REPORT_SUMMARY_FROM;
    request.max_size = AC_REPORT_SUMMARY_SPOOL_ROUND_BYTES;
    request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
    request.max_notifications = AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
    request.max_rounds = 64;
    request.pace_on_backpressure = true;
    request.stream_rounds = false;

    if (!fetch_.start_summary_fetch(request, millis())) {
        (void)fail_fetch("summary_start_failed");
        return false;
    }

    if (summary_.take(portMAX_DELAY)) {
        ReportSummaryStatus &status = summary_.status();
        status.state = ReportSummaryState::Fetching;
        status.active_spool = "Summary";
        status.error.clear();
        status.spool = fetch_.spool_status();
        summary_.give();
    }

    publish_json_snapshot();
    Log::logf(CAT_REPORT, LOG_INFO, "Summary refresh queued\n");
    return true;
}

ReportSummaryFetchEvent ReportSummaryService::poll(RpcArbiter &arbiter) {
    if (!fetch_.summary_active()) return ReportSummaryFetchEvent::None;

    fetch_.poll_spool(arbiter);

    bool publish_progress = false;
    const uint32_t now_ms = millis();
    if (summary_.take(0)) {
        ReportSummaryStatus &status = summary_.status();
        status.spool = fetch_.spool_status();
        status.elapsed_ms = fetch_.summary_elapsed_ms(now_ms);
        summary_.give();
        publish_progress = summary_.snapshot_progress_due(now_ms, 500);
    }
    if (publish_progress) publish_json_snapshot();

    if (fetch_.spool_complete()) return finish_fetch();
    if (fetch_.spool_failed()) {
        return fail_fetch(fetch_.spool_status().error.c_str());
    }
    return ReportSummaryFetchEvent::None;
}

bool ReportSummaryService::ensure_records() {
    if (!summary_.ensure_records()) {
        log_report_alloc_failed(
            "summary_records",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        (void)fail_fetch("summary_alloc_failed");
        return false;
    }

    return true;
}

bool ReportSummaryService::parse_result(ReportSpoolResult &result) {
    if (!ensure_records()) return false;

    ReportSummaryRecord *staging = nullptr;
    if (!summary_.take_scratch(portMAX_DELAY, staging)) {
        (void)fail_fetch("summary_staging_alloc_failed");
        return false;
    }

    memset(staging,
           0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));

    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;

    char error[64] = {};
    if (!report_parse_summary_spool(result,
                                    store_summary_record_to_buffer,
                                    &context,
                                    error,
                                    sizeof(error))) {
        summary_.give_scratch();
        (void)fail_fetch(error[0] ? error : "summary_parse_failed");
        return false;
    }

    size_t write_count = 0;
    if (!summary_.take(portMAX_DELAY)) {
        summary_.give_scratch();
        return false;
    }

    summary_.clear_records();
    summary_.replace_records_from(staging,
                                  context.count,
                                  context.nights_with_therapy);
    summary_.finalize_records();
    write_count = summary_.record_count();
    if (write_count) {
        memcpy(staging,
               summary_.records(),
               write_count * sizeof(ReportSummaryRecord));
    }
    summary_.give();

    if (write_count &&
        !ReportStore::write_summary_records(staging, write_count)) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Summary store write failed records=%lu\n",
                  static_cast<unsigned long>(write_count));
    }

    summary_.give_scratch();
    result_cache_.invalidate(0, true);
    return true;
}

bool ReportSummaryService::load_from_store() {
    if (!ensure_records()) return false;

    ReportSummaryRecord *staging = nullptr;
    if (!summary_.take_scratch(portMAX_DELAY, staging)) return false;

    memset(staging,
           0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));

    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;

    if (!ReportStore::read_summary_records(store_summary_record_to_buffer,
                                           &context)) {
        summary_.give_scratch();
        return false;
    }

    if (!summary_.take(portMAX_DELAY)) {
        summary_.give_scratch();
        return false;
    }

    summary_.clear_records();
    summary_.replace_records_from(staging,
                                  context.count,
                                  context.nights_with_therapy);
    summary_.finalize_records();

    ReportSummaryStatus &status = summary_.status();
    status.state = ReportSummaryState::Ready;
    status.revision++;
    status.error.clear();
    status.active_spool.clear();

    const uint32_t records_total = status.records_total;
    const uint32_t nights_with_therapy = status.nights_with_therapy;
    summary_.give();

    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Summary loaded from store records=%lu "
              "therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));

    summary_.give_scratch();
    publish_json_snapshot();
    result_cache_.invalidate(0, true);
    return true;
}

ReportSummaryFetchEvent ReportSummaryService::finish_fetch() {
    ReportSpoolResult result;
    fetch_.move_spool_result_to(result);
    fetch_.finish_summary_fetch();

    if (summary_.take(portMAX_DELAY)) {
        ReportSummaryStatus &status = summary_.status();
        status.active_spool.clear();
        status.spool = fetch_.spool_status();
        summary_.give();
    }

    if (!parse_result(result)) return ReportSummaryFetchEvent::Failed;

    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    if (summary_.take(portMAX_DELAY)) {
        ReportSummaryStatus &status = summary_.status();
        status.state = ReportSummaryState::Ready;
        status.revision++;
        status.error.clear();
        records_total = status.records_total;
        nights_with_therapy = status.nights_with_therapy;
        summary_.give();
    }

    publish_json_snapshot();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Summary ready records=%lu therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));
    return ReportSummaryFetchEvent::Completed;
}

ReportSummaryFetchEvent ReportSummaryService::fail_fetch(
    const char *message) {
    fetch_.finish_summary_fetch();

    std::string error;
    if (summary_.take(portMAX_DELAY)) {
        ReportSummaryStatus &status = summary_.status();
        status.state = ReportSummaryState::Error;
        status.revision++;
        status.active_spool.clear();
        status.error = message ? message : "summary_error";
        status.spool = fetch_.spool_status();
        error = status.error;
        summary_.give();
    } else {
        error = message ? message : "summary_error";
    }

    Log::logf(CAT_REPORT, LOG_WARN, "Summary failed: %s\n", error.c_str());
    publish_json_snapshot();
    return ReportSummaryFetchEvent::Failed;
}

ReportSummaryStatus ReportSummaryService::status() const {
    ReportSummaryStatus snapshot;
    if (!summary_.take(pdMS_TO_TICKS(20))) {
        snapshot.state = ReportSummaryState::Error;
        snapshot.error = "summary_busy";
        return snapshot;
    }

    snapshot = summary_.status();
    summary_.give();
    return snapshot;
}

ReportSummarySnapshotResult ReportSummaryService::publish_json_snapshot() {
    snapshot_error_[0] = '\0';

    ReportSummaryStatus status_snapshot;
    uint32_t data_epoch_snapshot = 0;

    if (summary_.take(portMAX_DELAY)) {
        status_snapshot = summary_.status();
        data_epoch_snapshot = night_index_.data_epoch();
        summary_.give();
    } else {
        strlcpy(snapshot_error_, "summary_busy", sizeof(snapshot_error_));
        return ReportSummarySnapshotResult::Busy;
    }

    ReportNightIndexSnapshotRef indexed_snapshot;
    const char *index_error = nullptr;
    const ReportNightIndexSnapshotResult index_result =
        night_index_service_.snapshot(indexed_snapshot, &index_error);
    if (index_result == ReportNightIndexSnapshotResult::Busy) {
        strlcpy(snapshot_error_,
                index_error ? index_error : "night_index_busy",
                sizeof(snapshot_error_));
        return ReportSummarySnapshotResult::Busy;
    }
    if (index_result != ReportNightIndexSnapshotResult::Ready ||
        !indexed_snapshot) {
        strlcpy(snapshot_error_,
                index_error ? index_error : "night_index_failed",
                sizeof(snapshot_error_));

        if (!summary_.snapshot_available()) {
            LargeTextBuffer error_json;
            error_json =
                "{\"state\":\"error\",\"error\":\"summary_snapshot_";
            error_json += snapshot_error_;
            error_json += "\",\"nights\":[]}";
            summary_.publish_snapshot(error_json);
        }
        return ReportSummarySnapshotResult::Failed;
    }

    status_snapshot.records_total =
        static_cast<uint32_t>(indexed_snapshot->count());
    status_snapshot.nights_with_therapy =
        static_cast<uint32_t>(indexed_snapshot->therapy_night_count());

    LargeTextBuffer summary_json_build;
    const bool json_ok = report_append_summary_json_from_snapshot(
        summary_json_build,
        status_snapshot,
        data_epoch_snapshot,
        *indexed_snapshot);

    if (!json_ok || summary_json_build.overflowed()) {
        strlcpy(snapshot_error_,
                "summary_json_alloc",
                sizeof(snapshot_error_));
        if (!summary_.snapshot_available()) {
            summary_json_build =
                "{\"state\":\"error\",\"error\":"
                "\"summary_json_alloc\",\"nights\":[]}";
            summary_.publish_snapshot(summary_json_build);
        }
        return ReportSummarySnapshotResult::Failed;
    }

    summary_.publish_snapshot(summary_json_build);
    return ReportSummarySnapshotResult::Published;
}

void ReportSummaryService::build_json(LargeTextBuffer &json) const {
    summary_.build_snapshot_json(json);
}

ReportSummaryStatus ReportManager::summary_status() const {
    return summary_service_.status();
}

void ReportManager::build_summary_json(LargeTextBuffer &json) const {
    summary_service_.build_json(json);
}

}  // namespace aircannect
