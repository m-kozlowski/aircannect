#pragma once

namespace aircannect {

class ReportCacheStorageRuntime;
class ReportFetchRuntime;
class ReportNightIndexRuntime;
class ReportResultCacheRuntime;
class ReportSummaryRuntime;

class ReportCacheWriteService {
public:
    ReportCacheWriteService(ReportCacheStorageRuntime &storage,
                            ReportFetchRuntime &fetch,
                            ReportSummaryRuntime &summary,
                            ReportNightIndexRuntime &night_index,
                            ReportResultCacheRuntime &result_cache);

    bool service();

private:
    ReportCacheStorageRuntime &storage_;
    ReportFetchRuntime &fetch_;
    ReportSummaryRuntime &summary_;
    ReportNightIndexRuntime &night_index_;
    ReportResultCacheRuntime &result_cache_;
};

}  // namespace aircannect
