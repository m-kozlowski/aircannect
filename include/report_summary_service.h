#pragma once

#include <stdint.h>

#include "large_text_buffer.h"
#include "report_fetch_runtime.h"
#include "report_night_index_runtime.h"
#include "report_night_index_service.h"
#include "report_result_cache_runtime.h"
#include "report_summary_runtime.h"
#include "rpc_arbiter.h"

namespace aircannect {

enum class ReportSummaryFetchEvent : uint8_t {
    None,
    Completed,
    Failed,
};

class ReportSummaryService {
public:
    ReportSummaryService(ReportSummaryRuntime &summary,
                         ReportFetchRuntime &fetch,
                         ReportNightIndexRuntime &night_index,
                         ReportNightIndexService &night_index_service,
                         ReportResultCacheRuntime &result_cache);

    void begin();
    void load_initial_snapshot();

    bool active() const { return fetch_.summary_active(); }
    bool request_refresh(bool force, bool cache_fetch_active);
    ReportSummaryFetchEvent poll(RpcArbiter &arbiter);

    ReportSummaryStatus status() const;
    void build_json(LargeTextBuffer &json) const;
    bool publish_json_snapshot();

private:
    bool ensure_records();
    bool parse_result(ReportSpoolResult &result);
    bool load_from_store();
    ReportSummaryFetchEvent finish_fetch();
    ReportSummaryFetchEvent fail_fetch(const char *message);

    ReportSummaryRuntime &summary_;
    ReportFetchRuntime &fetch_;
    ReportNightIndexRuntime &night_index_;
    ReportNightIndexService &night_index_service_;
    ReportResultCacheRuntime &result_cache_;
};

}  // namespace aircannect
