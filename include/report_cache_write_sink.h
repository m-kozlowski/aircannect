#pragma once

#include "report_cache_coalescer.h"

namespace aircannect {

class ReportCacheStorageRuntime;
class ReportSummaryRuntime;

class ReportCacheWriteSink final : public ReportCacheCoalescerSink {
public:
    ReportCacheWriteSink(ReportCacheStorageRuntime &storage,
                         ReportSummaryRuntime &summary);

    CacheWriteEnqueueResult enqueue_cache_write(
        CacheCoalesceBuffer &buf,
        const ReportStoreChunkKey &key,
        const ReportStoreChunkMeta &meta) override;
    void note_cache_chunk_coverage(const ReportParsedChunk &chunk) override;

private:
    ReportCacheStorageRuntime &storage_;
    ReportSummaryRuntime &summary_;
};

}  // namespace aircannect
