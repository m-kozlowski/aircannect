#include "report_store_internal.h"

#include <stdio.h>

#include "board_report.h"
#include "debug_log.h"
#include "report_legacy_storage.h"

namespace aircannect {
namespace ReportStore {

using namespace ReportStoreInternal;

bool write_coverage_batch(const char *source,
                          const ReportStoreCoverageRecord *records,
                          size_t count) {
    if (!source || !source[0]) {
        note_error("bad_coverage_source", &current.coverage_write_errors);
        return false;
    }

    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        note_error("coverage_scratch_alloc_failed",
                   &current.coverage_write_errors);
        return false;
    }

    ReportLegacyStorageGuard g;
    if (!ensure_layout()) return false;

    // One load -> coalesce ALL records -> one atomic rewrite. Doing this per
    // record (the old per-night call) re-read+rewrote the whole file each time,
    // O(nights x filesize) SD on the main loop -> CAN RX starves -> framing CRC.
    size_t set_count = load_coverage(source, scratch);
    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        // Only Complete intervals are persisted; an Incomplete span is just a gap.
        if (records[i].state != ReportStoreCoverageState::Complete) continue;
        if (!valid_coverage_record(records[i])) {
            note_error("bad_coverage_record", &current.coverage_write_errors);
            continue;
        }
        coverage_insert(scratch, set_count, records[i]);
        ++added;
    }
    if (added == 0) {
        set_error(current.last_error, sizeof(current.last_error), "");
        return true;
    }
    if (!rewrite_coverage_file(source, scratch, set_count)) {
        note_error("coverage_write_failed", &current.coverage_write_errors);
        return false;
    }

    current.coverage_records_written += static_cast<uint32_t>(added);
    set_error(current.last_error, sizeof(current.last_error), "");
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "coverage source=%s intervals=%u added=%u\n",
              source, static_cast<unsigned>(set_count),
              static_cast<unsigned>(added));
    return true;
}

bool coverage_complete(const char *source,
                       int64_t start_ms,
                       int64_t end_ms,
                       uint32_t parser_schema) {
    int64_t missing_ms = start_ms;
    if (!coverage_first_missing(source,
                                start_ms,
                                end_ms,
                                parser_schema,
                                missing_ms)) {
        return false;
    }
    return missing_ms >= end_ms;
}

bool coverage_first_missing(const char *source,
                            int64_t start_ms,
                            int64_t end_ms,
                            uint32_t parser_schema,
                            int64_t &missing_ms) {
    missing_ms = start_ms;
    if (!source || !source[0] || start_ms < 0 || end_ms <= start_ms ||
        parser_schema == 0) {
        note_error("bad_coverage_query", &current.coverage_read_errors);
        return false;
    }

    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        note_error("coverage_scratch_alloc_failed",
                   &current.coverage_read_errors);
        return false;
    }

    ReportLegacyStorageGuard g;
    const size_t count = load_coverage(source, scratch);
    const int64_t tol = AC_REPORT_COVERAGE_TOLERANCE_MS;
    int64_t covered_until = start_ms;

    // scratch is sorted by start and same-tag coalesced, so one walk over the
    // matching-schema intervals finds the first gap. Tolerance absorbs small
    // joint/boundary slack; a real interior gap (minutes) is not masked.
    for (size_t i = 0; i < count; ++i) {
        const ReportStoreCoverageRecord &iv = scratch[i];
        if (iv.parser_schema != parser_schema) continue;
        if (iv.start_ms > covered_until + tol) break;  // gap before this span
        if (iv.end_ms > covered_until) covered_until = iv.end_ms;
        if (covered_until + tol >= end_ms) {
            missing_ms = end_ms;
            set_error(current.last_error, sizeof(current.last_error), "");
            return true;
        }
    }

    missing_ms = covered_until < end_ms ? covered_until : end_ms;
    return true;
}

bool clear_coverage(const char *source,
                    int64_t start_ms,
                    int64_t end_ms,
                    uint32_t &deleted) {
    ReportLegacyStorageGuard g;
    deleted = 0;
    if (!source || !source[0] || start_ms < 0 || end_ms <= start_ms) {
        note_error("bad_coverage_clear_query",
                   &current.coverage_write_errors);
        return false;
    }

    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) {
        note_error("bad_coverage_path", &current.coverage_write_errors);
        return false;
    }
    if (!ReportLegacyStorage::exists(path)) return true;

    char tmp_path[REPORT_PATH_MAX + 8];
    const int tmp_written =
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tmp_written <= 0 ||
        static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
        note_error("bad_coverage_tmp", &current.coverage_write_errors);
        return false;
    }
    ReportLegacyStorage::remove(tmp_path);

    ReportLegacyFile in = ReportLegacyStorage::open(path, "r");
    ReportLegacyFile out = ReportLegacyStorage::open(tmp_path, "w");
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        ReportLegacyStorage::remove(tmp_path);
        note_error("coverage_clear_open_failed",
                   &current.coverage_write_errors);
        return false;
    }

    bool ok = true;
    bool kept = false;
    while (true) {
        uint8_t raw[COVERAGE_RECORD_SIZE];
        const int got = in.read(raw, sizeof(raw));
        if (got == 0) break;
        if (got != static_cast<int>(sizeof(raw))) {
            ok = false;
            note_error("coverage_short_read",
                       &current.coverage_write_errors);
            break;
        }

        ReportStoreCoverageRecord record;
        if (!decode_coverage_record(raw, record)) {
            // Drop unreadable/old-schema records during a clear (they are
            // effectively removed) instead of aborting the whole rewrite.
            deleted++;
            continue;
        }
        if (ranges_overlap(record.start_ms,
                           record.end_ms,
                           start_ms,
                           end_ms)) {
            deleted++;
            continue;
        }
        if (!write_all(out, raw, sizeof(raw))) {
            ok = false;
            note_error("coverage_rewrite_failed",
                       &current.coverage_write_errors);
            break;
        }
        kept = true;
    }
    in.close();
    out.close();

    if (!ok) {
        ReportLegacyStorage::remove(tmp_path);
        return false;
    }
    if (!ReportLegacyStorage::remove(path)) {
        ReportLegacyStorage::remove(tmp_path);
        note_error("coverage_remove_failed", &current.coverage_write_errors);
        return false;
    }
    if (kept) {
        if (!ReportLegacyStorage::rename(tmp_path, path)) {
            ReportLegacyStorage::remove(tmp_path);
            note_error("coverage_commit_failed",
                       &current.coverage_write_errors);
            return false;
        }
    } else {
        ReportLegacyStorage::remove(tmp_path);
    }

    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

}  // namespace ReportStore
}  // namespace aircannect
