#include "report_store_internal.h"

#include <stdio.h>

#include "board_report.h"
#include "memory_manager.h"
#include "storage_manager.h"

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

bool coverage_same_tag(const ReportStoreCoverageRecord &a,
                       const ReportStoreCoverageRecord &b) {
    return a.parser_schema == b.parser_schema &&
           a.source_hash == b.source_hash && a.origin == b.origin;
}

}  // namespace

bool ranges_overlap(int64_t left_start,
                    int64_t left_end,
                    int64_t right_start,
                    int64_t right_end) {
    return left_end > right_start && left_start < right_end;
}

ReportStoreCoverageRecord *ensure_coverage_scratch() {
    if (coverage_scratch) return coverage_scratch;

    coverage_scratch = static_cast<ReportStoreCoverageRecord *>(
        Memory::calloc_large(COVERAGE_MAX_INTERVALS,
                             sizeof(ReportStoreCoverageRecord)));
    return coverage_scratch;
}

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
            current.coverage_read_errors++;
            continue;
        }
        current.coverage_records_read++;
        coverage_insert(recs, count, record);
    }
    file.close();
    return count;
}

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

}  // namespace ReportStoreInternal
}  // namespace aircannect
