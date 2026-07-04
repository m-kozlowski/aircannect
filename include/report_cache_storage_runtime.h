#pragma once

#include <stdint.h>
#include <string>

#include "report_cache_coalescer.h"
#include "report_cache_write_queue.h"
#include "report_parser.h"
#include "report_store.h"
#include "report_trash_cleanup.h"

namespace aircannect {

class ReportCacheStorageRuntime {
public:
    using CacheCoalesceBuffer = report_manager_internal::CacheCoalesceBuffer;
    using CacheFlushResult = report_manager_internal::CacheFlushResult;
    using CacheWriteEnqueueResult =
        report_manager_internal::CacheWriteEnqueueResult;
    using CacheWriteQueueSlot =
        report_manager_internal::CacheWriteQueueSlot;

    bool begin();
    void release();

    // Coalesced chunk writes
    ReportCacheCoalesceResult buffer_chunk(const ReportParsedChunk &chunk,
                                           int64_t night_start_ms,
                                           ReportCacheCoalescerSink &sink);
    CacheFlushResult flush_coalesced(ReportCacheCoalescerSink &sink);
    void discard_coalesced();

    // Write queue
    CacheWriteEnqueueResult enqueue_write(CacheCoalesceBuffer &buf,
                                          const ReportStoreChunkKey &key,
                                          const ReportStoreChunkMeta &meta);
    void begin_write_fetch();
    void abort_write_fetch();
    bool writes_pending_for_active_fetch() const;
    bool write_failed_for_active_fetch(std::string &error) const;
    bool write_backpressure_active() const;

    bool take_next_write(CacheWriteQueueSlot &job);
    bool complete_write(const CacheWriteQueueSlot &job, bool ok);

    // Store maintenance
    void service_trash_cleanup(bool realtime_active, bool report_busy);

private:
    ReportCacheCoalescer coalescer_;
    ReportCacheWriteQueue write_queue_;
    ReportTrashCleanup trash_cleanup_;
};

}  // namespace aircannect
