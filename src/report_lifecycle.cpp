#include "report_manager.h"

#include "memory_manager.h"
#include "report_diagnostics.h"

namespace aircannect {

ReportManager::~ReportManager() {
    Memory::free(records_);
    records_ = nullptr;

    Memory::free(summary_scratch_);
    summary_scratch_ = nullptr;

    Memory::free(night_epochs_);
    night_epochs_ = nullptr;
    night_epoch_count_ = 0;

    Memory::free(cache_source_night_extent_ms_);
    cache_source_night_extent_ms_ = nullptr;

    if (cache_coalesce_) {
        discard_cache_coalesce_buffers();
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            cache_coalesce_[i].~CacheCoalesceBuffer();
        }
        Memory::free(cache_coalesce_);
        cache_coalesce_ = nullptr;
    }

    if (cache_write_queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            cache_write_queue_[i].~CacheWriteQueueSlot();
        }
        Memory::free(cache_write_queue_);
        cache_write_queue_ = nullptr;
    }

    if (result_slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            result_slots_[i].~MaterializedResult();
        }
        Memory::free(result_slots_);
        result_slots_ = nullptr;
    }

    Memory::free(result_chunks_);
    result_chunks_ = nullptr;
    result_chunk_capacity_ = 0;

    Memory::free(result_edf_sessions_);
    result_edf_sessions_ = nullptr;
    result_edf_session_count_ = 0;

    Memory::free(result_resolved_plan_);
    result_resolved_plan_ = nullptr;

    Memory::free(result_resolve_scratch_);
    result_resolve_scratch_ = nullptr;

    Memory::free(prepare_indexed_night_);
    prepare_indexed_night_ = nullptr;

    Memory::free(range_indexed_night_);
    range_indexed_night_ = nullptr;

    Memory::free(index_cache_);
    index_cache_ = nullptr;
    index_cache_count_ = 0;
    index_cache_valid_ = false;

    Memory::free(range_chunks_);
    range_chunks_ = nullptr;
    range_chunk_count_ = 0;

    Memory::free(range_edf_sessions_);
    range_edf_sessions_ = nullptr;
    range_edf_session_count_ = 0;

    Memory::free(durable_index_);
    durable_index_ = nullptr;
    durable_index_count_ = 0;
    durable_index_valid_ = false;

    Memory::free(durable_index_save_);
    durable_index_save_ = nullptr;
    durable_index_save_count_ = 0;
    durable_index_save_pending_ = false;

    if (summary_lock_) {
        vSemaphoreDelete(summary_lock_);
        summary_lock_ = nullptr;
    }

    if (summary_scratch_lock_) {
        vSemaphoreDelete(summary_scratch_lock_);
        summary_scratch_lock_ = nullptr;
    }

    if (result_slots_lock_) {
        vSemaphoreDelete(result_slots_lock_);
        result_slots_lock_ = nullptr;
    }

    if (index_cache_lock_) {
        vSemaphoreDelete(index_cache_lock_);
        index_cache_lock_ = nullptr;
    }

    if (durable_index_lock_) {
        vSemaphoreDelete(durable_index_lock_);
        durable_index_lock_ = nullptr;
    }

    if (plot_cache_write_lock_) {
        if (xSemaphoreTake(plot_cache_write_lock_, portMAX_DELAY) == pdTRUE) {
            reset_result_cache_write_locked();
            xSemaphoreGive(plot_cache_write_lock_);
        }
        vSemaphoreDelete(plot_cache_write_lock_);
        plot_cache_write_lock_ = nullptr;
    }

    if (build_queue_lock_) {
        vSemaphoreDelete(build_queue_lock_);
        build_queue_lock_ = nullptr;
    }

    if (prefetch_lock_) {
        vSemaphoreDelete(prefetch_lock_);
        prefetch_lock_ = nullptr;
    }

    if (cache_write_lock_) {
        vSemaphoreDelete(cache_write_lock_);
        cache_write_lock_ = nullptr;
    }
}

void ReportManager::begin() {
    if (!summary_lock_) summary_lock_ = xSemaphoreCreateMutex();
    if (!summary_scratch_lock_) summary_scratch_lock_ = xSemaphoreCreateMutex();
    if (!index_cache_lock_) index_cache_lock_ = xSemaphoreCreateMutex();
    if (!durable_index_lock_) durable_index_lock_ = xSemaphoreCreateMutex();
    if (!plot_cache_write_lock_) {
        plot_cache_write_lock_ = xSemaphoreCreateMutex();
    }
    if (!prefetch_lock_) prefetch_lock_ = xSemaphoreCreateMutex();
    if (!build_queue_lock_) build_queue_lock_ = xSemaphoreCreateMutex();
    if (!cache_write_lock_) cache_write_lock_ = xSemaphoreCreateMutex();

    if (!night_epochs_) {
        night_epochs_ = static_cast<NightEpoch *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX, sizeof(NightEpoch), false));
        if (!night_epochs_) {
            log_report_alloc_failed(
                "night_epochs",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(NightEpoch));
        }
    }

    ensure_cache_source_night_extents();
    ensure_cache_coalesce_slots();
    ensure_cache_write_queue_slots();
    load_durable_night_index();

    clear_summary_records();
    summary_status_ = {};
    if (!load_summary_from_store()) {
        publish_summary_json_snapshot();
    }
}

bool ReportManager::ensure_cache_source_night_extents() {
    if (cache_source_night_extent_ms_) return true;

    cache_source_night_extent_ms_ = static_cast<int64_t *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(int64_t),
                             false));
    if (!cache_source_night_extent_ms_) {
        log_report_alloc_failed(
            "cache_source_night_extents",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
        return false;
    }

    return true;
}

void ReportManager::set_edf_report_catalog(EdfReportCatalogJob *catalog) {
    edf_catalog_ = catalog;
}

bool ReportManager::take_summary_lock(TickType_t timeout) const {
    return !summary_lock_ || xSemaphoreTake(summary_lock_, timeout) == pdTRUE;
}

void ReportManager::give_summary_lock() const {
    if (summary_lock_) xSemaphoreGive(summary_lock_);
}

bool ReportManager::take_summary_scratch(TickType_t timeout,
                                         ReportSummaryRecord *&out) {
    out = nullptr;
    if (!summary_scratch_lock_ ||
        xSemaphoreTake(summary_scratch_lock_, timeout) != pdTRUE) {
        return false;
    }

    if (!summary_scratch_) {
        summary_scratch_ = static_cast<ReportSummaryRecord *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportSummaryRecord),
                                 false));
    }
    if (!summary_scratch_) {
        xSemaphoreGive(summary_scratch_lock_);
        log_report_alloc_failed(
            "summary_scratch",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        return false;
    }

    out = summary_scratch_;
    return true;
}

void ReportManager::give_summary_scratch() {
    if (summary_scratch_lock_) xSemaphoreGive(summary_scratch_lock_);
}

}  // namespace aircannect
