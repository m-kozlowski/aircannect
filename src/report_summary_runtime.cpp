#include "report_summary_runtime.h"

namespace aircannect {

void ReportSummaryRuntime::begin() {
    status_.begin();
    scratch_.begin();
    coverage_marks_.begin();
}

bool ReportSummaryRuntime::take(TickType_t timeout) const {
    return status_.take(timeout);
}

void ReportSummaryRuntime::give() const {
    status_.give();
}

ReportSummaryStatus &ReportSummaryRuntime::status() {
    return status_.status();
}

const ReportSummaryStatus &ReportSummaryRuntime::status() const {
    return status_.status();
}

void ReportSummaryRuntime::reset_status() {
    status_.reset_status();
}

void ReportSummaryRuntime::publish_revision() {
    status_.publish_revision();
}

uint32_t ReportSummaryRuntime::revision() const {
    return status_.revision();
}

bool ReportSummaryRuntime::ensure_records() {
    return records_.ensure();
}

void ReportSummaryRuntime::clear_records() {
    records_.clear();
}

ReportSummaryRecord *ReportSummaryRuntime::records() {
    return records_.data();
}

const ReportSummaryRecord *ReportSummaryRuntime::records() const {
    return records_.data();
}

size_t ReportSummaryRuntime::record_count() const {
    return records_.count();
}

uint32_t ReportSummaryRuntime::nights_with_therapy() const {
    return records_.nights_with_therapy();
}

int64_t ReportSummaryRuntime::night_start_for_timestamp(
    int64_t timestamp_ms) const {
    if (!take(portMAX_DELAY)) return timestamp_ms;

    const ReportSummaryRecord *records = records_.data();
    const size_t count = records_.count();
    if (!records || count == 0) {
        give();
        return timestamp_ms;
    }

    int64_t nearest_start = 0;
    bool have_nearest = false;
    for (size_t i = 0; i < count; ++i) {
        const ReportSummaryRecord &record = records[i];
        if (!record.valid || !record.duration_min) continue;

        const int64_t start_ms = static_cast<int64_t>(record.start_ms);
        const int64_t end_ms = static_cast<int64_t>(record.end_ms);
        if (timestamp_ms >= start_ms && timestamp_ms < end_ms) {
            give();
            return start_ms;
        }
        if (start_ms <= timestamp_ms &&
            (!have_nearest || start_ms > nearest_start)) {
            nearest_start = start_ms;
            have_nearest = true;
        }
    }

    const int64_t result = have_nearest ? nearest_start : timestamp_ms;
    give();
    return result;
}

void ReportSummaryRuntime::replace_records_from(
    const ReportSummaryRecord *records,
    size_t count,
    uint32_t nights_with_therapy) {
    records_.replace_from(records, count, nights_with_therapy);
}

void ReportSummaryRuntime::sort_records_by_start() {
    records_.sort_by_start();
}

void ReportSummaryRuntime::apply_record_counts_to_status() {
    records_.apply_counts_to(status_.status());
}

void ReportSummaryRuntime::finalize_records() {
    sort_records_by_start();
    apply_record_counts_to_status();
}

bool ReportSummaryRuntime::take_scratch(TickType_t timeout,
                                        ReportSummaryRecord *&out) {
    return scratch_.take(timeout, out);
}

void ReportSummaryRuntime::give_scratch() {
    scratch_.give();
}

void ReportSummaryRuntime::request_snapshot_publish() {
    snapshot_.request_publish();
}

void ReportSummaryRuntime::publish_snapshot(LargeTextBuffer &build_buffer) {
    snapshot_.publish(build_buffer);
}

void ReportSummaryRuntime::publish_snapshot_fallback(
    LargeTextBuffer &build_buffer) {
    snapshot_.publish_fallback(build_buffer);
}

void ReportSummaryRuntime::build_snapshot_json(LargeTextBuffer &json) const {
    snapshot_.build_json(json);
}

bool ReportSummaryRuntime::snapshot_available() const {
    return snapshot_.available();
}

bool ReportSummaryRuntime::snapshot_publish_pending() const {
    return snapshot_.publish_pending();
}

uint32_t ReportSummaryRuntime::snapshot_generation() const {
    return snapshot_.requested_generation();
}

void ReportSummaryRuntime::begin_snapshot_progress(uint32_t now_ms,
                                                   uint32_t interval_ms) {
    snapshot_.begin_progress(now_ms, interval_ms);
}

bool ReportSummaryRuntime::snapshot_progress_due(uint32_t now_ms,
                                                 uint32_t interval_ms) {
    return snapshot_.progress_due(now_ms, interval_ms);
}

bool ReportSummaryRuntime::reset_coverage_marks() {
    return coverage_marks_.reset();
}

void ReportSummaryRuntime::note_coverage_chunk(
    const ReportParsedChunk &chunk) {
    coverage_marks_.note_chunk(records_.data(), records_.count(), chunk);
}

int64_t ReportSummaryRuntime::coverage_extent(size_t record_index) const {
    return coverage_marks_.extent(record_index);
}

}  // namespace aircannect
