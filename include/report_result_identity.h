#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_night_index.h"
#include "report_summary_types.h"

namespace aircannect {

class ReportResultIdentity {
public:
    ReportResultIdentity() = default;
    ~ReportResultIdentity();
    ReportResultIdentity(const ReportResultIdentity &) = delete;
    ReportResultIdentity &operator=(const ReportResultIdentity &) = delete;

    void clear();
    bool set(const ReportIndexedNight &night, const char *etag);

    bool valid() const;
    bool etag_matches(const char *etag) const;

    const ReportIndexedNight &indexed_night() const { return *indexed_night_; }
    const ReportSummaryRecord &summary() const { return summary_; }
    const char *etag() const { return etag_; }
    uint64_t night_start_ms() const { return summary_.start_ms; }

private:
    ReportIndexedNight *indexed_night_ = nullptr;
    ReportSummaryRecord summary_;
    char etag_[AC_REPORT_RESULT_ETAG_MAX] = {};
};

void report_format_result_etag(const ReportSummaryRecord &record,
                               uint64_t source_signature,
                               uint32_t night_epoch,
                               char *out,
                               size_t out_size);

}  // namespace aircannect
