#include "report_manager.h"

#include <algorithm>
#include <stdint.h>
#include <string.h>

#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"
#include "report_night_index_store.h"

namespace aircannect {

bool ReportManager::build_indexed_nights(ReportIndexedNight *out,
                                         size_t capacity,
                                         size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    uint32_t catalog_refresh_id = 0;
    if (!index_cache_key(summary_revision,
                         catalog_present,
                         catalog_state,
                         catalog_refresh_id)) {
        return false;
    }

    if (index_cache_lock_ &&
        xSemaphoreTake(index_cache_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        const bool copied = copy_index_cache_locked(out,
                                                    capacity,
                                                    count,
                                                    summary_revision,
                                                    catalog_present,
                                                    catalog_state,
                                                    catalog_refresh_id);
        xSemaphoreGive(index_cache_lock_);
        if (copied) return true;
    }

    ReportIndexedNight *fresh =
        static_cast<ReportIndexedNight *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportIndexedNight),
            false));
    if (!fresh) {
        log_report_alloc_failed(
            "report_night_index_build",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t fresh_count = 0;
    const bool ready =
        build_indexed_nights_uncached(fresh,
                                      AC_REPORT_SUMMARY_RECORD_MAX,
                                      fresh_count);
    if (!ready) {
        Memory::free(fresh);
        return false;
    }

    if (index_cache_lock_ &&
        xSemaphoreTake(index_cache_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        (void)publish_index_cache_locked(fresh,
                                         fresh_count,
                                         summary_revision,
                                         catalog_present,
                                         catalog_state,
                                         catalog_refresh_id);
        xSemaphoreGive(index_cache_lock_);
    }

    count = fresh_count < capacity ? fresh_count : capacity;
    if (count > 0) {
        memcpy(out, fresh, count * sizeof(ReportIndexedNight));
    }
    Memory::free(fresh);
    return true;
}

bool ReportManager::build_indexed_nights_uncached(ReportIndexedNight *out,
                                                  size_t capacity,
                                                  size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    ReportNightIndex index(out, capacity);
    (void)seed_index_from_durable(index);

    if (take_summary_lock(pdMS_TO_TICKS(20))) {
        const size_t raw_count = records_ ? record_count_ : 0;
        for (size_t i = 0; i < raw_count; ++i) {
            if (!index.add_summary_record(records_[i])) break;
        }
        give_summary_lock();
    }

    ReportIndexedNight *sort_scratch =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            sizeof(ReportIndexedNight),
            false));
    if (!sort_scratch) {
        log_report_alloc_failed("report_night_index_sort",
                                sizeof(ReportIndexedNight));
        return false;
    }

    auto finish_index = [&](bool catalog_pending,
                            bool authoritative) -> bool {
        if (!index.finish(sort_scratch)) {
            Memory::free(sort_scratch);
            return false;
        }

        count = index.count();
        if (catalog_pending) {
            for (size_t i = 0; i < count; ++i) {
                out[i].edf_catalog_pending =
                    !out[i].has_edf ||
                    !indexed_night_summary_ranges_covered_by_data(out[i]);
            }
        }

        if (authoritative) {
            uint32_t crc = 0;
            if (ReportNightIndexStore::content_crc(out, count, crc)) {
                schedule_durable_night_index_save(out, count, crc);
            }
        }

        Memory::free(sort_scratch);
        sort_scratch = nullptr;
        return true;
    };

    if (!edf_catalog_) {
        return finish_index(false, false);
    }

    EdfReportCatalogStatus catalog_status;
    const bool have_catalog_status = edf_catalog_->status(catalog_status, 0);
    if (!have_catalog_status ||
        catalog_status.state != EdfReportCatalogState::Ready) {
        if (!have_catalog_status ||
            catalog_status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_->request_refresh();
        }
        return finish_index(!have_catalog_status ||
                            catalog_status.state !=
                                EdfReportCatalogState::Error,
                            false);
    }

    int32_t timezone_offset_min = 0;
    const bool have_timezone =
        edf_catalog_->timezone_offset_minutes(timezone_offset_min);
    const size_t catalog_count = edf_catalog_->session_count();

    EdfReportSessionDescriptor *session_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!session_scratch) {
        Memory::free(sort_scratch);
        log_report_alloc_failed("report_night_edf_session_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        Memory::free(session_scratch);
        Memory::free(sort_scratch);
        log_report_alloc_failed("report_night_edf_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < catalog_count; ++i) {
            if (!edf_catalog_->copy_session(i, *session_scratch)) continue;

            const bool has_numeric =
                edf_session_has_report_numeric(*session_scratch);
            const bool has_annotation =
                edf_session_has_report_annotation(*session_scratch);
            if (pass == 0) {
                if (!has_numeric ||
                    !edf_catalog_session_reportable(*session_scratch,
                                                    *marker_scratch)) {
                    continue;
                }
            } else {
                if (has_numeric || !has_annotation ||
                    !edf_catalog_session_reportable(*session_scratch,
                                                    *marker_scratch)) {
                    continue;
                }
            }

            if (!index.add_edf_session(*session_scratch,
                                       have_timezone,
                                       timezone_offset_min)) {
                break;
            }
        }
    }

    Memory::free(marker_scratch);
    Memory::free(session_scratch);

    return finish_index(false, true);
}

