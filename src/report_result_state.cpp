#include "report_manager.h"

#include <algorithm>
#include <new>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"
#include "report_result_json.h"

namespace aircannect {

bool ReportManager::ensure_result_chunks() {
    if (result_chunks_) return true;
    result_chunks_ = static_cast<ReportResultChunk *>(
        Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                             sizeof(ReportResultChunk),
                             false));
    if (!result_chunks_) {
        log_report_alloc_failed(
            "result_chunks",
            AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
        fail_result_prepare("result_manifest_alloc_failed");
        return false;
    }
    result_chunk_capacity_ = AC_REPORT_RESULT_CHUNK_MAX;
    return true;
}

bool ReportManager::ensure_result_slots() {
    if (!result_slots_lock_) {
        result_slots_lock_ = xSemaphoreCreateMutex();
        if (!result_slots_lock_) {
            log_report_alloc_failed("result_slots_lock", 0);
            return false;
        }
    }
    if (result_slots_) return true;
    result_slots_ = static_cast<MaterializedResult *>(Memory::alloc_large(
        AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult), false));
    if (!result_slots_) {
        log_report_alloc_failed(
            "result_slots",
            AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult));
        return false;
    }
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        new (&result_slots_[i]) MaterializedResult();
    }
    return true;
}

bool ReportManager::ensure_result_edf_sessions() {
    if (result_edf_sessions_) return true;
    result_edf_sessions_ = static_cast<EdfReportSessionDescriptor *>(
        Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                             sizeof(EdfReportSessionDescriptor),
                             false));
    if (!result_edf_sessions_) {
        log_report_alloc_failed(
            "result_edf_sessions",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    return true;
}

bool ReportManager::result_uses_edf_provider() const {
    if (!result_chunks_) return false;
    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        if (result_chunks_[i].provider_ref.provider == ReportProviderId::Edf) {
            return true;
        }
    }
    return false;
}

void ReportManager::release_result_edf_sessions() {
    Memory::free(result_edf_sessions_);
    result_edf_sessions_ = nullptr;
    result_edf_session_count_ = 0;
}

void ReportManager::clear_result_ranges() {
    memset(result_ranges_, 0, sizeof(result_ranges_));
    result_range_count_ = 0;
}

bool ReportManager::set_result_ranges_from_indexed_night(
    const ReportIndexedNight &night) {
    clear_result_ranges();
    ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t range_count =
        collect_indexed_night_report_ranges(night,
                                            ranges,
                                            AC_REPORT_SUMMARY_SESSION_MAX);
    for (size_t i = 0; i < range_count; ++i) {
        if (result_range_count_ >= AC_REPORT_SUMMARY_SESSION_MAX) break;
        PlotRange &range = result_ranges_[result_range_count_++];
        range.start_ms = ranges[i].start_ms;
        range.end_ms = ranges[i].end_ms;
    }
    std::sort(result_ranges_,
              result_ranges_ + result_range_count_,
              [](const PlotRange &a, const PlotRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return result_range_count_ > 0;
}

bool ReportManager::set_result_ranges_from_edf_sessions() {
    if (!result_edf_sessions_ || result_edf_session_count_ == 0) return false;
    clear_result_ranges();
    for (size_t i = 0; i < result_edf_session_count_ &&
                       result_range_count_ < AC_REPORT_SUMMARY_SESSION_MAX;
         ++i) {
        const EdfReportSessionDescriptor &session = result_edf_sessions_[i];
        if (!edf_session_has_report_numeric(session)) continue;
        if (session.earliest_header_start_ms <= 0 ||
            session.latest_header_end_ms <= session.earliest_header_start_ms) {
            continue;
        }
        PlotRange &range = result_ranges_[result_range_count_++];
        range.start_ms = session.earliest_header_start_ms;
        range.end_ms = session.latest_header_end_ms;
    }
    std::sort(result_ranges_,
              result_ranges_ + result_range_count_,
              [](const PlotRange &a,
                 const PlotRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return result_range_count_ > 0;
}

bool ReportManager::result_data_span(int64_t &span_start_ms,
                                     int64_t &span_end_ms) const {
    if (result_range_count_ == 0) {
        return indexed_night_data_span(result_indexed_night_,
                                       span_start_ms,
                                       span_end_ms) ||
               night_data_span(result_night_, span_start_ms, span_end_ms);
    }
    span_start_ms = result_ranges_[0].start_ms;
    span_end_ms = result_ranges_[0].end_ms;
    for (size_t i = 1; i < result_range_count_; ++i) {
        span_start_ms = std::min(span_start_ms, result_ranges_[i].start_ms);
        span_end_ms = std::max(span_end_ms, result_ranges_[i].end_ms);
    }
    return span_end_ms > span_start_ms;
}

void ReportManager::clear_result_prepare() {
    reset_plot_build();
    reset_range_plot_build(true);
    if (result_chunks_ && result_chunk_capacity_) {
        memset(result_chunks_, 0,
               result_chunk_capacity_ * sizeof(ReportResultChunk));
    }
    memset(result_streams_, 0, sizeof(result_streams_));
    result_stream_count_ = 0;
    release_result_edf_sessions();
    result_indexed_night_ = {};
    result_night_ = {};
    result_etag_[0] = '\0';
    clear_result_ranges();
    result_plot_bin_.clear();
    result_skip_plot_cache_ = false;
    result_status_ = {};
}

void ReportManager::fail_result_prepare(const char *message) {
    reset_plot_build();
    result_status_.state = ReportResultState::Error;
    result_status_.error = message ? message : "result_prepare_failed";
    if (result_night_.start_ms != 0 && result_status_.night_start_ms != 0) {
        publish_result_to_slot();
    }
    release_result_edf_sessions();
    Log::logf(CAT_REPORT, LOG_WARN, "Result prepare failed: %s\n",
              result_status_.error.c_str());
}

const char *ReportManager::result_state_name() const {
    return report_result_state_name(result_status_.state);
}

bool ReportManager::add_result_stream(ReportStoreChunkKind kind,
                                      ReportSourceId source,
                                      ReportSignalId signal,
                                      const char *name,
                                      bool required,
                                      bool complete,
                                      size_t &stream_index) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < result_stream_count_; ++i) {
        ReportResultStream &stream = result_streams_[i];
        if (stream.kind == kind && stream.signal == signal &&
            stream.name && strcmp(stream.name, name) == 0) {
            stream_index = i;
            if (required) stream.required = true;
            if (!complete && stream.complete) {
                stream.complete = false;
                if (stream.required) result_status_.missing_streams++;
            }
            if (stream.chunk_count == 0) {
                stream.source = source;
            }
            return true;
        }
    }

    if (result_stream_count_ >= AC_REPORT_RESULT_STREAM_MAX) {
        fail_result_prepare("result_streams_full");
        return false;
    }
    stream_index = result_stream_count_;
    ReportResultStream &stream = result_streams_[result_stream_count_++];
    stream.kind = kind;
    stream.source = source;
    stream.signal = signal;
    stream.name = name;
    stream.required = required;
    stream.complete = complete;
    result_status_.stream_count =
        static_cast<uint32_t>(result_stream_count_);
    if (required && !complete) result_status_.missing_streams++;
    return true;
}


}  // namespace aircannect
