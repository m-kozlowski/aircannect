#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_night_index.h"
#include "report_night_index_service.h"
#include "report_summary_types.h"

namespace aircannect {

struct ReportSummaryNight {
    size_t summary_index = 0;
    size_t therapy_index = 0;
    ReportSummaryRecord record;
};

using ReportSummaryNightCallback =
    bool (*)(void *context, const ReportSummaryNight &night);

class ReportNightQueryService {
public:
    explicit ReportNightQueryService(ReportNightIndexService &night_index);

    bool night_etag(size_t therapy_index, char *out, size_t out_size) const;

    bool for_each_summary_night(ReportSummaryNightCallback callback,
                                void *context) const;
    bool summary_night_by_therapy_index(size_t therapy_index,
                                        ReportSummaryRecord &out) const;
    bool latest_summary_night(ReportSummaryRecord &out) const;

private:
    ReportNightIndexService &night_index_;
};

}  // namespace aircannect
