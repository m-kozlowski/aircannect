#include "report_manager.h"

#include <stdio.h>

#include "background_worker.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_records.h"
#include "report_sources.h"
#include "report_store.h"

namespace aircannect {

// Coalesce parsed chunks per compatible (kind,name) segment. A night/timebase
// break or size cap opens another active segment instead of forcing an immediate
// writer-queue flush from the parser callback; source finalization drains the
// pending segments with normal backpressure handling.
bool ReportManager::ensure_cache_coalesce_slots() {
    if (cache_coalesce_) return true;

    cache_coalesce_ = static_cast<CacheCoalesceBuffer *>(Memory::alloc_large(
        AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer), false));
    if (!cache_coalesce_) {
        log_report_alloc_failed(
            "cache_coalesce",
            AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer));
        return false;
    }

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        new (&cache_coalesce_[i]) CacheCoalesceBuffer();
    }
    return true;
}

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

bool ReportManager::buffer_parsed_chunk(const ReportParsedChunk &chunk) {
    if (!ensure_cache_coalesce_slots()) {
        fail_cache_fetch("cache_coalesce_alloc_failed");
        return false;
    }

    const int64_t night = night_start_for_timestamp(chunk.start_ms);
    const bool is_series = chunk.kind == ReportStoreChunkKind::Series;
    ReportSeriesV2UniformView series_view;
    if (is_series) {
        if (!report_series_payload_v2_uniform_view(chunk.payload,
                                                   chunk.payload_len,
                                                   chunk.record_count,
                                                   series_view) ||
            series_view.missing_bitmap_bytes != 0) {
            fail_cache_fetch("cache_series_v2_invalid");
            return false;
        }
    }

    const size_t incoming_bytes =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    const size_t fixed_bytes =
        is_series ? report_series_v2_header_size() : 0;

    size_t slot = AC_REPORT_COALESCE_SLOTS;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (cache_coalesce_[i].active &&
            cache_coalesce_[i].kind == chunk.kind &&
            cache_coalesce_[i].name == chunk.name) {
            CacheCoalesceBuffer &buf = cache_coalesce_[i];
            bool size_overflow = incoming_bytes > SIZE_MAX - buf.payload.size();
            size_t projected_size =
                size_overflow ? SIZE_MAX : buf.payload.size() + incoming_bytes;
            if (!size_overflow) {
                size_overflow = fixed_bytes > SIZE_MAX - projected_size;
                if (!size_overflow) projected_size += fixed_bytes;
            }
            if (size_overflow ||
                projected_size > AC_REPORT_COALESCE_TARGET_BYTES) {
                continue;
            }
            if (buf.night_start_ms != night) continue;
            if (is_series &&
                (buf.series_interval_ms != series_view.interval_ms ||
                 chunk.start_ms != buf.last_ms)) {
                continue;
            }
            slot = i;
            break;
        }
    }

    if (slot == AC_REPORT_COALESCE_SLOTS) {
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            if (!cache_coalesce_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == AC_REPORT_COALESCE_SLOTS) {
            const CacheFlushResult flush = flush_cache_coalesce_buffer(0);
            if (flush == CacheFlushResult::Blocked) {
                cache_status_.error = "cache_coalesce_backpressure";
                return false;
            }
            if (flush == CacheFlushResult::Failed) {
                cache_status_.error = "cache_coalesce_flush_failed";
                return false;
            }
            slot = 0;
        }

        CacheCoalesceBuffer &buf = cache_coalesce_[slot];
        buf.active = true;
        buf.kind = chunk.kind;
        buf.source = chunk.source;
        buf.name = chunk.name;
        buf.night_start_ms = night;
        buf.first_ms = chunk.start_ms;
        buf.last_ms = chunk.end_ms;
        buf.record_count = 0;
        buf.payload_schema = chunk.payload_schema;
        buf.series_interval_ms = is_series ? series_view.interval_ms : 0;
        buf.series_values_pending = is_series;
        buf.payload.clear();
        buf.payload.set_max_size(AC_REPORT_COALESCE_TARGET_BYTES + 65536);
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    const uint8_t *append_data =
        is_series ? series_view.values_milli_le : chunk.payload;
    const size_t append_len =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    if (!buf.payload.append(append_data, append_len)) {
        fail_cache_fetch("cache_coalesce_alloc");
        return false;
    }

    if (chunk.start_ms < buf.first_ms) buf.first_ms = chunk.start_ms;
    if (chunk.end_ms > buf.last_ms) buf.last_ms = chunk.end_ms;
    buf.record_count += chunk.record_count;
    note_cache_chunk_coverage(chunk);
    return true;
}

