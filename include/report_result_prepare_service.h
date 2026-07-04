#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_cache_fetch_service.h"
#include "report_manager_internal_types.h"
#include "report_night_index_service.h"
#include "report_result_build_service.h"
#include "report_result_cache_runtime.h"

namespace aircannect {

class ReportResultPrepareService {
public:
    using ResultPrepareOutcome =
        report_manager_internal::ResultPrepareOutcome;

    ReportResultPrepareService(ReportResultBuildService &result_build,
                               ReportResultCacheRuntime &result_cache,
                               ReportNightIndexService &night_index,
                               ReportCacheFetchService &cache_fetch);

    void clear_prepare();
    void fail_prepare(const char *message);

    ResultPrepareOutcome prepare_by_therapy_index(size_t therapy_index,
                                                  bool refresh_cache);
    ResultPrepareOutcome prepare_by_night_start(uint64_t night_start_ms,
                                                size_t therapy_index,
                                                bool refresh_cache);

private:
    bool refresh_cache_if_needed(const ReportIndexedNight &night,
                                 size_t therapy_index,
                                 bool refresh_cache,
                                 bool &deferred);

    ReportResultRuntime &runtime() { return result_build_.runtime(); }
    const ReportResultRuntime &runtime() const {
        return result_build_.runtime();
    }

    ReportResultBuildService &result_build_;
    ReportResultCacheRuntime &result_cache_;
    ReportNightIndexService &night_index_;
    ReportCacheFetchService &cache_fetch_;
};

}  // namespace aircannect
