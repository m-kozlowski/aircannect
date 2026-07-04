#include "report_night_index_runtime.h"

#include "report_result_identity.h"

namespace aircannect {

bool ReportNightIndexRuntime::begin() {
    const bool durable_ok = durable_.begin();
    const bool epochs_ok = epochs_.begin();
    return durable_ok && epochs_ok;
}

bool ReportNightIndexRuntime::load_durable() {
    return durable_.load();
}

bool ReportNightIndexRuntime::seed_from_durable(ReportNightIndex &index) const {
    return durable_.seed(index);
}

void ReportNightIndexRuntime::schedule_durable_save(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t content_crc) const {
    durable_.schedule_save(src, count, content_crc);
}

bool ReportNightIndexRuntime::service_durable_writer() {
    return durable_.service_writer();
}

void ReportNightIndexRuntime::clear_epochs() {
    epochs_.clear();
}

void ReportNightIndexRuntime::note_chunk_committed(uint64_t night_start_ms) {
    epochs_.note_chunk_committed(night_start_ms);
}

void ReportNightIndexRuntime::remove_night(uint64_t night_start_ms) {
    epochs_.remove_night(night_start_ms);
}

uint32_t ReportNightIndexRuntime::data_epoch() const {
    return epochs_.data_epoch();
}

uint32_t ReportNightIndexRuntime::night_epoch(uint64_t night_start_ms) const {
    return epochs_.night_epoch(night_start_ms);
}

void ReportNightIndexRuntime::format_result_etag(
    const ReportIndexedNight &night,
    char *out,
    size_t out_size) const {
    report_format_result_etag(night.summary,
                              night.source_signature,
                              night_epoch(night.summary.start_ms),
                              out,
                              out_size);
}

}  // namespace aircannect
