#pragma once

#include <stdint.h>

#include "edf_str_session.h"

namespace aircannect {

struct ReportSummaryRecord;

struct EdfStrSummaryApplyResult {
    uint16_t day = 0;
    uint32_t values = 0;
};

bool edf_str_summary_sleep_day(const ReportSummaryRecord &record,
                               uint16_t &day);
bool edf_str_apply_summary_record(const ReportSummaryRecord &record,
                                  EdfStrSessionAccumulator &session,
                                  EdfStrSummaryApplyResult &result);

}  // namespace aircannect
