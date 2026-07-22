#pragma once

#include <stdint.h>

#include "large_text_buffer.h"
#include "report_fetch_runtime.h"
#include "report_night_index_runtime.h"
#include "report_night_index_service.h"
#include "report_result_cache_runtime.h"
#include "report_summary_runtime.h"

namespace aircannect {

enum class ReportSummaryFetchEvent : uint8_t {
    None,
    Completed,
    Failed,
};

enum class ReportSummarySnapshotResult : uint8_t {
    Published,
    Busy,
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
    ReportSummaryFetchEvent poll();

    ReportSummaryStatus status() const;
    void build_json(LargeTextBuffer &json) const;
    void request_json_snapshot_publish();
    bool json_snapshot_publish_pending() const;
    uint32_t json_snapshot_generation() const;
    ReportSummarySnapshotResult publish_json_snapshot();
    const char *snapshot_error() const { return snapshot_error_; }

private:
    bool ensure_records();
    bool parse_result(ReportSpoolResult &result);
    bool load_from_store();
    ReportSummaryFetchEvent finish_fetch();
    ReportSummaryFetchEvent fail_fetch(const char *message);
    void publish_changed_json_snapshot();

    ReportSummaryRuntime &summary_;
    ReportFetchRuntime &fetch_;
    ReportNightIndexRuntime &night_index_;
    ReportNightIndexService &night_index_service_;
    ReportResultCacheRuntime &result_cache_;
    char snapshot_error_[48] = {};
};

}  // namespace aircannect
