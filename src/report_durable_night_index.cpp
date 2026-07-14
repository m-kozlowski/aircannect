#include "report_durable_night_index.h"

#include <utility>

#include "debug_log.h"
#include "report_night_index_store.h"

namespace aircannect {

ReportDurableNightIndex::~ReportDurableNightIndex() {
    index_.reset();
    save_.reset();

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
    ReportNightIndexSnapshotRef loaded;
    uint32_t crc = 0;
    if (!ReportNightIndexStore::load(loaded, crc) || !loaded) return false;
    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    ReportNightIndexSnapshotRef old = std::move(index_);
    index_ = loaded;
    index_valid_ = true;
    xSemaphoreGive(lock_);
    old.reset();

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Durable night index loaded nights=%lu crc=0x%08lx\n",
              static_cast<unsigned long>(loaded->count()),
              static_cast<unsigned long>(crc));
    return true;
}

bool ReportDurableNightIndex::snapshot(
    ReportNightIndexSnapshotRef &out) const {
    out.reset();
    if (!lock_ || xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }

    if (index_valid_) out = index_;
    xSemaphoreGive(lock_);
    return out != nullptr;
}

void ReportDurableNightIndex::schedule_save(
    const ReportNightIndexSnapshotRef &snapshot) const {
    if (!snapshot || !lock_) return;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return;

    const bool unchanged = !save_pending_ &&
                           index_.get() == snapshot.get();
    const bool already_pending = save_pending_ &&
                                 save_.get() == snapshot.get();
    if (unchanged || already_pending) {
        xSemaphoreGive(lock_);
        return;
    }

    ReportNightIndexSnapshotRef old_pending = std::move(save_);
    save_ = snapshot;
    save_pending_ = true;
    save_requested_ms_ = millis() + 1000;

    xSemaphoreGive(lock_);
    old_pending.reset();
}

bool ReportDurableNightIndex::service_writer() {
    if (!lock_) return false;

    ReportNightIndexSnapshotRef snapshot;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    if (!save_pending_ || !save_) {
        xSemaphoreGive(lock_);
        return false;
    }

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - save_requested_ms_) < 0) {
        xSemaphoreGive(lock_);
        return false;
    }

    snapshot = save_;
    xSemaphoreGive(lock_);

    uint32_t written_crc = 0;
    const bool ok = ReportNightIndexStore::save(*snapshot, written_crc);
    if (!ok) {
        if (xSemaphoreTake(lock_, portMAX_DELAY) == pdTRUE) {
            retry_later_locked(60000);
            xSemaphoreGive(lock_);
        }

        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Durable night index save failed written_crc=0x%08lx\n",
                  static_cast<unsigned long>(written_crc));
        return false;
    }

    ReportNightIndexSnapshotRef old_index;
    ReportNightIndexSnapshotRef completed_save;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return false;

    old_index = std::move(index_);
    index_ = snapshot;
    index_valid_ = true;

    if (save_pending_ &&
        save_.get() == snapshot.get()) {
        completed_save = std::move(save_);
        save_pending_ = false;
    }

    xSemaphoreGive(lock_);
    old_index.reset();
    completed_save.reset();

    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Durable night index saved nights=%lu crc=0x%08lx\n",
              static_cast<unsigned long>(snapshot->count()),
              static_cast<unsigned long>(written_crc));
    return true;
}

void ReportDurableNightIndex::retry_later_locked(uint32_t delay_ms) const {
    save_requested_ms_ = millis() + delay_ms;
}

}  // namespace aircannect
