#include "report_cache_coverage_marks.h"

#include <string.h>

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_night_index.h"

namespace aircannect {

ReportCacheCoverageMarks::~ReportCacheCoverageMarks() {
    Memory::free(extent_ms_);
}

bool ReportCacheCoverageMarks::begin() {
    if (extent_ms_) return true;

    extent_ms_ = static_cast<int64_t *>(Memory::calloc_large(
        AC_REPORT_SUMMARY_RECORD_MAX,
        sizeof(int64_t),
        false));
    if (!extent_ms_) {
        log_report_alloc_failed(
            "cache_source_night_extents",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
        return false;
    }

    return true;
}

bool ReportCacheCoverageMarks::reset() {
    if (!begin()) return false;

    memset(extent_ms_, 0, AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
    return true;
}

void ReportCacheCoverageMarks::note_chunk(
    const ReportSummaryRecord *records,
    size_t record_count,
    const ReportParsedChunk &chunk) {
    if (chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) return;
    if (!extent_ms_ || !records) return;

    const size_t count = record_count < AC_REPORT_SUMMARY_RECORD_MAX
                             ? record_count
                             : AC_REPORT_SUMMARY_RECORD_MAX;
    for (size_t record_index = 0; record_index < count; ++record_index) {
        const ReportSummaryRecord &record = records[record_index];
        if (!record.valid || !record.duration_min) continue;

        if (!ranges_overlap(chunk.start_ms,
                            chunk.end_ms,
                            static_cast<int64_t>(record.start_ms),
                            static_cast<int64_t>(record.end_ms))) {
            continue;
        }

        if (chunk.end_ms > extent_ms_[record_index]) {
            extent_ms_[record_index] = chunk.end_ms;
        }
    }
}

int64_t ReportCacheCoverageMarks::extent(size_t record_index) const {
    if (!extent_ms_ || record_index >= AC_REPORT_SUMMARY_RECORD_MAX) return 0;

    return extent_ms_[record_index];
}

}  // namespace aircannect
