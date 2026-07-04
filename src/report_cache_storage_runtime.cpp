#include "report_cache_storage_runtime.h"

namespace aircannect {

bool ReportCacheStorageRuntime::begin() {
    const bool queue_ok = write_queue_.begin();
    const bool coalescer_ok = coalescer_.begin();

    return queue_ok && coalescer_ok;
}

void ReportCacheStorageRuntime::release() {
    coalescer_.release();
}

ReportCacheCoalesceResult ReportCacheStorageRuntime::buffer_chunk(
    const ReportParsedChunk &chunk,
    int64_t night_start_ms,
    ReportCacheCoalescerSink &sink) {
    return coalescer_.buffer(chunk, night_start_ms, sink);
}

ReportCacheStorageRuntime::CacheFlushResult
ReportCacheStorageRuntime::flush_coalesced(ReportCacheCoalescerSink &sink) {
    return coalescer_.flush_all(sink);
}

void ReportCacheStorageRuntime::discard_coalesced() {
    coalescer_.discard();
}

ReportCacheStorageRuntime::CacheWriteEnqueueResult
ReportCacheStorageRuntime::enqueue_write(CacheCoalesceBuffer &buf,
                                         const ReportStoreChunkKey &key,
                                         const ReportStoreChunkMeta &meta) {
    return write_queue_.enqueue(buf, key, meta);
}

void ReportCacheStorageRuntime::begin_write_fetch() {
    write_queue_.begin_fetch();
}

void ReportCacheStorageRuntime::abort_write_fetch() {
    write_queue_.abort_fetch();
}

bool ReportCacheStorageRuntime::writes_pending_for_active_fetch() const {
    return write_queue_.pending_for_active_fetch();
}

bool ReportCacheStorageRuntime::write_failed_for_active_fetch(
    std::string &error) const {
    return write_queue_.failed_for_active_fetch(error);
}

bool ReportCacheStorageRuntime::write_backpressure_active() const {
    return write_queue_.backpressure_active();
}

bool ReportCacheStorageRuntime::take_next_write(CacheWriteQueueSlot &job) {
    return write_queue_.take_next(job);
}

bool ReportCacheStorageRuntime::complete_write(const CacheWriteQueueSlot &job,
                                               bool ok) {
    return write_queue_.complete(job, ok);
}

void ReportCacheStorageRuntime::service_trash_cleanup(bool realtime_active,
                                                      bool report_busy) {
    trash_cleanup_.service(realtime_active, report_busy);
}

}  // namespace aircannect
