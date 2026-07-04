#include "report_result_runtime.h"

#include "report_result_cache_runtime.h"
#include "report_result_json.h"

namespace aircannect {

ReportResultStatus ReportResultRuntime::status_with_diagnostics(
    const ReportResultCacheRuntime &cache) const {
    ReportResultStatus copy = status_;
    cache.apply_diagnostics(copy);

    return copy;
}

bool ReportResultRuntime::ensure_chunks() {
    return scratch_.ensure_chunks();
}

bool ReportResultRuntime::ensure_edf_sessions() {
    return scratch_.ensure_edf_sessions();
}

bool ReportResultRuntime::ensure_resolve_buffers() {
    return scratch_.ensure_resolve_buffers();
}

bool ReportResultRuntime::uses_edf_provider() const {
    const ReportResultChunk *chunks = scratch_.chunks();
    if (!chunks) return false;

    for (uint32_t i = 0; i < status_.chunk_count; ++i) {
        if (chunks[i].provider_ref.provider == ReportProviderId::Edf) {
            return true;
        }
    }

    return false;
}

void ReportResultRuntime::release_edf_sessions() {
    scratch_.release_edf_sessions();
}

void ReportResultRuntime::clear_ranges() {
    ranges_.clear();
}

bool ReportResultRuntime::set_ranges_from_indexed_night(
    const ReportIndexedNight &night) {
    return ranges_.set_from_indexed_night(night);
}

bool ReportResultRuntime::set_ranges_from_edf_sessions() {
    return ranges_.set_from_edf_sessions(scratch_.edf_sessions(),
                                         scratch_.edf_session_count());
}

bool ReportResultRuntime::data_span(int64_t &span_start_ms,
                                    int64_t &span_end_ms) const {
    return ranges_.data_span(identity_.indexed_night(),
                             identity_.summary(),
                             span_start_ms,
                             span_end_ms);
}

void ReportResultRuntime::clear_prepare_state() {
    scratch_.clear_chunks();
    streams_.clear();
    scratch_.release_edf_sessions();
    identity_.clear();
    ranges_.clear();
    plot_.result_bin.clear();
    plot_.skip_cache = false;
    status_ = {};
}

void ReportResultRuntime::mark_error(const char *message) {
    status_.state = ReportResultState::Error;
    status_.error = message ? message : "result_prepare_failed";
}

const char *ReportResultRuntime::state_name() const {
    return report_result_state_name(status_.state);
}

bool ReportResultRuntime::current_result_publishable(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *etag,
    bool refresh_cache) const {
    // Idempotent re-prepare: the ETag check is required because another EDF
    // session can be appended to the same noon-noon night without changing the
    // Summary start/end identity.
    if (refresh_cache || !etag || !etag[0]) return false;
    if (!identity_.etag_matches(etag)) return false;
    if (status_.state != ReportResultState::Ready &&
        status_.state != ReportResultState::Partial) {
        return false;
    }

    return status_.therapy_index == therapy_index &&
           status_.chunk_count > 0 &&
           status_.night_start_ms == night.summary.start_ms &&
           plot_.result_bin.size() > 0;
}

bool ReportResultRuntime::add_stream(ReportStoreChunkKind kind,
                                     ReportSourceId source,
                                     ReportSignalId signal,
                                     const char *name,
                                     bool required,
                                     bool complete,
                                     size_t &stream_index) {
    if (!name || !name[0]) return false;

    return streams_.add_or_update(kind,
                                  source,
                                  signal,
                                  name,
                                  required,
                                  complete,
                                  status_,
                                  stream_index);
}

}  // namespace aircannect
