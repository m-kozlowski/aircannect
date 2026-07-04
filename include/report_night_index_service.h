#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_edf_catalog_context.h"
#include "report_night_index.h"
#include "report_night_index_cache.h"
#include "report_night_index_runtime.h"
#include "report_summary_runtime.h"

namespace aircannect {

class ReportNightIndexService {
public:
    ReportNightIndexService(ReportSummaryRuntime &summary,
                            ReportNightIndexRuntime &runtime,
                            ReportEdfCatalogContext &edf_catalog);

    bool begin();

    bool build(ReportIndexedNight *out,
               size_t capacity,
               size_t &count) const;
    bool by_therapy_index(size_t therapy_index,
                          ReportIndexedNight &out) const;
    bool by_start(uint64_t night_start_ms,
                  ReportIndexedNight &out,
                  size_t *therapy_index_out = nullptr) const;
    bool cache_key(ReportNightIndexCacheKey &key) const;

    void format_result_etag(const ReportIndexedNight &night,
                            char *out,
                            size_t out_size) const;

private:
    bool build_uncached(ReportIndexedNight *out,
                        size_t capacity,
                        size_t &count) const;

    ReportSummaryRuntime &summary_;
    ReportNightIndexRuntime &runtime_;
    ReportEdfCatalogContext &edf_catalog_;
    mutable ReportNightIndexCache cache_;
};

}  // namespace aircannect
