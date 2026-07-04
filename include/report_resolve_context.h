#pragma once

#include <stddef.h>

#include "edf_report_catalog.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"
#include "report_source_resolver.h"

namespace aircannect {

class ScopedReportResolveContext {
public:
    explicit ScopedReportResolveContext(const char *context,
                                        bool include_sessions = true)
        : context_(context),
          include_sessions_(include_sessions),
          sessions_(static_cast<EdfReportSessionDescriptor *>(
              include_sessions
                  ? Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                                         sizeof(EdfReportSessionDescriptor),
                                         false)
                  : nullptr)),
          plan_(static_cast<ReportResolvedPlan *>(
              Memory::calloc_large(1, sizeof(ReportResolvedPlan), false))),
          scratch_(static_cast<ReportResolveScratch *>(
              Memory::calloc_large(1, sizeof(ReportResolveScratch), false))) {
        if ((include_sessions_ && !sessions_) || !plan_ || !scratch_) {
            size_t bytes = sizeof(ReportResolvedPlan) +
                           sizeof(ReportResolveScratch);
            if (include_sessions_) {
                bytes += AC_REPORT_EDF_SESSION_MAX *
                         sizeof(EdfReportSessionDescriptor);
            }
            log_report_alloc_failed(context_, bytes);
        }
    }

    ~ScopedReportResolveContext() {
        Memory::free(scratch_);
        Memory::free(plan_);
        Memory::free(sessions_);
    }

    ScopedReportResolveContext(const ScopedReportResolveContext &) = delete;
    ScopedReportResolveContext &operator=(const ScopedReportResolveContext &) =
        delete;

    explicit operator bool() const {
        return (!include_sessions_ || sessions_) && plan_ && scratch_;
    }

    EdfReportSessionDescriptor *sessions() { return sessions_; }
    ReportResolvedPlan &plan() { return *plan_; }
    ReportResolveScratch &scratch() { return *scratch_; }

private:
    const char *context_ = nullptr;
    bool include_sessions_ = true;
    EdfReportSessionDescriptor *sessions_ = nullptr;
    ReportResolvedPlan *plan_ = nullptr;
    ReportResolveScratch *scratch_ = nullptr;
};

}  // namespace aircannect
