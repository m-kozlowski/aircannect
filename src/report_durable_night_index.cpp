#include "report_manager.h"

#include <algorithm>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"
#include "report_night_index_store.h"

namespace aircannect {

bool ReportManager::load_durable_night_index() {
    if (!durable_index_lock_) return false;
    if (!durable_index_) {
        durable_index_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportIndexedNight),
                                 false));
        if (!durable_index_) {
            log_report_alloc_failed(
                "durable_night_index",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            return false;
        }
    }

    size_t count = 0;
    uint32_t crc = 0;
    const bool loaded = ReportNightIndexStore::load(durable_index_,
                                                    AC_REPORT_SUMMARY_RECORD_MAX,
                                                    count,
                                                    crc);
    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    durable_index_count_ = loaded ? count : 0;
    durable_index_crc_ = loaded ? crc : 0;
    durable_index_valid_ = loaded;
    xSemaphoreGive(durable_index_lock_);

    if (loaded) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index loaded nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(crc));
    }
    return loaded;
}

bool ReportManager::seed_index_from_durable(ReportNightIndex &index) const {
    if (!durable_index_lock_ || !durable_index_) return false;
    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    const bool valid = durable_index_valid_;
    const size_t count = std::min(durable_index_count_,
                                  static_cast<size_t>(
                                      AC_REPORT_SUMMARY_RECORD_MAX));

    bool ok = valid;
    for (size_t i = 0; valid && i < count; ++i) {
        if (!index.add_indexed_night(durable_index_[i])) {
            ok = false;
            break;
        }
    }

    xSemaphoreGive(durable_index_lock_);
    return ok;
}

void ReportManager::schedule_durable_night_index_save(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t content_crc) const {
    if ((!src && count) || count > AC_REPORT_SUMMARY_RECORD_MAX ||
        !durable_index_lock_) {
        return;
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        const bool unchanged = durable_index_valid_ &&
                               durable_index_crc_ == content_crc &&
                               !durable_index_save_pending_;
        xSemaphoreGive(durable_index_lock_);

        if (unchanged) return;
    }

    if (!durable_index_save_) {
        durable_index_save_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportIndexedNight),
                                 false));
        if (!durable_index_save_) {
            log_report_alloc_failed(
                "durable_night_index_save",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            return;
        }
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }

    if (count > 0) {
        memcpy(durable_index_save_, src, count * sizeof(ReportIndexedNight));
    }
    durable_index_save_count_ = count;
    durable_index_save_crc_ = content_crc;
    durable_index_save_pending_ = true;
    durable_index_save_requested_ms_ = millis() + 1000;

    xSemaphoreGive(durable_index_lock_);
}

bool ReportManager::service_durable_night_index_writer() {
    if (!durable_index_lock_) return false;

    size_t count = 0;
    uint32_t expected_crc = 0;

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    if (!durable_index_save_pending_) {
        xSemaphoreGive(durable_index_lock_);
        return false;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - durable_index_save_requested_ms_) < 0) {
        xSemaphoreGive(durable_index_lock_);
        return false;
    }

    count = durable_index_save_count_;
    expected_crc = durable_index_save_crc_;
    xSemaphoreGive(durable_index_lock_);

    ReportIndexedNight *snapshot = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!snapshot) {
        log_report_alloc_failed(
            "durable_night_index_write_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));

        if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            durable_index_save_requested_ms_ = millis() + 60000;
            xSemaphoreGive(durable_index_lock_);
        }
        return false;
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        Memory::free(snapshot);
        return false;
    }

    count = std::min(durable_index_save_count_,
                     static_cast<size_t>(AC_REPORT_SUMMARY_RECORD_MAX));
    expected_crc = durable_index_save_crc_;
    if (count > 0) {
        memcpy(snapshot,
               durable_index_save_,
               count * sizeof(ReportIndexedNight));
    }
    xSemaphoreGive(durable_index_lock_);

    uint32_t written_crc = 0;
    const bool ok = ReportNightIndexStore::save(snapshot, count, written_crc);
    if (ok) {
        if (!durable_index_) {
            durable_index_ = static_cast<ReportIndexedNight *>(
                Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                     sizeof(ReportIndexedNight),
                                     false));
        }

        if (durable_index_ &&
            xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(durable_index_,
                   snapshot,
                   count * sizeof(ReportIndexedNight));
            durable_index_count_ = count;
            durable_index_crc_ = written_crc;
            durable_index_valid_ = true;

            if (durable_index_save_pending_ &&
                durable_index_save_crc_ == expected_crc &&
                durable_index_save_count_ == count) {
                durable_index_save_pending_ = false;
            }

            xSemaphoreGive(durable_index_lock_);
        }

        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index saved nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(written_crc));
    } else {
        if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            durable_index_save_requested_ms_ = millis() + 60000;
            xSemaphoreGive(durable_index_lock_);
        }

        Log::logf(CAT_REPORT, LOG_WARN,
                  "Durable night index save failed\n");
    }

    Memory::free(snapshot);
    return ok;
}

}  // namespace aircannect
