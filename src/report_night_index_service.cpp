#include "report_night_index_service.h"

#include <algorithm>
#include <ctype.h>
#include <stdint.h>
#include <utility>

#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_index_scratch.h"
#include "report_manager_limits.h"

namespace aircannect {
namespace {

class UniqueNightKeySet {
public:
    UniqueNightKeySet()
        : keys_(static_cast<uint64_t *>(Memory::calloc_large(
              AC_REPORT_SUMMARY_RECORD_MAX,
              sizeof(uint64_t),
              false))) {}

    ~UniqueNightKeySet() { Memory::free(keys_); }

    UniqueNightKeySet(const UniqueNightKeySet &) = delete;
    UniqueNightKeySet &operator=(const UniqueNightKeySet &) = delete;

    explicit operator bool() const { return keys_ != nullptr; }

    bool add(uint64_t key, bool &inserted) {
        inserted = false;
        for (size_t i = 0; i < count_; ++i) {
            if (keys_[i] == key) return true;
        }
        if (count_ >= AC_REPORT_SUMMARY_RECORD_MAX) return false;

        keys_[count_++] = key;
        inserted = true;
        return true;
    }

    uint64_t *keys_ = nullptr;
    size_t count_ = 0;
};

bool parse_sleep_day_key(const char *sleep_day, uint64_t &out) {
    out = 0;
    if (!sleep_day) return false;

    for (size_t i = 0; i < 8; ++i) {
        const unsigned char ch = static_cast<unsigned char>(sleep_day[i]);
        if (!isdigit(ch)) return false;
        out = out * 10u + static_cast<uint64_t>(ch - '0');
    }
    return sleep_day[8] == '\0';
}

bool add_summary_keys(UniqueNightKeySet &starts,
                      UniqueNightKeySet &sleep_days,
                      const ReportSummaryRecord &record,
                      size_t &capacity) {
    bool start_inserted = false;
    if (record.start_ms == 0) {
        capacity++;
    } else if (!starts.add(record.start_ms, start_inserted)) {
        return false;
    } else if (start_inserted) {
        capacity++;
    }

    char sleep_day[9] = {};
    if (!report_summary_sleep_day_yyyymmdd(record,
                                           sleep_day,
                                           sizeof(sleep_day))) {
        return true;
    }

    uint64_t day_key = 0;
    if (!parse_sleep_day_key(sleep_day, day_key)) return true;

    bool day_inserted = false;
    return sleep_days.add(day_key, day_inserted);
}

}  // namespace

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

ReportNightIndexSnapshotResult ReportNightIndexService::snapshot(
    ReportNightIndexSnapshotRef &out,
    const char **error_out) const {
    out.reset();
    if (error_out) *error_out = nullptr;

    ReportNightIndexCacheKey key;
    if (!cache_key(key)) {
        if (error_out) *error_out = "cache_key_busy";
        return ReportNightIndexSnapshotResult::Busy;
    }

    switch (cache_.acquire(key, out)) {
        case ReportNightIndexCacheAcquire::Hit:
            return ReportNightIndexSnapshotResult::Ready;
        case ReportNightIndexCacheAcquire::Busy:
            if (error_out) *error_out = "snapshot_busy";
            return ReportNightIndexSnapshotResult::Busy;
        case ReportNightIndexCacheAcquire::Error:
            if (error_out) *error_out = "snapshot_cache_unavailable";
            return ReportNightIndexSnapshotResult::Failed;
        case ReportNightIndexCacheAcquire::Build:
            break;
    }

    ReportNightIndexSnapshotRef built;
    bool authoritative = false;
    const char *build_error = nullptr;
    const ReportNightIndexSnapshotResult result =
        build_uncached(built, authoritative, build_error);
    if (result != ReportNightIndexSnapshotResult::Ready || !built) {
        cache_.cancel_build(key);
        if (error_out) *error_out = build_error ? build_error
                                                : "snapshot_build_failed";
        return result;
    }

    ReportNightIndexCacheKey current_key;
    if (!cache_key(current_key) ||
        !report_night_index_cache_key_equal(key, current_key)) {
        cache_.cancel_build(key);
        if (error_out) *error_out = "snapshot_source_changed";
        return ReportNightIndexSnapshotResult::Busy;
    }

    if (!cache_.complete_build(key, built)) {
        cache_.cancel_build(key);
        if (error_out) *error_out = "snapshot_publish_failed";
        return ReportNightIndexSnapshotResult::Failed;
    }

    if (authoritative) runtime_.schedule_durable_save(built);

    out = std::move(built);
    return ReportNightIndexSnapshotResult::Ready;
}

ReportNightIndexSnapshotResult ReportNightIndexService::build_uncached(
    ReportNightIndexSnapshotRef &out,
    bool &authoritative,
    const char *&error) const {
    out.reset();
    authoritative = false;
    error = nullptr;

    EdfReportCatalogStatus catalog_status;
    const bool have_catalog_status =
        edf_catalog_ && edf_catalog_.status(catalog_status, 0);
    const bool catalog_ready =
        have_catalog_status &&
        catalog_status.state == EdfReportCatalogState::Ready;

    ReportNightIndexSnapshotRef durable;
    if (!catalog_ready) (void)runtime_.durable_snapshot(durable);

    UniqueNightKeySet night_starts;
    UniqueNightKeySet sleep_days;
    if (!night_starts || !sleep_days) {
        log_report_alloc_failed(
            "report_night_index_keys",
            2 * AC_REPORT_SUMMARY_RECORD_MAX * sizeof(uint64_t));
        error = "night_key_alloc";
        return ReportNightIndexSnapshotResult::Failed;
    }

    size_t capacity = 0;
    if (durable) {
        for (size_t i = 0; i < durable->count(); ++i) {
            const ReportSummaryRecord *record = durable->summary_at(i);
            if (!record || !record->valid) continue;
            if (!add_summary_keys(night_starts,
                                  sleep_days,
                                  *record,
                                  capacity)) {
                error = "night_key_capacity";
                return ReportNightIndexSnapshotResult::Failed;
            }
        }
    }

    if (!summary_.take(pdMS_TO_TICKS(20))) {
        error = "summary_busy";
        return ReportNightIndexSnapshotResult::Busy;
    }

    const ReportSummaryRecord *summary_records = summary_.records();
    const size_t summary_count = summary_.record_count();
    for (size_t i = 0; summary_records && i < summary_count; ++i) {
        if (!summary_records[i].valid) continue;
        if (!add_summary_keys(night_starts,
                              sleep_days,
                              summary_records[i],
                              capacity)) {
            summary_.give();
            error = "night_key_capacity";
            return ReportNightIndexSnapshotResult::Failed;
        }
    }
    summary_.give();

    EdfReportSessionDescriptor *session_scratch = nullptr;
    const size_t catalog_count = catalog_ready
        ? edf_catalog_.session_count()
        : 0;
    if (catalog_ready && catalog_count > 0) {
        session_scratch = static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
        if (!session_scratch) {
            log_report_alloc_failed("report_night_edf_session_scratch",
                                    sizeof(EdfReportSessionDescriptor));
            error = "edf_session_alloc";
            return ReportNightIndexSnapshotResult::Failed;
        }

        for (size_t i = 0; i < catalog_count; ++i) {
            if (!edf_catalog_.copy_session(i, *session_scratch)) continue;
            if (!edf_report_session_reportable(*session_scratch)) continue;

            uint64_t day_key = 0;
            if (!parse_sleep_day_key(session_scratch->sleep_day, day_key)) {
                continue;
            }

            bool inserted = false;
            if (!sleep_days.add(day_key, inserted)) {
                Memory::free(session_scratch);
                error = "night_key_capacity";
                return ReportNightIndexSnapshotResult::Failed;
            }
            if (inserted) capacity++;
        }
    }

    if (capacity > AC_REPORT_SUMMARY_RECORD_MAX) {
        Memory::free(session_scratch);
        error = "night_index_capacity";
        return ReportNightIndexSnapshotResult::Failed;
    }

    if (capacity == 0) {
        Memory::free(session_scratch);
        out = ReportNightIndexSnapshot::create(nullptr, 0);
        authoritative = catalog_ready;
        if (!out) {
            error = "snapshot_alloc";
            return ReportNightIndexSnapshotResult::Failed;
        }
        return ReportNightIndexSnapshotResult::Ready;
    }

    ReportIndexedNight *nights = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(capacity,
                             sizeof(ReportIndexedNight),
                             false));
    if (!nights) {
        Memory::free(session_scratch);
        log_report_alloc_failed("report_night_index_build",
                                capacity * sizeof(ReportIndexedNight));
        error = "night_index_alloc";
        return ReportNightIndexSnapshotResult::Failed;
    }

