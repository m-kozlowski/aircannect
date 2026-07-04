#include "report_summary_scratch.h"

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_manager_limits.h"

namespace aircannect {

ReportSummaryScratch::~ReportSummaryScratch() {
    Memory::free(records_);
    records_ = nullptr;

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

void ReportSummaryScratch::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
}

bool ReportSummaryScratch::take(TickType_t timeout, ReportSummaryRecord *&out) {
    out = nullptr;
    if (!lock_ || xSemaphoreTake(lock_, timeout) != pdTRUE) {
        return false;
    }

    if (!records_) {
        records_ = static_cast<ReportSummaryRecord *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportSummaryRecord),
            false));
    }
    if (!records_) {
        xSemaphoreGive(lock_);
        log_report_alloc_failed(
            "summary_scratch",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        return false;
    }

    out = records_;
    return true;
}

void ReportSummaryScratch::give() {
    if (lock_) xSemaphoreGive(lock_);
}

}  // namespace aircannect
