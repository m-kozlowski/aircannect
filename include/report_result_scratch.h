#pragma once

#include <stddef.h>

#include "edf_report_catalog.h"
#include "report_manager_internal_types.h"
#include "report_materializer.h"
#include "report_night_index.h"
#include "report_source_resolver.h"

namespace aircannect {

class ReportResultScratch {
public:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ~ReportResultScratch();

    bool ensure_chunks();
    bool ensure_edf_sessions();
    bool ensure_resolve_buffers();
    bool ensure_prepare_indexed_night();

    void clear_chunks();
    void release_edf_sessions();
    void release();

    ReportResultChunk *chunks() { return chunks_; }
    const ReportResultChunk *chunks() const { return chunks_; }
    size_t chunk_capacity() const { return chunk_capacity_; }

    EdfReportSessionDescriptor *edf_sessions() { return edf_sessions_; }
    const EdfReportSessionDescriptor *edf_sessions() const {
        return edf_sessions_;
    }
    size_t &edf_session_count() { return edf_session_count_; }
    size_t edf_session_count() const { return edf_session_count_; }

    ReportResolvedPlan *resolved_plan() { return resolved_plan_; }
    ReportResolveScratch *resolve_scratch() { return resolve_scratch_; }
    ReportIndexedNight *prepare_indexed_night() {
        return prepare_indexed_night_;
    }

private:
    ReportResultChunk *chunks_ = nullptr;
    size_t chunk_capacity_ = 0;

    EdfReportSessionDescriptor *edf_sessions_ = nullptr;
    size_t edf_session_count_ = 0;

    ReportResolvedPlan *resolved_plan_ = nullptr;
    ReportResolveScratch *resolve_scratch_ = nullptr;
    ReportIndexedNight *prepare_indexed_night_ = nullptr;
};

}  // namespace aircannect