    ReportNightIndex index(nights, capacity);
    if (durable) {
        ScopedIndexedNight durable_night("report_night_index_durable_seed");
        if (!durable_night) {
            Memory::free(session_scratch);
            Memory::free(nights);
            error = "durable_seed_alloc";
            return ReportNightIndexSnapshotResult::Failed;
        }

        for (size_t i = 0; i < durable->count(); ++i) {
            if (!durable->materialize(i, durable_night.get()) ||
                !index.add_indexed_night(durable_night.get())) {
                Memory::free(session_scratch);
                Memory::free(nights);
                error = "durable_seed_failed";
                return ReportNightIndexSnapshotResult::Failed;
            }
        }
    }

    if (!summary_.take(pdMS_TO_TICKS(20))) {
        Memory::free(session_scratch);
        Memory::free(nights);
        error = "summary_busy";
        return ReportNightIndexSnapshotResult::Busy;
    }

    summary_records = summary_.records();
    const size_t raw_summary_count = summary_.record_count();
    bool summary_ok = true;
    for (size_t i = 0; summary_records && i < raw_summary_count; ++i) {
        if (!index.add_summary_record(summary_records[i])) {
            summary_ok = false;
            break;
        }
    }
    summary_.give();
    if (!summary_ok) {
        Memory::free(session_scratch);
        Memory::free(nights);
        error = "night_index_capacity";
        return ReportNightIndexSnapshotResult::Failed;
    }

