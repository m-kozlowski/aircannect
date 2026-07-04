#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_proto.h"
#include "report_summary_types.h"

namespace aircannect {

class ReportSummaryRecordStore {
public:
    ~ReportSummaryRecordStore();

    bool ensure();
    void release();
    void clear();

    ReportSummaryRecord *data() { return records_; }
    const ReportSummaryRecord *data() const { return records_; }

    size_t count() const { return count_; }
    uint32_t nights_with_therapy() const { return nights_with_therapy_; }

    void replace_from(const ReportSummaryRecord *records,
                      size_t count,
                      uint32_t nights_with_therapy);
    void sort_by_start();
    void apply_counts_to(ReportSummaryStatus &status) const;

private:
    ReportSummaryRecord *records_ = nullptr;
    size_t count_ = 0;
    uint32_t nights_with_therapy_ = 0;
};

}  // namespace aircannect
