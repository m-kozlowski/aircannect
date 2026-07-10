#include "report_night_index_service.h"

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

ReportNightIndexService::ReportNightIndexService(
    ReportSummaryRuntime &summary,
    ReportNightIndexRuntime &runtime,
    ReportEdfCatalogContext &edf_catalog)
    : summary_(summary),
      runtime_(runtime),
      edf_catalog_(edf_catalog) {}

bool ReportNightIndexService::begin() {
    return cache_.begin();
}

bool ReportNightIndexService::build(ReportIndexedNight *out,
                                    size_t capacity,
                                    size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    ReportNightIndexCacheKey cache_key;
    if (!this->cache_key(cache_key)) {
        return false;
    }

    if (cache_.copy(cache_key, out, capacity, count)) {
        return true;
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
        build_uncached(fresh, AC_REPORT_SUMMARY_RECORD_MAX, fresh_count);
    if (!ready) {
        Memory::free(fresh);
        return false;
    }

    (void)cache_.publish(cache_key, fresh, fresh_count);

    count = fresh_count < capacity ? fresh_count : capacity;
    if (count > 0) {
        memcpy(out, fresh, count * sizeof(ReportIndexedNight));
    }
    Memory::free(fresh);
    return true;
}

bool ReportNightIndexService::build_uncached(ReportIndexedNight *out,
                                             size_t capacity,
                                             size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    ReportNightIndex index(out, capacity);
    (void)runtime_.seed_from_durable(index);

    if (summary_.take(pdMS_TO_TICKS(20))) {
        const ReportSummaryRecord *records = summary_.records();
        const size_t raw_count = records ? summary_.record_count() : 0;
        for (size_t i = 0; i < raw_count; ++i) {
            if (!index.add_summary_record(records[i])) break;
        }
        summary_.give();
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
                runtime_.schedule_durable_save(out, count, crc);
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
    const bool have_catalog_status = edf_catalog_.status(catalog_status, 0);
    if (!have_catalog_status ||
        catalog_status.state != EdfReportCatalogState::Ready) {
        if (!have_catalog_status ||
            catalog_status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_.request_refresh();
        }
        return finish_index(!have_catalog_status ||
                            catalog_status.state !=
                                EdfReportCatalogState::Error,
                            false);
    }

    int32_t timezone_offset_min = 0;
    const bool have_timezone =
        edf_catalog_.timezone_offset_minutes(timezone_offset_min);
    const size_t catalog_count = edf_catalog_.session_count();

    EdfReportSessionDescriptor *session_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!session_scratch) {
        Memory::free(sort_scratch);
        log_report_alloc_failed("report_night_edf_session_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }

    for (size_t i = 0; i < catalog_count; ++i) {
        if (!edf_catalog_.copy_session(i, *session_scratch)) continue;
        if (!edf_catalog_.session_reportable(*session_scratch)) continue;

        if (!index.add_edf_session(*session_scratch,
                                   have_timezone,
                                   timezone_offset_min)) {
            Memory::free(session_scratch);
            Memory::free(sort_scratch);
            return false;
        }
    }

    Memory::free(session_scratch);

    return finish_index(false, true);
}

bool ReportNightIndexService::cache_key(ReportNightIndexCacheKey &key) const {
    key = {};
    key.catalog_present = edf_catalog_.present();
    key.catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);

    if (summary_.take(pdMS_TO_TICKS(20))) {
        key.summary_revision = summary_.status().revision;
        summary_.give();
    } else {
        return false;
    }

    if (!edf_catalog_) return true;

    EdfReportCatalogStatus catalog_status;
    if (!edf_catalog_.status(catalog_status, 0)) {
        key.catalog_state =
            static_cast<uint8_t>(EdfReportCatalogState::Refreshing);
        return true;
    }
    key.catalog_state = static_cast<uint8_t>(catalog_status.state);
    key.catalog_refresh_id = catalog_status.refresh_id;
    return true;
}

bool ReportNightIndexService::by_therapy_index(
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
    if (build(snapshot, AC_REPORT_SUMMARY_RECORD_MAX, count)) {
        found = ReportNightIndex::by_therapy_index(snapshot,
                                                   count,
                                                   therapy_index,
                                                   out);
    }

    Memory::free(snapshot);
    return found;
}

bool ReportNightIndexService::by_start(uint64_t night_start_ms,
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
    if (build(snapshot, AC_REPORT_SUMMARY_RECORD_MAX, count)) {
        found = ReportNightIndex::by_start(snapshot,
                                           count,
                                           night_start_ms,
                                           out,
                                           therapy_index_out);
    }

    Memory::free(snapshot);
    return found;
}

void ReportNightIndexService::format_result_etag(
    const ReportIndexedNight &night,
    char *out,
    size_t out_size) const {
    runtime_.format_result_etag(night, out, out_size);
}

}  // namespace aircannect
