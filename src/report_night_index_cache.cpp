#include "report_night_index_cache.h"

#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"

namespace aircannect {

bool report_night_index_cache_key_equal(
    const ReportNightIndexCacheKey &a,
    const ReportNightIndexCacheKey &b) {
    return a.summary_revision == b.summary_revision &&
           a.catalog_present == b.catalog_present &&
           a.catalog_state == b.catalog_state &&
           a.catalog_refresh_id == b.catalog_refresh_id;
}

ReportNightIndexCache::~ReportNightIndexCache() {
    clear();

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportNightIndexCache::begin() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool ReportNightIndexCache::copy(const ReportNightIndexCacheKey &key,
                                 ReportIndexedNight *out,
                                 size_t capacity,
                                 size_t &count) const {
    count = 0;
    if (!lock_ || !out || capacity == 0) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    if (!matches_locked(key)) {
        xSemaphoreGive(lock_);
        return false;
    }

    count = count_ < capacity ? count_ : capacity;
    if (count > 0) {
        memcpy(out, entries_, count * sizeof(ReportIndexedNight));
    }

    xSemaphoreGive(lock_);
    return true;
}

bool ReportNightIndexCache::publish(const ReportNightIndexCacheKey &key,
                                    const ReportIndexedNight *src,
                                    size_t count) {
    if (!lock_ || !src) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return false;

    if (!ensure_entries_locked()) {
        xSemaphoreGive(lock_);
        return false;
    }

    const size_t stored = count < AC_REPORT_SUMMARY_RECORD_MAX
                              ? count
                              : AC_REPORT_SUMMARY_RECORD_MAX;
    if (stored > 0) {
        memcpy(entries_, src, stored * sizeof(ReportIndexedNight));
    }
    if (stored < count_) {
        memset(entries_ + stored,
               0,
               (count_ - stored) * sizeof(ReportIndexedNight));
    }

    count_ = stored;
    key_ = key;
    valid_ = true;

    xSemaphoreGive(lock_);
    return true;
}

void ReportNightIndexCache::clear() {
    if (lock_) xSemaphoreTake(lock_, portMAX_DELAY);

    clear_entries_locked();

    if (lock_) xSemaphoreGive(lock_);
}

bool ReportNightIndexCache::matches_locked(
    const ReportNightIndexCacheKey &key) const {
    return valid_ &&
           entries_ != nullptr &&
           report_night_index_cache_key_equal(key_, key);
}

bool ReportNightIndexCache::ensure_entries_locked() {
    if (entries_) return true;

    entries_ = static_cast<ReportIndexedNight *>(Memory::calloc_large(
        AC_REPORT_SUMMARY_RECORD_MAX,
        sizeof(ReportIndexedNight),
        false));
    if (entries_) return true;

    log_report_alloc_failed(
        "report_night_index_cache",
        AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
    valid_ = false;
    count_ = 0;
    return false;
}

void ReportNightIndexCache::clear_entries_locked() {
    Memory::free(entries_);
    entries_ = nullptr;
    count_ = 0;
    valid_ = false;
    key_ = {};
}

}  // namespace aircannect
