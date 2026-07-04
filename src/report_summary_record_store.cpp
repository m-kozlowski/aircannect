#include "report_summary_record_store.h"

#include <algorithm>
#include <string.h>

#include "memory_manager.h"

namespace aircannect {

ReportSummaryRecordStore::~ReportSummaryRecordStore() {
    release();
}

bool ReportSummaryRecordStore::ensure() {
    if (records_) return true;

    records_ = static_cast<ReportSummaryRecord *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportSummaryRecord),
                             false));
    return records_ != nullptr;
}

void ReportSummaryRecordStore::release() {
    Memory::free(records_);
    records_ = nullptr;
    count_ = 0;
    nights_with_therapy_ = 0;
}

void ReportSummaryRecordStore::clear() {
    if (records_) {
        memset(records_,
               0,
               AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    }

    count_ = 0;
    nights_with_therapy_ = 0;
}

void ReportSummaryRecordStore::replace_from(
    const ReportSummaryRecord *records,
    size_t count,
    uint32_t nights_with_therapy) {
    clear();

    if (records && count > 0) {
        memcpy(records_,
               records,
               count * sizeof(ReportSummaryRecord));
    }

    count_ = count;
    nights_with_therapy_ = nights_with_therapy;
}

void ReportSummaryRecordStore::sort_by_start() {
    if (!records_ || count_ <= 1) return;

    std::sort(records_,
              records_ + count_,
              [](const ReportSummaryRecord &a,
                 const ReportSummaryRecord &b) {
                  return a.start_ms < b.start_ms;
              });
}

void ReportSummaryRecordStore::apply_counts_to(
    ReportSummaryStatus &status) const {
    status.records_total = static_cast<uint32_t>(count_);
    status.nights_with_therapy = nights_with_therapy_;
}

}  // namespace aircannect