ReportManager::CacheFlushResult ReportManager::flush_cache_coalesce_buffer(
    size_t slot) {
    if (!cache_coalesce_ || slot >= AC_REPORT_COALESCE_SLOTS) {
        return CacheFlushResult::Flushed;
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    if (!buf.active) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    if (buf.record_count > 0 && buf.payload.size() > 0 &&
        buf.last_ms > buf.first_ms) {
        ReportStoreChunkKey key;
        key.kind = buf.kind;
        key.source = report_source_spool_type(buf.source);
        key.name = buf.name;
        key.start_ms = buf.first_ms;
        key.end_ms = buf.last_ms;
        key.night_start_ms = buf.night_start_ms;
        if (!key.source || !key.source[0]) {
            result = CacheFlushResult::Failed;
        } else {
            ReportStoreChunkMeta meta;
            meta.record_count = buf.record_count;
            meta.payload_schema = buf.payload_schema;
            if (buf.kind == ReportStoreChunkKind::Series &&
                buf.series_values_pending) {
                ReportSpoolBuffer built;
                if (!report_build_series_payload_v2_uniform_values_le(
                        built,
                        buf.series_interval_ms,
                        buf.payload.data(),
                        buf.record_count)) {
                    Log::logf(CAT_REPORT,
                              LOG_WARN,
                              "Cache series v2 build failed source=%s "
                              "name=%s records=%lu\n",
                              key.source ? key.source : "",
                              key.name ? key.name : "",
                              static_cast<unsigned long>(buf.record_count));
                    result = CacheFlushResult::Failed;
                } else {
                    buf.payload.move_from(built);
                    buf.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
                    buf.series_values_pending = false;
                    meta.payload_schema = buf.payload_schema;
                }
            }
            if (result == CacheFlushResult::Flushed) {
                const CacheWriteEnqueueResult queued =
                    enqueue_cache_write(buf, key, meta);
                if (queued == CacheWriteEnqueueResult::Blocked) {
                    return CacheFlushResult::Blocked;
                }
                if (queued == CacheWriteEnqueueResult::Failed) {
                    result = CacheFlushResult::Failed;
                }
            }
        }
    }

    buf.active = false;
    buf.payload.clear();
    buf.record_count = 0;
    buf.first_ms = 0;
    buf.last_ms = 0;
    buf.night_start_ms = 0;
    buf.name = nullptr;
    buf.payload_schema = 0;
    buf.series_interval_ms = 0;
    buf.series_values_pending = false;
    return result;
}

ReportManager::CacheWriteEnqueueResult ReportManager::enqueue_cache_write(
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

ReportManager::CacheFlushResult
ReportManager::flush_all_cache_coalesce_buffers() {
    if (!cache_coalesce_) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        const CacheFlushResult flush = flush_cache_coalesce_buffer(i);
        if (flush == CacheFlushResult::Failed) return flush;
        if (flush == CacheFlushResult::Blocked) {
            result = CacheFlushResult::Blocked;
        }
    }
    return result;
}

void ReportManager::discard_cache_coalesce_buffers() {
    if (!cache_coalesce_) return;

    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        CacheCoalesceBuffer &buf = cache_coalesce_[i];
        buf.active = false;
        buf.payload.clear();
        buf.record_count = 0;
        buf.first_ms = 0;
        buf.last_ms = 0;
        buf.night_start_ms = 0;
        buf.name = nullptr;
        buf.payload_schema = 0;
        buf.series_interval_ms = 0;
        buf.series_values_pending = false;
    }
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
