#include "report_cache_fetch_state.h"

namespace aircannect {

void ReportCacheFetchState::reset_for_night(const ReportSummaryRecord &night) {
    active_ = false;
    source_finalizing_ = false;
    night_ = night;
    source_count_ = 0;
    source_index_ = 0;
    finalizing_plan_ = {};

    status_ = {};
    status_.night_start_ms = night.start_ms;
    status_.night_end_ms = night.end_ms;
    status_.active = true;
}

bool ReportCacheFetchState::add_source(ReportSourceId source,
                                       int64_t from_ms) {
    for (size_t i = 0; i < source_count_; ++i) {
        ReportCacheSourcePlan &existing = plans_[i];
        if (existing.source != source) continue;

        if (from_ms < existing.from_ms) existing.from_ms = from_ms;
        return true;
    }

    if (source_count_ >= AC_REPORT_CACHE_SOURCE_MAX) return false;

    ReportCacheSourcePlan &plan = plans_[source_count_++];
    plan.source = source;
    plan.from_ms = from_ms;
    return true;
}

const ReportCacheSourcePlan *ReportCacheFetchState::current_source() const {
    if (source_index_ >= source_count_) return nullptr;
    return &plans_[source_index_];
}

void ReportCacheFetchState::mark_no_work() {
    active_ = false;
    status_.active = false;
    status_.revision++;
    status_.error.clear();
}

void ReportCacheFetchState::activate() {
    active_ = true;
    source_finalizing_ = false;
    status_.active = true;
    status_.source_count = static_cast<uint32_t>(source_count_);

    const ReportCacheSourcePlan *current = current_source();
    status_.active_source = current ? current->source : ReportSourceId::Summary;
    status_.source_index = static_cast<uint32_t>(source_index_);
}

void ReportCacheFetchState::update_active_source(
    ReportSourceId source,
    const SpoolClientStatus &spool) {
    status_.active_source = source;
    status_.source_index = static_cast<uint32_t>(source_index_);
    status_.spool = spool;
}

void ReportCacheFetchState::update_spool(const SpoolClientStatus &spool) {
    status_.spool = spool;
}

void ReportCacheFetchState::note_chunk_written() {
    status_.chunks_written++;
}

void ReportCacheFetchState::set_error(const char *message) {
    status_.error = message ? message : "";
}

void ReportCacheFetchState::begin_finalizing_current_source() {
    const ReportCacheSourcePlan *current = current_source();
    finalizing_plan_ = current ? *current : ReportCacheSourcePlan{};
    source_finalizing_ = true;
}

void ReportCacheFetchState::complete_finalizing_source() {
    source_finalizing_ = false;
    finalizing_plan_ = {};
    source_index_++;
}

void ReportCacheFetchState::finish(const SpoolClientStatus &spool) {
    active_ = false;
    source_finalizing_ = false;
    status_.active = false;
    status_.revision++;
    status_.error.clear();
    status_.spool = spool;
}

void ReportCacheFetchState::fail(const char *message,
                                 const SpoolClientStatus &spool) {
    active_ = false;
    source_finalizing_ = false;
    status_.active = false;
    status_.revision++;
    status_.error = message ? message : "cache_fetch_failed";
    status_.spool = spool;
}

void ReportCacheFetchState::cancel(const char *message,
                                   const SpoolClientStatus &spool) {
    fail(message ? message : "cancelled", spool);
}

}  // namespace aircannect
