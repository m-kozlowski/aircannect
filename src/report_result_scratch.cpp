#include "report_result_scratch.h"

#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_manager_limits.h"

namespace aircannect {

ReportResultScratch::~ReportResultScratch() {
    release();
}

bool ReportResultScratch::ensure_chunks() {
    if (chunks_) return true;

    chunks_ = static_cast<ReportResultChunk *>(
        Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                             sizeof(ReportResultChunk),
                             false));
    if (!chunks_) {
        log_report_alloc_failed(
            "result_chunks",
            AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
        return false;
    }

    chunk_capacity_ = AC_REPORT_RESULT_CHUNK_MAX;
    return true;
}

bool ReportResultScratch::ensure_edf_sessions() {
    if (edf_sessions_) return true;

    edf_sessions_ = static_cast<EdfReportSessionDescriptor *>(
        Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                             sizeof(EdfReportSessionDescriptor),
                             false));
    if (!edf_sessions_) {
        log_report_alloc_failed(
            "result_edf_sessions",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    return true;
}

bool ReportResultScratch::ensure_resolve_buffers() {
    if (!resolved_plan_) {
        resolved_plan_ = static_cast<ReportResolvedPlan *>(
            Memory::calloc_large(1, sizeof(ReportResolvedPlan), false));
        if (!resolved_plan_) {
            log_report_alloc_failed("result_resolved_plan",
                                    sizeof(ReportResolvedPlan));
            return false;
        }
    }

    if (!resolve_scratch_) {
        resolve_scratch_ = static_cast<ReportResolveScratch *>(
            Memory::calloc_large(1, sizeof(ReportResolveScratch), false));
        if (!resolve_scratch_) {
            log_report_alloc_failed("result_resolve_scratch",
                                    sizeof(ReportResolveScratch));
            return false;
        }
    }

    return true;
}

bool ReportResultScratch::ensure_prepare_indexed_night() {
    if (prepare_indexed_night_) return true;

    prepare_indexed_night_ = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
    if (!prepare_indexed_night_) {
        log_report_alloc_failed("prepare_indexed_night",
                                sizeof(ReportIndexedNight));
        return false;
    }

    return true;
}

void ReportResultScratch::clear_chunks() {
    if (!chunks_ || !chunk_capacity_) return;

    memset(chunks_, 0, chunk_capacity_ * sizeof(ReportResultChunk));
}

void ReportResultScratch::release_edf_sessions() {
    Memory::free(edf_sessions_);
    edf_sessions_ = nullptr;
    edf_session_count_ = 0;
}

void ReportResultScratch::release() {
    Memory::free(chunks_);
    chunks_ = nullptr;
    chunk_capacity_ = 0;

    release_edf_sessions();

    Memory::free(resolved_plan_);
    resolved_plan_ = nullptr;

    Memory::free(resolve_scratch_);
    resolve_scratch_ = nullptr;

    Memory::free(prepare_indexed_night_);
    prepare_indexed_night_ = nullptr;
}

}  // namespace aircannect
