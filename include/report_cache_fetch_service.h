#pragma once

#include <stdint.h>

#include "report_cache_storage_runtime.h"
#include "report_cache_write_sink.h"
#include "report_edf_catalog_context.h"
#include "report_fetch_runtime.h"
#include "report_sources.h"
#include "report_summary_runtime.h"
#include "rpc_arbiter.h"

namespace aircannect {

struct ReportIndexedNight;

enum class ReportCacheFetchEvent : uint8_t {
    None,
    Completed,
    Failed,
};

class ReportCacheFetchService {
public:
    ReportCacheFetchService(ReportFetchRuntime &fetch,
                            ReportSummaryRuntime &summary,
                            ReportCacheStorageRuntime &storage,
                            ReportCacheWriteSink &write_sink,
                            ReportEdfCatalogContext &edf_catalog);

    bool active() const { return fetch_.cache_active(); }
    bool has_sources() const { return fetch_.cache().has_sources(); }

    ReportCacheFetchState &state() { return fetch_.cache(); }
    const ReportCacheFetchState &state() const { return fetch_.cache(); }
    const ReportCacheFetchStatus &status() const {
        return fetch_.cache_status();
    }

    void set_pending_prepare(size_t therapy_index, bool refresh_cache);
    bool take_pending_prepare(ReportPendingResultPrepare &out);

    bool build_plan(const ReportIndexedNight &night,
                    bool force,
                    bool latest_tail_refresh);
    bool activate_plan();
    ReportCacheFetchEvent start_next_source();
    ReportCacheFetchEvent poll(RpcArbiter &arbiter);
    ReportCacheFetchEvent finish();
    ReportCacheFetchEvent fail(const char *message);
    ReportCacheFetchEvent cancel(const char *message);

private:
    static bool write_parsed_chunk(void *context,
                                   const ReportParsedChunk &chunk);

    bool buffer_parsed_chunk(const ReportParsedChunk &chunk);
    bool store_cache_round(ReportSpoolResult &result);
    bool reset_source_coverage_marks();
    bool write_source_coverage(ReportSourceId source, int64_t from_ms);
    bool fail_if_write_failed();
    ReportCacheFetchEvent finalize_source_if_ready();
    bool drain_spool_rounds();
    ReportCacheFetchEvent finish_spool_if_terminal();

    ReportFetchRuntime &fetch_;
    ReportSummaryRuntime &summary_;
    ReportCacheStorageRuntime &storage_;
    ReportCacheWriteSink &write_sink_;
    ReportEdfCatalogContext &edf_catalog_;
};

}  // namespace aircannect
