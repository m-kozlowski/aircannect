#pragma once

#include <stdint.h>

#include "report_cache_fetch_service.h"
#include "report_edf_catalog_context.h"
#include "report_night_coverage.h"
#include "report_night_index_service.h"

namespace aircannect {

using ReportNightCacheSkipFn = bool (*)(uint64_t night_start_ms,
                                        uint32_t now_ms,
                                        const void *context);

class ReportNightCacheService {
public:
    ReportNightCacheService(ReportNightIndexService &night_index,
                            ReportEdfCatalogContext &edf_catalog,
                            ReportCacheFetchService &cache_fetch);

    bool coverage(uint64_t night_start_ms,
                  ReportNightCoverageStatus &out) const;

    bool next_needing_cache(uint64_t &night_start_ms_out,
                            ReportNightCacheSkipFn skip = nullptr,
                            const void *skip_context = nullptr) const;

    bool request_cache(uint64_t night_start_ms, bool force);

private:
    ReportNightIndexService &night_index_;
    ReportEdfCatalogContext &edf_catalog_;
    ReportCacheFetchService &cache_fetch_;
};

}  // namespace aircannect