bool ReportManager::index_cache_key(
    uint32_t &summary_revision,
    bool &catalog_present,
    uint8_t &catalog_state,
    uint32_t &catalog_refresh_id) const {
    summary_revision = 0;
    catalog_present = edf_catalog_ != nullptr;
    catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    catalog_refresh_id = 0;

    if (take_summary_lock(pdMS_TO_TICKS(20))) {
        summary_revision = summary_status_.revision;
        give_summary_lock();
    } else {
        return false;
    }

    if (!edf_catalog_) return true;

    EdfReportCatalogStatus catalog_status;
    if (!edf_catalog_->status(catalog_status, 0)) {
        catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Refreshing);
        return true;
    }
    catalog_state = static_cast<uint8_t>(catalog_status.state);
    catalog_refresh_id = catalog_status.refresh_id;
    return true;
}

bool ReportManager::index_cache_matches(
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    return index_cache_valid_ &&
           index_cache_summary_revision_ == summary_revision &&
           index_cache_catalog_present_ == catalog_present &&
           index_cache_catalog_state_ == catalog_state &&
           index_cache_catalog_refresh_id_ == catalog_refresh_id &&
           index_cache_ != nullptr;
}

bool ReportManager::copy_index_cache_locked(
    ReportIndexedNight *out,
    size_t capacity,
    size_t &count,
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    count = 0;
    if (!out || capacity == 0 ||
        !index_cache_matches(summary_revision,
                             catalog_present,
                             catalog_state,
                             catalog_refresh_id)) {
        return false;
    }

    count = index_cache_count_ < capacity ? index_cache_count_ : capacity;
    if (count > 0) {
        memcpy(out, index_cache_, count * sizeof(ReportIndexedNight));
    }
    return true;
}

bool ReportManager::publish_index_cache_locked(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    if (!src) return false;

    if (!index_cache_) {
        index_cache_ = static_cast<ReportIndexedNight *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportIndexedNight),
            false));
        if (!index_cache_) {
            log_report_alloc_failed(
                "report_night_index_cache",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            index_cache_valid_ = false;
            index_cache_count_ = 0;
            return false;
        }
    }

    const size_t stored = count < AC_REPORT_SUMMARY_RECORD_MAX
                              ? count
                              : AC_REPORT_SUMMARY_RECORD_MAX;
    if (stored > 0) {
        memcpy(index_cache_, src, stored * sizeof(ReportIndexedNight));
    }
    if (stored < index_cache_count_) {
        memset(index_cache_ + stored,
               0,
               (index_cache_count_ - stored) * sizeof(ReportIndexedNight));
    }

    index_cache_count_ = stored;
    index_cache_summary_revision_ = summary_revision;
    index_cache_catalog_present_ = catalog_present;
    index_cache_catalog_state_ = catalog_state;
    index_cache_catalog_refresh_id_ = catalog_refresh_id;
    index_cache_valid_ = true;
    return true;
}

bool ReportManager::indexed_night_by_therapy_index(
    size_t therapy_index,
    ReportIndexedNight &out) const {
    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "report_night_index_lookup",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t count = 0;
    bool found = false;
    if (build_indexed_nights(snapshot,
                             AC_REPORT_SUMMARY_RECORD_MAX,
                             count)) {
        found = ReportNightIndex::by_therapy_index(snapshot,
                                                   count,
                                                   therapy_index,
                                                   out);
    }

    Memory::free(snapshot);
    return found;
}

bool ReportManager::indexed_night_by_start(uint64_t night_start_ms,
                                           ReportIndexedNight &out,
                                           size_t *therapy_index_out) const {
    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "report_night_index_lookup",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t count = 0;
    bool found = false;
    if (build_indexed_nights(snapshot,
                             AC_REPORT_SUMMARY_RECORD_MAX,
                             count)) {
        found = ReportNightIndex::by_start(snapshot,
                                           count,
                                           night_start_ms,
                                           out,
                                           therapy_index_out);
    }

    Memory::free(snapshot);
    return found;
}

}  // namespace aircannect
