#pragma once

#include "report_records.h"
#include "report_spool_types.h"

namespace aircannect {

bool report_event_seen(const ReportSpoolBuffer &seen,
                       const ReportEventRecord &event);
bool remember_report_event(ReportSpoolBuffer &seen,
                           const ReportEventRecord &event);

}  // namespace aircannect
