#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>

#include "large_text_buffer.h"
#include "report_cache_coverage_marks.h"
#include "report_parser.h"
#include "report_summary_record_store.h"
#include "report_summary_scratch.h"
#include "report_summary_snapshot.h"
#include "report_summary_status_store.h"
#include "report_summary_types.h"

namespace aircannect {

class ReportSummaryRuntime {
public:
    void begin();

    // Status and lock
    bool take(TickType_t timeout) const;
    void give() const;

    ReportSummaryStatus &status();
    const ReportSummaryStatus &status() const;
    void reset_status();
    void publish_revision();
    uint32_t revision() const;

    // Records
    bool ensure_records();
    void clear_records();
    ReportSummaryRecord *records();
    const ReportSummaryRecord *records() const;
    size_t record_count() const;
    uint32_t nights_with_therapy() const;
    int64_t night_start_for_timestamp(int64_t timestamp_ms) const;
    void replace_records_from(const ReportSummaryRecord *records,
                              size_t count,
                              uint32_t nights_with_therapy);
    void finalize_records();

    // Scratch buffer
    bool take_scratch(TickType_t timeout, ReportSummaryRecord *&out);
    void give_scratch();

    // Published JSON snapshot
    void publish_snapshot(LargeTextBuffer &build_buffer);
    void build_snapshot_json(LargeTextBuffer &json) const;
    bool snapshot_progress_due(uint32_t now_ms, uint32_t interval_ms);

    // Cache coverage sidecar
    bool reset_coverage_marks();
    void note_coverage_chunk(const ReportParsedChunk &chunk);
    int64_t coverage_extent(size_t record_index) const;

private:
    void sort_records_by_start();
    void apply_record_counts_to_status();

    ReportSummaryRecordStore records_;
    ReportSummaryScratch scratch_;
    ReportSummaryStatusStore status_;
    ReportSummarySnapshot snapshot_;
    ReportCacheCoverageMarks coverage_marks_;
};

}  // namespace aircannect
