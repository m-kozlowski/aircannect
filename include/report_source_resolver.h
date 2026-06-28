#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_plan.h"
#include "report_data_provider.h"
#include "report_night_index.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

static constexpr size_t AC_REPORT_RESOLVED_STREAM_MAX = 32;
static constexpr size_t AC_REPORT_RESOLVED_SEGMENT_MAX =
    AC_REPORT_SUMMARY_SESSION_MAX * 12;

struct ReportCoverageInterval {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    ReportSourceId source = ReportSourceId::Summary;
};

struct ReportResolveScratch {
    ReportCoverageInterval coverage[AC_REPORT_RESOLVED_SEGMENT_MAX] = {};
    ReportCoverageInterval fallback[AC_REPORT_RESOLVED_SEGMENT_MAX] = {};
    ReportSessionRange source_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    ReportSessionRange required_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    EdfReportRequiredRange edf_required_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
};

enum class ReportResolvedProvider : uint8_t {
    None,
    Edf,
    Spool,
};

struct ReportResolvedStream {
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    ReportSourceId preferred_source = ReportSourceId::Summary;
    ReportSourceId selected_source = ReportSourceId::Summary;
    ReportResolvedProvider provider = ReportResolvedProvider::None;
    bool required = false;
    bool complete = false;
    bool low_res = false;
    bool has_covered_segment = false;
    bool has_missing_segment = false;
    bool has_edf_segment = false;
    bool has_spool_segment = false;
};

struct ReportResolvedSegment {
    size_t stream_index = 0;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    ReportSourceId source = ReportSourceId::Summary;
    ReportResolvedProvider provider = ReportResolvedProvider::None;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    bool required = false;
    bool complete = false;
};

struct ReportResolvedPlan {
    ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t range_count = 0;

    ReportResolvedStream streams[AC_REPORT_RESOLVED_STREAM_MAX] = {};
    size_t stream_count = 0;

    ReportResolvedSegment segments[AC_REPORT_RESOLVED_SEGMENT_MAX] = {};
    size_t segment_count = 0;

    bool events_available = false;
    uint32_t missing_required = 0;
};

class ReportSourceResolver {
public:
    ReportSourceResolver(const ReportDataProvider &edf_provider,
                         const ReportDataProvider &spool_provider,
                         ReportResolveScratch &scratch);

    bool build_plan(const ReportIndexedNight &night,
                    int64_t range_start_ms,
                    int64_t range_end_ms,
                    ReportResolvedPlan &out) const;

private:
    const ReportDataProvider &edf_;
    const ReportDataProvider &spool_;
    ReportResolveScratch &scratch_;

    bool add_stream(ReportResolvedPlan &plan,
                    ReportStoreChunkKind kind,
                    ReportSignalId signal,
                    const char *name,
                    ReportSourceId preferred_source,
                    ReportSourceId selected_source,
                    ReportResolvedProvider provider,
                    bool required,
                    bool complete,
                    size_t &stream_index) const;

    bool add_segment(ReportResolvedPlan &plan,
                     size_t stream_index,
                     ReportStoreChunkKind kind,
                     ReportSignalId signal,
                     const char *name,
                     ReportSourceId source,
                     ReportResolvedProvider provider,
                     int64_t start_ms,
                     int64_t end_ms,
                     bool required,
                     bool complete) const;

    ReportSourceId choose_spool_source_for_signal(
        const ReportIndexedNight &night,
        const ReportSignalDef &signal) const;

    bool source_chunk_extent(const ReportIndexedNight &night,
                             ReportSourceId source,
                             const char *name,
                             int64_t &min_start,
                             int64_t &max_end) const;

    bool add_events(const ReportSessionRange *ranges,
                    size_t range_count,
                    ReportResolvedPlan &plan) const;

    bool add_signal(const ReportIndexedNight &night,
                    const ReportSignalDef &signal,
                    const ReportSessionRange *ranges,
                    size_t range_count,
                    ReportResolvedPlan &plan) const;
};

}  // namespace aircannect
