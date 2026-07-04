#include "report_build_runtime.h"

namespace aircannect {

bool ReportBuildRuntime::begin() {
    return queue_.begin();
}

ReportBuildQueueSnapshot ReportBuildRuntime::snapshot() const {
    return queue_.snapshot();
}

ReportBuildRuntime::BuildQueueResult ReportBuildRuntime::enqueue(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh,
    bool idle_prebuild) {
    return queue_.enqueue(night_start_ms,
                          therapy_index,
                          refresh,
                          idle_prebuild);
}

bool ReportBuildRuntime::has_capacity() const {
    return queue_.has_capacity();
}

bool ReportBuildRuntime::has_pending() const {
    return queue_.has_pending();
}

void ReportBuildRuntime::clear(uint64_t night_start_ms, bool all) {
    queue_.clear(night_start_ms, all);
}

void ReportBuildRuntime::note_read(const char *state) {
    queue_.note_read(state);
}

void ReportBuildRuntime::note_service_block(const char *reason) {
    queue_.note_service_block(reason);
}

bool ReportBuildRuntime::peek_head(ResultBuildJob &out) const {
    return queue_.peek_head(out);
}

void ReportBuildRuntime::note_service_started() {
    queue_.note_service_started();
}

void ReportBuildRuntime::note_build_result(const ResultBuildJob &job,
                                           const char *outcome,
                                           const char *state,
                                           const char *error) {
    queue_.note_build_result(job, outcome, state, error);
}

bool ReportBuildRuntime::defer_head(const ResultBuildJob &job,
                                    uint32_t next_attempt_ms) {
    return queue_.defer_head(job, next_attempt_ms);
}

bool ReportBuildRuntime::pop_head(const ResultBuildJob &job) {
    return queue_.pop_head(job);
}

bool ReportBuildRuntime::prebuild_key_matches(
    const ReportNightIndexCacheKey &key) const {
    return prebuild_.key_matches(key);
}

void ReportBuildRuntime::reset_prebuild_for_key(
    const ReportNightIndexCacheKey &key) {
    prebuild_.reset_for_key(key);
}

bool ReportBuildRuntime::prebuild_rescan_delay_active(
    uint32_t now_ms) const {
    return prebuild_.rescan_delay_active(now_ms);
}

void ReportBuildRuntime::mark_prebuild_drained(uint32_t now_ms,
                                               uint32_t delay_ms) {
    prebuild_.mark_drained(now_ms, delay_ms);
}

size_t ReportBuildRuntime::prebuild_cursor() const {
    return prebuild_.cursor();
}

void ReportBuildRuntime::advance_prebuild_cursor() {
    prebuild_.advance_cursor();
}

void ReportBuildRuntime::rewind_prebuild_cursor() {
    prebuild_.rewind_cursor();
}

}  // namespace aircannect
