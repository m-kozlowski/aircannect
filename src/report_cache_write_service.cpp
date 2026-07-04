#include "report_cache_write_service.h"

#include <freertos/FreeRTOS.h>

#include "debug_log.h"
#include "report_cache_storage_runtime.h"
#include "report_fetch_runtime.h"
#include "report_night_index_runtime.h"
#include "report_result_cache_runtime.h"
#include "report_store.h"
#include "report_summary_runtime.h"

namespace aircannect {

ReportCacheWriteService::ReportCacheWriteService(
    ReportCacheStorageRuntime &storage,
    ReportFetchRuntime &fetch,
    ReportSummaryRuntime &summary,
    ReportNightIndexRuntime &night_index,
    ReportResultCacheRuntime &result_cache)
    : storage_(storage),
      fetch_(fetch),
      summary_(summary),
      night_index_(night_index),
      result_cache_(result_cache) {}

bool ReportCacheWriteService::service() {
    ReportCacheStorageRuntime::CacheWriteQueueSlot job;
    if (!storage_.take_next_write(job)) {
        if (result_cache_.service_writer()) return true;
        return night_index_.service_durable_writer();
    }

    const uint64_t night_start_ms =
        static_cast<uint64_t>(job.key.night_start_ms);
    const size_t payload_size = job.payload.size();
    const bool ok = job.active &&
                    ReportStore::write_chunk(job.key,
                                             job.meta,
                                             job.payload.data(),
                                             job.payload.size());
    job.payload.clear();

    const bool current_fetch = storage_.complete_write(job, ok);
    if (ok && current_fetch) {
        fetch_.cache().note_chunk_written();

        if (summary_.take(portMAX_DELAY)) {
            night_index_.note_chunk_committed(night_start_ms);
            summary_.give();
        }
    } else if (!ok && current_fetch) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Cache chunk write failed source=%s name=%s "
                  "start=%lld end=%lld bytes=%u\n",
                  job.key.source ? job.key.source : "",
                  job.key.name ? job.key.name : "",
                  static_cast<long long>(job.key.start_ms),
                  static_cast<long long>(job.key.end_ms),
                  static_cast<unsigned>(payload_size));
    }

    return true;
}

}  // namespace aircannect
