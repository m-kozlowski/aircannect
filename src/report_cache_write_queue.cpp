#include "report_cache_write_queue.h"

#include <new>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"

namespace aircannect {

ReportCacheWriteQueue::~ReportCacheWriteQueue() {
    if (queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            queue_[i].~CacheWriteQueueSlot();
        }
        Memory::free(queue_);
        queue_ = nullptr;
    }

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportCacheWriteQueue::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool ReportCacheWriteQueue::ensure_slots() {
    if (queue_) return true;

    queue_ = static_cast<CacheWriteQueueSlot *>(Memory::alloc_large(
        AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot), false));
    if (!queue_) {
        log_report_alloc_failed(
            "cache_write_queue",
            AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
        new (&queue_[i]) CacheWriteQueueSlot();
    }
    return true;
}

ReportCacheWriteQueue::CacheWriteEnqueueResult
ReportCacheWriteQueue::enqueue(CacheCoalesceBuffer &buf,
                               const ReportStoreChunkKey &key,
                               const ReportStoreChunkMeta &meta) {
    if (!lock_ || !ensure_slots()) return CacheWriteEnqueueResult::Failed;
    if (!xSemaphoreTake(lock_, pdMS_TO_TICKS(5))) {
        return CacheWriteEnqueueResult::Blocked;
    }

    if (count_ >= AC_REPORT_CACHE_WRITE_QUEUE_MAX) {
        xSemaphoreGive(lock_);
        Log::logf(CAT_REPORT, LOG_DEBUG,
                  "Cache chunk writer backpressure source=%s name=%s\n",
                  key.source ? key.source : "",
                  key.name ? key.name : "");
        return CacheWriteEnqueueResult::Blocked;
    }

    CacheWriteQueueSlot &job = queue_[tail_];
    job.active = true;
    job.fetch_id = fetch_id_;
    job.key = key;
    job.meta = meta;
    job.payload.clear();
    job.payload.move_from(buf.payload);

    tail_ = (tail_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    count_++;
    pending_++;

    xSemaphoreGive(lock_);
    return CacheWriteEnqueueResult::Queued;
}

void ReportCacheWriteQueue::begin_fetch() {
    if (!lock_) return;

    xSemaphoreTake(lock_, portMAX_DELAY);
    reset_fetch_state_locked();
    xSemaphoreGive(lock_);
}

void ReportCacheWriteQueue::abort_fetch() {
    begin_fetch();
}

bool ReportCacheWriteQueue::pending_for_active_fetch() const {
    if (!lock_ || !xSemaphoreTake(lock_, pdMS_TO_TICKS(5))) return true;

    const bool pending = pending_ > 0;

    xSemaphoreGive(lock_);
    return pending;
}

bool ReportCacheWriteQueue::failed_for_active_fetch(std::string &error) const {
    if (!lock_ || !xSemaphoreTake(lock_, pdMS_TO_TICKS(5))) return false;

    const bool failed = failed_fetch_id_ != 0 && failed_fetch_id_ == fetch_id_;
    if (failed) error = error_;

    xSemaphoreGive(lock_);
    return failed;
}

bool ReportCacheWriteQueue::backpressure_active() const {
    if (!lock_ || !xSemaphoreTake(lock_, pdMS_TO_TICKS(5))) return true;

    const bool active =
        count_ >= AC_REPORT_CACHE_WRITE_BACKPRESSURE_WATERMARK;

    xSemaphoreGive(lock_);
    return active;
}

bool ReportCacheWriteQueue::take_next(CacheWriteQueueSlot &job) {
    if (!lock_ || !queue_) return false;
    if (!xSemaphoreTake(lock_, pdMS_TO_TICKS(20))) return false;

    if (count_ == 0) {
        xSemaphoreGive(lock_);
        return false;
    }

    CacheWriteQueueSlot &slot = queue_[head_];
    job.active = slot.active;
    job.fetch_id = slot.fetch_id;
    job.key = slot.key;
    job.meta = slot.meta;
    job.payload.move_from(slot.payload);
    slot.active = false;

    head_ = (head_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    count_--;

    xSemaphoreGive(lock_);
    return true;
}

bool ReportCacheWriteQueue::complete(const CacheWriteQueueSlot &job, bool ok) {
    if (!lock_) return false;

    bool current_fetch = false;
    xSemaphoreTake(lock_, portMAX_DELAY);

    current_fetch = job.fetch_id == fetch_id_;
    if (current_fetch) {
        if (pending_ > 0) pending_--;
        if (!ok) {
            failed_fetch_id_ = job.fetch_id;
            error_ = "cache_write_failed";
        }
    }

    xSemaphoreGive(lock_);
    return current_fetch;
}

void ReportCacheWriteQueue::reset_fetch_state_locked() {
    if (queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            CacheWriteQueueSlot &job = queue_[i];
            job.active = false;
            job.payload.clear();
        }
    }

    head_ = 0;
    tail_ = 0;
    count_ = 0;
    pending_ = 0;
    failed_fetch_id_ = 0;
    error_.clear();
    ++fetch_id_;
    if (fetch_id_ == 0) ++fetch_id_;
}

}  // namespace aircannect
