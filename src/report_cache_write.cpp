#include "report_manager.h"

#include "background_worker.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_store.h"

namespace aircannect {

bool ReportManager::ensure_cache_write_queue_slots() {
    if (cache_write_queue_) return true;

    cache_write_queue_ = static_cast<CacheWriteQueueSlot *>(Memory::alloc_large(
        AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot), false));
    if (!cache_write_queue_) {
        log_report_alloc_failed(
            "cache_write_queue",
            AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
        new (&cache_write_queue_[i]) CacheWriteQueueSlot();
    }
    return true;
}

report_manager_internal::CacheWriteEnqueueResult
ReportManager::enqueue_cache_write(
    CacheCoalesceBuffer &buf,
    const ReportStoreChunkKey &key,
    const ReportStoreChunkMeta &meta) {
    if (!cache_write_lock_ || !ensure_cache_write_queue_slots()) {
        return CacheWriteEnqueueResult::Failed;
    }
    if (!xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return CacheWriteEnqueueResult::Blocked;
    }
    if (cache_write_count_ >= AC_REPORT_CACHE_WRITE_QUEUE_MAX) {
        xSemaphoreGive(cache_write_lock_);
        Log::logf(CAT_REPORT, LOG_DEBUG,
                  "Cache chunk writer backpressure source=%s name=%s\n",
                  key.source ? key.source : "",
                  key.name ? key.name : "");
        return CacheWriteEnqueueResult::Blocked;
    }

    CacheWriteQueueSlot &job = cache_write_queue_[cache_write_tail_];
    job.active = true;
    job.fetch_id = cache_write_fetch_id_;
    job.key = key;
    job.meta = meta;
    job.payload.clear();
    job.payload.move_from(buf.payload);
    cache_write_tail_ =
        (cache_write_tail_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    cache_write_count_++;
    cache_write_pending_++;
    xSemaphoreGive(cache_write_lock_);

    if (BackgroundWorker *worker = background_worker()) {
        worker->wake();
    }
    return CacheWriteEnqueueResult::Queued;
}

void ReportManager::reset_cache_write_fetch_state_locked() {
    if (cache_write_queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            CacheWriteQueueSlot &job = cache_write_queue_[i];
            job.active = false;
            job.payload.clear();
        }
    }

    cache_write_head_ = 0;
    cache_write_tail_ = 0;
    cache_write_count_ = 0;
    cache_write_pending_ = 0;
    cache_write_failed_fetch_id_ = 0;
    cache_write_error_.clear();
    ++cache_write_fetch_id_;
    if (cache_write_fetch_id_ == 0) ++cache_write_fetch_id_;
}

void ReportManager::begin_cache_write_fetch() {
    if (!cache_write_lock_) return;

    xSemaphoreTake(cache_write_lock_, portMAX_DELAY);
    reset_cache_write_fetch_state_locked();
    xSemaphoreGive(cache_write_lock_);

    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
}

void ReportManager::abort_cache_write_fetch() {
    if (!cache_write_lock_) return;

    xSemaphoreTake(cache_write_lock_, portMAX_DELAY);
    reset_cache_write_fetch_state_locked();
    xSemaphoreGive(cache_write_lock_);

    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
}

bool ReportManager::cache_writes_pending_for_active_fetch() const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return true;
    }

    const bool pending = cache_write_pending_ > 0;
    xSemaphoreGive(cache_write_lock_);
    return pending;
}

bool ReportManager::cache_write_failed_for_active_fetch(
    std::string &error) const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return false;
    }

    const bool failed =
        cache_write_failed_fetch_id_ != 0 &&
        cache_write_failed_fetch_id_ == cache_write_fetch_id_;
    if (failed) error = cache_write_error_;
    xSemaphoreGive(cache_write_lock_);
    return failed;
}

bool ReportManager::cache_write_backpressure_active() const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return true;
    }

    const bool active =
        cache_write_count_ >=
        AC_REPORT_CACHE_WRITE_BACKPRESSURE_WATERMARK;
    xSemaphoreGive(cache_write_lock_);
    return active;
}

void ReportManager::note_cache_chunk_committed(uint64_t night_start_ms) {
    cache_status_.chunks_written++;
    if (!take_summary_lock(portMAX_DELAY)) return;

    cache_data_epoch_++;
    if (night_start_ms && night_epochs_) {
        for (size_t i = 0; i < night_epoch_count_; ++i) {
            if (night_epochs_[i].night_start_ms == night_start_ms) {
                night_epochs_[i].epoch++;
                give_summary_lock();
                return;
            }
        }

        if (night_epoch_count_ < AC_REPORT_SUMMARY_RECORD_MAX) {
            night_epochs_[night_epoch_count_].night_start_ms = night_start_ms;
            night_epochs_[night_epoch_count_].epoch = 1;
            ++night_epoch_count_;
        }
    }
    give_summary_lock();
}

bool ReportManager::service_cache_writer() {
    if (!cache_write_lock_ || !cache_write_queue_) {
        if (service_result_cache_writer()) return true;
        return service_durable_night_index_writer();
    }

    CacheWriteQueueSlot job;
    if (!xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(20))) return false;
    if (cache_write_count_ == 0) {
        xSemaphoreGive(cache_write_lock_);
        if (service_result_cache_writer()) return true;
        return service_durable_night_index_writer();
    }

    CacheWriteQueueSlot &slot = cache_write_queue_[cache_write_head_];
    job.active = slot.active;
    job.fetch_id = slot.fetch_id;
    job.key = slot.key;
    job.meta = slot.meta;
    job.payload.move_from(slot.payload);
    slot.active = false;
    cache_write_head_ =
        (cache_write_head_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    cache_write_count_--;
    xSemaphoreGive(cache_write_lock_);

    const uint64_t night_start_ms =
        static_cast<uint64_t>(job.key.night_start_ms);
    const size_t payload_size = job.payload.size();
    const bool ok = job.active &&
                    ReportStore::write_chunk(job.key,
                                             job.meta,
                                             job.payload.data(),
                                             job.payload.size());
    job.payload.clear();

    bool current_fetch = false;
    if (xSemaphoreTake(cache_write_lock_, portMAX_DELAY)) {
        current_fetch = job.fetch_id == cache_write_fetch_id_;
        if (current_fetch) {
            if (cache_write_pending_ > 0) cache_write_pending_--;
            if (!ok) {
                cache_write_failed_fetch_id_ = job.fetch_id;
                cache_write_error_ = "cache_write_failed";
            }
        }
        xSemaphoreGive(cache_write_lock_);
    }

    if (ok && current_fetch) {
        note_cache_chunk_committed(night_start_ms);
    } else if (!ok && current_fetch) {
        Log::logf(CAT_REPORT, LOG_WARN,
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
