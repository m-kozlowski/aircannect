#include "report_manager.h"

#include <algorithm>
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

bool ReportManager::ensure_summary_records() {
    if (records_) return true;

    records_ = static_cast<ReportSummaryRecord *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportSummaryRecord),
                             false));
    if (!records_) {
        log_report_alloc_failed(
            "summary_records",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        fail_summary("summary_alloc_failed");
        return false;
    }

    return true;
}

bool ReportManager::parse_summary_result(ReportSpoolResult &result) {
    if (!ensure_summary_records()) return false;

    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) {
        fail_summary("summary_staging_alloc_failed");
        return false;
    }

    memset(staging, 0,
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
        give_summary_scratch();
        fail_summary(error[0] ? error : "summary_parse_failed");
        return false;
    }

    size_t write_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }

    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;

    finalize_summary_records();
    write_count = record_count_;
    if (write_count) {
        memcpy(staging, records_, write_count * sizeof(ReportSummaryRecord));
    }
    give_summary_lock();

    if (write_count &&
        !ReportStore::write_summary_records(staging, write_count)) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Summary store write failed records=%lu\n",
                  static_cast<unsigned long>(write_count));
    }

    give_summary_scratch();
    invalidate_materialized(0, true);
    return true;
}

bool ReportManager::load_summary_from_store() {
    if (!ensure_summary_records()) return false;

    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) return false;

    memset(staging, 0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));

    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;

    if (!ReportStore::read_summary_records(store_summary_record_to_buffer,
                                           &context)) {
        give_summary_scratch();
        return false;
    }

    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }

    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;

    finalize_summary_records();
    summary_status_.state = ReportSummaryState::Ready;
    summary_status_.revision++;
    summary_status_.error.clear();
    summary_status_.active_spool.clear();

    const uint32_t records_total = summary_status_.records_total;
    const uint32_t nights_with_therapy = summary_status_.nights_with_therapy;
    give_summary_lock();

    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Summary loaded from store records=%lu "
              "therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));

    give_summary_scratch();
    publish_summary_json_snapshot();
    invalidate_materialized(0, true);
    return true;
}

void ReportManager::finalize_summary_records() {
    if (records_ && record_count_ > 1) {
        std::sort(records_, records_ + record_count_,
                  [](const ReportSummaryRecord &a,
                     const ReportSummaryRecord &b) {
                      return a.start_ms < b.start_ms;
                  });
    }

    summary_status_.records_total = static_cast<uint32_t>(record_count_);
    summary_status_.nights_with_therapy = nights_with_therapy_;
}

void ReportManager::finish_summary_fetch() {
    ReportSpoolResult result;
    spool_.move_result_to(result);
    summary_fetch_active_ = false;

    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.active_spool.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }

    if (!parse_summary_result(result)) return;

    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Ready;
        summary_status_.revision++;
        summary_status_.error.clear();
        records_total = summary_status_.records_total;
        nights_with_therapy = summary_status_.nights_with_therapy;
        give_summary_lock();
    }

    publish_summary_json_snapshot();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Summary ready records=%lu therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));

    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        const bool refresh_cache = pending_result_refresh_cache_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index, refresh_cache);
    }
}

void ReportManager::fail_summary(const char *message) {
    summary_fetch_active_ = false;

    std::string error;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Error;
        summary_status_.revision++;
        summary_status_.active_spool.clear();
        summary_status_.error = message ? message : "summary_error";
        summary_status_.spool = spool_.status();
        error = summary_status_.error;
        give_summary_lock();
    } else {
        error = message ? message : "summary_error";
    }

    Log::logf(CAT_REPORT, LOG_WARN, "Summary failed: %s\n", error.c_str());

    publish_summary_json_snapshot();
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(error.c_str());
    }
}

void ReportManager::clear_summary_records() {
    if (records_) {
        memset(records_, 0,
               AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    }

    record_count_ = 0;
    nights_with_therapy_ = 0;
}

const char *ReportManager::summary_state_name() const {
    return report_summary_state_name(summary_status().state);
}

ReportSummaryStatus ReportManager::summary_status() const {
    ReportSummaryStatus snapshot;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        snapshot.state = ReportSummaryState::Error;
        snapshot.error = "summary_busy";
        return snapshot;
    }

    snapshot = summary_status_;
    give_summary_lock();
    return snapshot;
}

bool ReportManager::publish_summary_json_snapshot() {
    ReportIndexedNight *indexed_snapshot = nullptr;
    ReportSummaryStatus status_snapshot;
    uint32_t data_epoch_snapshot = 0;
    size_t record_count_snapshot = 0;
    bool ok = true;

    if (take_summary_lock(portMAX_DELAY)) {
        status_snapshot = summary_status_;
        data_epoch_snapshot = cache_data_epoch_;
        give_summary_lock();
    } else {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_busy";
        ok = false;
    }

    indexed_snapshot = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!indexed_snapshot) {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_snapshot_alloc";
        record_count_snapshot = 0;
        ok = false;
    } else if (!build_indexed_nights(indexed_snapshot,
                                     AC_REPORT_SUMMARY_RECORD_MAX,
                                     record_count_snapshot)) {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_snapshot_build";
        record_count_snapshot = 0;
        Memory::free(indexed_snapshot);
        indexed_snapshot = nullptr;
        ok = false;
    } else {
        status_snapshot.records_total =
            static_cast<uint32_t>(record_count_snapshot);
    }

    uint32_t nights_with_therapy = 0;
    for (size_t i = 0; indexed_snapshot && i < record_count_snapshot; ++i) {
        if (report_indexed_night_visible_in_summary(indexed_snapshot[i])) {
            nights_with_therapy++;
        }
    }
    status_snapshot.nights_with_therapy = nights_with_therapy;

    summary_json_build_.clear();
    report_append_summary_json_from_indexed(summary_json_build_,
                                            status_snapshot,
                                            data_epoch_snapshot,
                                            indexed_snapshot,
                                            record_count_snapshot);
    Memory::free(indexed_snapshot);

    if (summary_json_build_.overflowed()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Summary JSON snapshot allocation failed\n");
        summary_json_build_ =
            "{\"state\":\"error\",\"error\":\"summary_snapshot_alloc\","
            "\"nights\":[]}";
        ok = false;
    }

    summary_json_snapshot_.swap(summary_json_build_);
    return ok;
}

void ReportManager::build_summary_json(LargeTextBuffer &json) const {
    if (!summary_json_snapshot_.length()) {
        json = "{\"state\":\"idle\",\"error\":\"summary_snapshot_missing\","
               "\"nights\":[]}";
        return;
    }

    json.clear();
    json.append(summary_json_snapshot_.c_str(), summary_json_snapshot_.length());
}

}  // namespace aircannect
