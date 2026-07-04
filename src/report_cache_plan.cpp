#include "report_cache_fetch_service.h"

#include <algorithm>

#include "edf_report_provider.h"
#include "report_data_provider.h"
#include "report_cache_plan.h"
#include "report_manager_helpers.h"
#include "report_night_index.h"
#include "report_resolve_context.h"
#include "report_source_resolver.h"
#include "report_sources.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

}  // namespace

bool report_cache_source_supported(ReportSourceId source) {
    switch (source) {
        case ReportSourceId::RespiratoryEvents:
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            return true;
        default:
            return false;
    }
}

bool ReportCacheFetchService::build_plan(
    const ReportIndexedNight &indexed_night,
    bool force,
    bool latest_tail_refresh) {
    const ReportSummaryRecord &night = indexed_night.summary;
    state().reset_for_night(night);

    auto add_plan_source = [&](ReportSourceId source,
                               int64_t from_ms) -> bool {
        if (!report_cache_source_supported(source)) return true;

        if (state().add_source(source, from_ms)) {
            return true;
        }

        (void)fail("cache_plan_full");
        return false;
    };

    int64_t latest_tail_start_ms = static_cast<int64_t>(night.start_ms);
    int64_t latest_tail_end_ms = static_cast<int64_t>(night.end_ms);
    if (latest_tail_refresh) {
        bool found_latest_range = false;
        auto maybe_use_latest_range = [&](const ReportSessionRange &range) {
            if (range.end_ms <= range.start_ms) return;
            if (!found_latest_range ||
                range.start_ms > latest_tail_start_ms) {
                latest_tail_start_ms = range.start_ms;
                latest_tail_end_ms = range.end_ms;
                found_latest_range = true;
            }
        };

        ReportSessionRange data_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
        const size_t data_count =
            collect_indexed_night_data_ranges(indexed_night,
                                              data_ranges,
                                              AC_REPORT_SUMMARY_SESSION_MAX);
        if (indexed_night.has_edf && data_count > 0) {
            for (size_t i = 0; i < data_count; ++i) {
                maybe_use_latest_range(data_ranges[i]);
            }

            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                const ReportSessionRange &display = indexed_night.ranges[i];
                bool overlaps_edf = false;
                for (size_t k = 0; k < data_count; ++k) {
                    if (ranges_overlap(display.start_ms,
                                       display.end_ms,
                                       data_ranges[k].start_ms,
                                       data_ranges[k].end_ms)) {
                        overlaps_edf = true;
                        break;
                    }
                }
                if (!overlaps_edf) maybe_use_latest_range(display);
            }
        } else {
            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                maybe_use_latest_range(indexed_night.ranges[i]);
            }
        }
    }

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);

    if (force) {
        for (size_t i = 0; i < source_count; ++i) {
            const ReportSourceDef &source = sources[i];
            if (source.id == ReportSourceId::Summary) continue;
            if (!add_plan_source(source.id,
                                 static_cast<int64_t>(night.start_ms))) {
                return false;
            }
        }
    } else {
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (latest_tail_refresh) {
            span_start_ms = latest_tail_start_ms;
            span_end_ms = latest_tail_end_ms;
        } else if (!indexed_night_data_span(indexed_night,
                                            span_start_ms,
                                            span_end_ms)) {
            state().mark_no_work();
            return true;
        }

        ScopedReportResolveContext resolve("cache_plan_resolver");
        if (!resolve) {
            (void)fail("cache_plan_alloc_failed");
            return false;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        const bool have_edf =
            edf_catalog_.collect_sessions_for_night(night,
                                                    span_start_ms,
                                                    span_end_ms,
                                                    resolve.sessions(),
                                                    AC_REPORT_EDF_SESSION_MAX,
                                                    session_count,
                                                    &edf_pending);
        if (edf_pending) {
            state().mark_no_work();
            return true;
        }

        EdfReportDataProvider edf_provider(have_edf ? resolve.sessions()
                                                    : nullptr,
                                           have_edf ? session_count : 0);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());

        ReportResolvedPlan &resolved = resolve.plan();
        if (!resolver.build_plan(indexed_night,
                                 span_start_ms,
                                 span_end_ms,
                                 resolved)) {
            (void)fail("cache_source_resolve_failed");
            return false;
        }

        for (size_t i = 0; i < resolved.segment_count; ++i) {
            const ReportResolvedSegment &segment = resolved.segments[i];
            if (segment.provider != ReportResolvedProvider::Spool ||
                segment.complete ||
                !segment.required ||
                segment.end_ms <= segment.start_ms) {
                continue;
            }

            const ReportSourceDef *source = report_source_def(segment.source);
            if (!source || !report_cache_source_supported(source->id)) continue;

            int64_t from_ms = segment.start_ms;
            if (latest_tail_refresh) {
                int64_t cached_end_ms = 0;
                if (report_manager_internal::source_latest_cached_end_for_night(
                        spool_report_provider(),
                        *source,
                        night,
                        cached_end_ms) &&
                    cached_end_ms > latest_tail_start_ms) {
                    from_ms =
                        cached_end_ms - AC_REPORT_LATEST_TAIL_OVERLAP_MS;
                    if (from_ms < latest_tail_start_ms) {
                        from_ms = latest_tail_start_ms;
                    }
                    if (from_ms > segment.start_ms) {
                        from_ms = segment.start_ms;
                    }
                }
            }

            if (!add_plan_source(source->id, from_ms)) return false;
        }
    }

    if (!has_sources()) {
        state().mark_no_work();
        return true;
    }

    return activate_plan();
}

}  // namespace aircannect
