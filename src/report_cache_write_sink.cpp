#include "report_cache_write_sink.h"

#include <freertos/FreeRTOS.h>

#include "background_worker.h"
#include "report_cache_storage_runtime.h"
#include "report_summary_runtime.h"

namespace aircannect {

ReportCacheWriteSink::ReportCacheWriteSink(
    ReportCacheStorageRuntime &storage,
    ReportSummaryRuntime &summary)
    : storage_(storage),
      summary_(summary) {}

ReportCacheWriteSink::CacheWriteEnqueueResult
ReportCacheWriteSink::enqueue_cache_write(
    CacheCoalesceBuffer &buf,
    const ReportStoreChunkKey &key,
    const ReportStoreChunkMeta &meta) {
    const CacheWriteEnqueueResult result =
        storage_.enqueue_write(buf, key, meta);

    if (BackgroundWorker *worker = background_worker()) {
        if (result == CacheWriteEnqueueResult::Queued) worker->wake();
    }

    return result;
}

void ReportCacheWriteSink::note_cache_chunk_coverage(
    const ReportParsedChunk &chunk) {
    if (chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) return;

    if (!summary_.take(portMAX_DELAY)) return;
    summary_.note_coverage_chunk(chunk);
    summary_.give();
}

}  // namespace aircannect
