#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_internal_types.h"
#include "report_parser.h"
#include "report_store.h"

namespace aircannect {

class ReportCacheCoalescerSink {
public:
    using CacheCoalesceBuffer = report_manager_internal::CacheCoalesceBuffer;
    using CacheWriteEnqueueResult =
        report_manager_internal::CacheWriteEnqueueResult;

    virtual CacheWriteEnqueueResult enqueue_cache_write(
        CacheCoalesceBuffer &buf,
        const ReportStoreChunkKey &key,
        const ReportStoreChunkMeta &meta) = 0;
    virtual void note_cache_chunk_coverage(
        const ReportParsedChunk &chunk) = 0;

protected:
    ~ReportCacheCoalescerSink() = default;
};

enum class ReportCacheCoalesceResult : uint8_t {
    Buffered,
    Backpressure,
    FlushFailed,
    AllocFailed,
    InvalidSeries,
    PayloadAllocFailed,
};

const char *report_cache_coalesce_error(ReportCacheCoalesceResult result);

class ReportCacheCoalescer {
public:
    using CacheFlushResult = report_manager_internal::CacheFlushResult;

    ~ReportCacheCoalescer();

    bool begin();
    void release();

    ReportCacheCoalesceResult buffer(const ReportParsedChunk &chunk,
                                     int64_t night_start_ms,
                                     ReportCacheCoalescerSink &sink);
    CacheFlushResult flush_all(ReportCacheCoalescerSink &sink);
    void discard();

private:
    using CacheCoalesceBuffer = report_manager_internal::CacheCoalesceBuffer;

    CacheFlushResult flush_slot(size_t slot,
                                ReportCacheCoalescerSink &sink);
    void reset_slot(CacheCoalesceBuffer &buf);

    CacheCoalesceBuffer *slots_ = nullptr;
};

}  // namespace aircannect
