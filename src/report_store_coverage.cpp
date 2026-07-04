#include "report_store_internal.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {

namespace {

// Coverage is stored per source as Complete intervals kept sorted by start and
// merged when same-tag (parser_schema/source_hash/origin) spans overlap or sit
// within the join tolerance; "missing" is simply a gap. A query is one walk for
// the first gap; a write loads, merges, and atomically rewrites, so refreshing a
// covered span never grows the file and old append-log files coalesce on first
// read. coverage_scratch is reused under the SD guard, which serialises every
// coverage op, so a single shared buffer is safe. It is lazy/PSRAM-backed:
// report coverage is storage/report work, not a CAN timing path.
ReportStoreCoverageRecord *coverage_scratch = nullptr;

ReportStoreCoverageRecord *ensure_coverage_scratch() {
    if (coverage_scratch) return coverage_scratch;

    coverage_scratch = static_cast<ReportStoreCoverageRecord *>(
        Memory::calloc_large(COVERAGE_MAX_INTERVALS,
                             sizeof(ReportStoreCoverageRecord)));
    return coverage_scratch;
}

bool coverage_same_tag(const ReportStoreCoverageRecord &a,
                       const ReportStoreCoverageRecord &b) {
    return a.parser_schema == b.parser_schema &&
           a.source_hash == b.source_hash && a.origin == b.origin;
}

// Insert rec into recs[] (kept sorted by start) and coalesce the same-tag run it
// now belongs to. Only Complete intervals are tracked; on overflow the interval
// is dropped (its span reads as missing and is re-fetched).
void coverage_insert(ReportStoreCoverageRecord *recs,
                     size_t &count,
                     const ReportStoreCoverageRecord &rec) {
    if (rec.state != ReportStoreCoverageState::Complete ||
        rec.end_ms <= rec.start_ms) {
        return;
    }
    if (count >= COVERAGE_MAX_INTERVALS) return;

    size_t pos = 0;
    while (pos < count && recs[pos].start_ms < rec.start_ms) ++pos;
    for (size_t i = count; i > pos; --i) recs[i] = recs[i - 1];
    recs[pos] = rec;
    ++count;

    // One left-to-right pass coalesces adjacent same-tag spans within tolerance.
    // It suffices because the array is sorted and a source has a single tag in
    // practice, so the whole run merges transitively (and any leftover adjacency
    // is still stitched by the query walk).
    const int64_t tol = AC_REPORT_COVERAGE_TOLERANCE_MS;
    size_t w = 0;
    for (size_t r = 1; r < count; ++r) {
        if (coverage_same_tag(recs[w], recs[r]) &&
            recs[r].start_ms <= recs[w].end_ms + tol) {
            if (recs[r].end_ms > recs[w].end_ms) {
                recs[w].end_ms = recs[r].end_ms;
            }
        } else {
            recs[++w] = recs[r];
        }
    }
    count = w + 1;
}

// Read a source's coverage file into recs[], coalescing as it goes; returns the
// interval count. Stale/old-schema records are skipped.
size_t load_coverage(const char *source, ReportStoreCoverageRecord *recs) {
    size_t count = 0;

    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) return 0;
    if (!Storage::exists(path)) return 0;

    File file = Storage::open(path, "r");
    if (!file) return 0;

    for (;;) {
        bool eof = false;
        ReportStoreCoverageRecord record;
        if (!read_coverage_record(file, record, eof)) {
            if (eof) break;
            current.coverage_read_errors++;  // skippable stale record
            continue;
        }
        current.coverage_records_read++;
        coverage_insert(recs, count, record);
    }
    file.close();
    return count;
}

// Atomically replace a source's coverage file with recs[] (tmp + rename); an
// empty set removes the file.
bool rewrite_coverage_file(const char *source,
                           const ReportStoreCoverageRecord *recs,
                           size_t count) {
    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) return false;

    char tmp[REPORT_PATH_MAX + 8];
    const int n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(tmp)) return false;

    Storage::remove(tmp);
    File out = Storage::open(tmp, "w");
    if (!out) return false;

    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
        uint8_t raw[COVERAGE_RECORD_SIZE];
        encode_coverage_record(raw, recs[i]);
        ok = write_all(out, raw, sizeof(raw));
    }
    out.close();
    if (!ok) {
        Storage::remove(tmp);
        return false;
    }

    Storage::remove(path);
    if (count == 0) {
        Storage::remove(tmp);
        return true;
    }
    if (!Storage::rename(tmp, path)) {
        Storage::remove(tmp);
        return false;
    }
    return true;
}

