#include "report_durable_night_index.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_manager_limits.h"
#include "report_night_index_store.h"

namespace aircannect {

ReportDurableNightIndex::~ReportDurableNightIndex() {
    Memory::free(index_);
    index_ = nullptr;
    index_count_ = 0;
    index_valid_ = false;

    Memory::free(save_);
    save_ = nullptr;
    save_count_ = 0;
    save_pending_ = false;

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportDurableNightIndex::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool ReportDurableNightIndex::load() {
    if (!lock_ || !ensure_index_buffer_locked()) return false;

    size_t count = 0;
    uint32_t crc = 0;
    const bool loaded = ReportNightIndexStore::load(index_,
                                                    AC_REPORT_SUMMARY_RECORD_MAX,
                                                    count,
                                                    crc);
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    index_count_ = loaded ? count : 0;
    index_crc_ = loaded ? crc : 0;
    index_valid_ = loaded;
    xSemaphoreGive(lock_);

    if (loaded) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index loaded nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(crc));
    }
    return loaded;
}

bool ReportDurableNightIndex::seed(ReportNightIndex &index) const {
    if (!lock_ || !index_) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return false;

    const bool valid = index_valid_;
    const size_t count = std::min(index_count_,
                                  static_cast<size_t>(
                                      AC_REPORT_SUMMARY_RECORD_MAX));

    bool ok = valid;
    for (size_t i = 0; valid && i < count; ++i) {
        if (!index.add_indexed_night(index_[i])) {
            ok = false;
            break;
        }
    }

    xSemaphoreGive(lock_);
    return ok;
}

void ReportDurableNightIndex::schedule_save(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t content_crc) const {
    if ((!src && count) || count > AC_REPORT_SUMMARY_RECORD_MAX || !lock_) {
        return;
    }

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        const bool unchanged = index_valid_ &&
                               index_crc_ == content_crc &&
                               !save_pending_;
        xSemaphoreGive(lock_);

        if (unchanged) return;
    }

    if (!ensure_save_buffer()) return;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return;

    if (count > 0) {
        memcpy(save_, src, count * sizeof(ReportIndexedNight));
    }
    save_count_ = count;
    save_crc_ = content_crc;
    save_pending_ = true;
    save_requested_ms_ = millis() + 1000;

    xSemaphoreGive(lock_);
}

bool ReportDurableNightIndex::service_writer() {
    if (!lock_) return false;

    size_t count = 0;
    uint32_t expected_crc = 0;

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    if (!save_pending_) {
        xSemaphoreGive(lock_);
        return false;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - save_requested_ms_) < 0) {
        xSemaphoreGive(lock_);
        return false;
    }

    count = save_count_;
    expected_crc = save_crc_;
    xSemaphoreGive(lock_);

    ReportIndexedNight *snapshot = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!snapshot) {
        log_report_alloc_failed(
            "durable_night_index_write_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));

        if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            retry_later_locked(60000);
            xSemaphoreGive(lock_);
        }
        return false;
    }

    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        Memory::free(snapshot);
        return false;
    }

    count = std::min(save_count_,
                     static_cast<size_t>(AC_REPORT_SUMMARY_RECORD_MAX));
    expected_crc = save_crc_;
    if (count > 0) {
        memcpy(snapshot, save_, count * sizeof(ReportIndexedNight));
    }
    xSemaphoreGive(lock_);

    uint32_t written_crc = 0;
    const bool ok = ReportNightIndexStore::save(snapshot, count, written_crc);
    if (ok) {
        if (!ensure_index_buffer_locked()) {
            Memory::free(snapshot);
            return false;
        }

        if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(index_, snapshot, count * sizeof(ReportIndexedNight));
            index_count_ = count;
            index_crc_ = written_crc;
            index_valid_ = true;

            if (save_pending_ &&
                save_crc_ == expected_crc &&
                save_count_ == count) {
                save_pending_ = false;
            }

            xSemaphoreGive(lock_);
        }

        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index saved nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(written_crc));
    } else {
        if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            retry_later_locked(60000);
            xSemaphoreGive(lock_);
        }

        Log::logf(CAT_REPORT, LOG_WARN,
                  "Durable night index save failed\n");
    }

    Memory::free(snapshot);
    return ok;
}

bool ReportDurableNightIndex::ensure_index_buffer_locked() const {
    if (index_) return true;

    index_ = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!index_) {
        log_report_alloc_failed(
            "durable_night_index",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    return true;
}

bool ReportDurableNightIndex::ensure_save_buffer() const {
    if (save_) return true;

    save_ = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!save_) {
        log_report_alloc_failed(
            "durable_night_index_save",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    return true;
}

void ReportDurableNightIndex::retry_later_locked(uint32_t delay_ms) const {
    save_requested_ms_ = millis() + delay_ms;
}

}  // namespace aircannect
