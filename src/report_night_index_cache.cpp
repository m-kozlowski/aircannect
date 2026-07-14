#include "report_night_index_cache.h"

#include <utility>

namespace aircannect {

bool report_night_index_cache_key_equal(
    const ReportNightIndexCacheKey &a,
    const ReportNightIndexCacheKey &b) {
    return a.summary_revision == b.summary_revision &&
           a.catalog_present == b.catalog_present &&
           a.catalog_state == b.catalog_state &&
           a.catalog_refresh_id == b.catalog_refresh_id &&
           a.timezone_revision == b.timezone_revision;
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

ReportNightIndexCacheAcquire ReportNightIndexCache::acquire(
    const ReportNightIndexCacheKey &key,
    ReportNightIndexSnapshotRef &out) {
    out.reset();
    if (!lock_) return ReportNightIndexCacheAcquire::Error;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ReportNightIndexCacheAcquire::Busy;
    }

    if (matches_locked(key)) {
        out = snapshot_;
        xSemaphoreGive(lock_);
        return ReportNightIndexCacheAcquire::Hit;
    }

    if (build_active_) {
        xSemaphoreGive(lock_);
        return ReportNightIndexCacheAcquire::Busy;
    }

    build_active_ = true;
    build_key_ = key;
    xSemaphoreGive(lock_);
    return ReportNightIndexCacheAcquire::Build;
}

bool ReportNightIndexCache::complete_build(
    const ReportNightIndexCacheKey &key,
    const ReportNightIndexSnapshotRef &snapshot) {
    if (!lock_ || !snapshot) return false;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return false;

    if (!build_active_ ||
        !report_night_index_cache_key_equal(build_key_, key)) {
        xSemaphoreGive(lock_);
        return false;
    }

    ReportNightIndexSnapshotRef old_snapshot = std::move(snapshot_);
    snapshot_ = snapshot;
    key_ = key;
    valid_ = true;
    build_active_ = false;
    build_key_ = {};

    xSemaphoreGive(lock_);
    old_snapshot.reset();
    return true;
}

void ReportNightIndexCache::cancel_build(
    const ReportNightIndexCacheKey &key) {
    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return;

    if (build_active_ &&
        report_night_index_cache_key_equal(build_key_, key)) {
        build_active_ = false;
        build_key_ = {};
    }

    xSemaphoreGive(lock_);
}

void ReportNightIndexCache::clear() {
    if (!lock_ || xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return;

    ReportNightIndexSnapshotRef old_snapshot = std::move(snapshot_);
    valid_ = false;
    key_ = {};
    build_active_ = false;
    build_key_ = {};

    xSemaphoreGive(lock_);
    old_snapshot.reset();
}

bool ReportNightIndexCache::matches_locked(
    const ReportNightIndexCacheKey &key) const {
    return valid_ &&
           snapshot_ != nullptr &&
           report_night_index_cache_key_equal(key_, key);
}

}  // namespace aircannect