void integrity_note_error(ReportStoreIntegrityResult &out,
                          const char *error) {
    out.errors++;
    copy_cstr(out.last_error, sizeof(out.last_error), error);
}

bool check_coverage_file_integrity(const char *source,
                                   ReportStoreIntegrityResult &out,
                                   bool repair) {
    if (!source || !source[0]) return true;

    ReportStoreCoverageRecord *scratch = ensure_coverage_scratch();
    if (!scratch) {
        integrity_note_error(out, "coverage_scratch_alloc_failed");
        return false;
    }

    size_t count = 0;
    uint32_t dropped = 0;

    char path[REPORT_PATH_MAX];
    if (!build_coverage_path(source, path, sizeof(path))) {
        integrity_note_error(out, "bad_coverage_path");
        return false;
    }

    File file = Storage::open(path, "r");
    if (!file) {
        integrity_note_error(out, "coverage_open_failed");
        return false;
    }
    out.coverage_files_checked++;

    for (;;) {
        bool eof = false;
        ReportStoreCoverageRecord record;
        if (!read_coverage_record(file, record, eof)) {
            if (eof) break;
            dropped++;
            continue;
        }
        out.coverage_records_checked++;
        const size_t before = count;
        coverage_insert(scratch, count, record);
        if (count == before) {
            dropped++;
        }
    }
    file.close();

    out.coverage_records_dropped += dropped;
    if (!repair || dropped == 0) return true;
    if (!rewrite_coverage_file(source, scratch, count)) {
        integrity_note_error(out, "coverage_rewrite_failed");
        return false;
    }

    out.coverage_files_rewritten++;
    out.repaired = true;
    return true;
}

}  // namespace

bool ranges_overlap(int64_t left_start,
                    int64_t left_end,
                    int64_t right_start,
                    int64_t right_end) {
    return left_end > right_start && left_start < right_end;
}

bool check_coverage_integrity(ReportStoreIntegrityResult &out, bool repair) {
    char root_path[REPORT_PATH_MAX];
    snprintf(root_path, sizeof(root_path), "%s/coverage", BASE_DIR);

    File root = Storage::open(root_path, "r");
    if (!root) return true;
    if (!root.isDirectory()) {
        root.close();
        integrity_note_error(out, "coverage_path_not_dir");
        return false;
    }

    bool ok = true;
    while (true) {
        File child = root.openNextFile();
        if (!child) break;

        const bool child_is_dir = child.isDirectory();
        char child_name[REPORT_PATH_MAX];
        copy_cstr(child_name, sizeof(child_name), path_basename(child.name()));
        child.close();
        if (child_is_dir || !name_ends_with(child_name, ".idx")) continue;

        char source[72];
        copy_cstr(source, sizeof(source), child_name);
        char *suffix = strstr(source, ".idx");
        if (suffix) *suffix = '\0';
        if (!check_coverage_file_integrity(source, out, repair)) ok = false;
    }
    root.close();
    return ok;
}

}  // namespace ReportStoreInternal

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

    Storage::Guard g;
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

bool write_coverage(const char *source,
                    const ReportStoreCoverageRecord &record) {
    return write_coverage_batch(source, &record, 1);
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

    Storage::Guard g;
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
    Storage::Guard g;
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
    if (!Storage::exists(path)) return true;

    char tmp_path[REPORT_PATH_MAX + 8];
    const int tmp_written =
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    if (tmp_written <= 0 ||
        static_cast<size_t>(tmp_written) >= sizeof(tmp_path)) {
        note_error("bad_coverage_tmp", &current.coverage_write_errors);
        return false;
    }
    Storage::remove(tmp_path);

    File in = Storage::open(path, "r");
    File out = Storage::open(tmp_path, "w");
    if (!in || !out) {
        if (in) in.close();
        if (out) out.close();
        Storage::remove(tmp_path);
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
        Storage::remove(tmp_path);
        return false;
    }
    if (!Storage::remove(path)) {
        Storage::remove(tmp_path);
        note_error("coverage_remove_failed", &current.coverage_write_errors);
        return false;
    }
    if (kept) {
        if (!Storage::rename(tmp_path, path)) {
            Storage::remove(tmp_path);
            note_error("coverage_commit_failed",
                       &current.coverage_write_errors);
            return false;
        }
    } else {
        Storage::remove(tmp_path);
    }

    set_error(current.last_error, sizeof(current.last_error), "");
    return true;
}

}  // namespace ReportStore
}  // namespace aircannect