    ScopedIndexedNight sort_scratch("report_night_index_sort");
    if (!sort_scratch) {
        Memory::free(session_scratch);
        Memory::free(nights);
        error = "sort_alloc";
        return ReportNightIndexSnapshotResult::Failed;
    }

    auto finish = [&](bool catalog_pending) {
        if (!index.finish(&sort_scratch.get())) {
            error = "night_index_finish";
            return ReportNightIndexSnapshotResult::Failed;
        }

        const size_t count = index.count();
        if (catalog_pending) {
            for (size_t i = 0; i < count; ++i) {
                nights[i].edf_catalog_pending =
                    !nights[i].has_edf ||
                    !indexed_night_summary_ranges_covered_by_data(nights[i]);
            }
        }

        out = ReportNightIndexSnapshot::create(nights, count);
        if (!out) {
            error = "snapshot_alloc";
            return ReportNightIndexSnapshotResult::Failed;
        }

        return ReportNightIndexSnapshotResult::Ready;
    };

    if (!edf_catalog_) {
        const ReportNightIndexSnapshotResult result = finish(false);
        Memory::free(session_scratch);
        Memory::free(nights);
        return result;
    }

    if (!catalog_ready) {
        if (!have_catalog_status ||
            catalog_status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_.request_refresh();
        }

        const bool pending = !have_catalog_status ||
            catalog_status.state != EdfReportCatalogState::Error;
        const ReportNightIndexSnapshotResult result = finish(pending);
        Memory::free(session_scratch);
        Memory::free(nights);
        return result;
    }

    for (size_t i = 0; i < catalog_count; ++i) {
        if (!edf_catalog_.copy_session(i, *session_scratch)) continue;
        if (!edf_report_session_reportable(*session_scratch)) continue;

        const ReportSummaryRecord *matching_summary = nullptr;
        for (size_t night_index = 0;
             night_index < index.count();
             ++night_index) {
            if (report_summary_matches_sleep_day(
                    nights[night_index].summary,
                    session_scratch->sleep_day)) {
                matching_summary = &nights[night_index].summary;
                break;
            }
        }

        int32_t timezone_offset_min = 0;
        const bool have_timezone =
            edf_catalog_.resolve_session_timezone(*session_scratch,
                                                  matching_summary,
                                                  timezone_offset_min);

        if (!index.add_edf_session(*session_scratch,
                                   have_timezone,
                                   timezone_offset_min)) {
            Memory::free(session_scratch);
            Memory::free(nights);
            error = "night_index_capacity";
            return ReportNightIndexSnapshotResult::Failed;
        }
    }

    authoritative = true;
    const ReportNightIndexSnapshotResult result = finish(false);
    Memory::free(session_scratch);
    Memory::free(nights);
    return result;
}

bool ReportNightIndexService::cache_key(
    ReportNightIndexCacheKey &key) const {
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
    key.timezone_revision = catalog_status.timezone_revision;
    return true;
}

ReportNightIndexLookupResult ReportNightIndexService::by_therapy_index(
    size_t therapy_index,
    ReportIndexedNight &out) const {
    ReportNightIndexSnapshotRef current;
    const ReportNightIndexSnapshotResult snapshot_result = snapshot(current);
    const bool found = snapshot_result == ReportNightIndexSnapshotResult::Ready &&
                       current &&
                       current->by_therapy_index(therapy_index, out);

    return classify_report_night_index_lookup(snapshot_result,
                                              current != nullptr,
                                              found);
}

ReportNightIndexLookupResult ReportNightIndexService::by_start(
    uint64_t night_start_ms,
    ReportIndexedNight &out,
    size_t *therapy_index_out) const {
    ReportNightIndexSnapshotRef current;
    const ReportNightIndexSnapshotResult snapshot_result = snapshot(current);
    const bool found = snapshot_result == ReportNightIndexSnapshotResult::Ready &&
                       current &&
                       current->by_start(night_start_ms,
                                         out,
                                         therapy_index_out);

    return classify_report_night_index_lookup(snapshot_result,
                                              current != nullptr,
                                              found);
}

void ReportNightIndexService::format_result_etag(
    const ReportIndexedNight &night,
    char *out,
    size_t out_size) const {
    runtime_.format_result_etag(night, out, out_size);
}

}  // namespace aircannect
