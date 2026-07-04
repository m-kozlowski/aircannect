#include "report_range_plot_builder.h"

#include <algorithm>
#include <string.h>

#include <Arduino.h>

#include "debug_log.h"
#include "edf_report_provider.h"
#include "report_edf_catalog_context.h"
#include "report_data_provider.h"
#include "report_night_index_service.h"
#include "report_plot_payload.h"
#include "report_range_plot_runtime.h"
#include "report_resolve_context.h"
#include "report_result_cache_runtime.h"
#include "report_source_resolver.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

}  // namespace

ReportRangePlotBuilder::ReportRangePlotBuilder(
    ReportRangePlotRuntime &range_plot,
    ReportResultCacheRuntime &cache,
    ReportNightIndexService &night_index,
    ReportEdfCatalogContext &edf_catalog)
    : range_plot_(range_plot),
      cache_(cache),
      night_index_(night_index),
      edf_catalog_(edf_catalog) {}

bool ReportRangePlotBuilder::active() const {
    return range_plot_.active();
}

bool ReportRangePlotBuilder::matches(size_t index,
                                     uint64_t night_start_ms,
                                     int64_t from_ms,
                                     int64_t to_ms) const {
    return range_plot_.matches(index, night_start_ms, from_ms, to_ms);
}

void ReportRangePlotBuilder::reset(bool clear_ready) {
    range_plot_.reset();

    if (clear_ready) cache_.reset_range(true);
}

bool ReportRangePlotBuilder::start(uint64_t night_start_ms,
                                   size_t therapy_index_hint,
                                   int64_t from_ms,
                                   int64_t to_ms,
                                   bool &waiting_for_result) {
    waiting_for_result = false;
    reset(false);
    if (to_ms <= from_ms) return false;
    if (!range_plot_.ensure_buffers()) return false;

    auto &range_plot = range_plot_.state();
    size_t therapy_index = therapy_index_hint;

    memset(range_plot.indexed_night, 0, sizeof(*range_plot.indexed_night));
    if (!night_index_.by_start(night_start_ms,
                               *range_plot.indexed_night,
                               &therapy_index)) {
        return false;
    }
    ReportIndexedNight &indexed_night = *range_plot.indexed_night;

    range_plot.edf_session_count = 0;
    bool edf_pending = false;
    memset(range_plot.edf_sessions,
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    bool have_edf =
        edf_catalog_.collect_sessions_for_night(indexed_night.summary,
                                                from_ms,
                                                to_ms,
                                                range_plot.edf_sessions,
                                                AC_REPORT_EDF_SESSION_MAX,
                                                range_plot.edf_session_count,
                                                &edf_pending);
    if (edf_pending) {
        range_plot.edf_session_count = 0;
        waiting_for_result = true;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Range plot waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<long long>(from_ms),
                  static_cast<long long>(to_ms));
        return false;
    }

    ScopedReportResolveContext resolve("range_resolver", false);
    if (!resolve) return false;

    EdfReportDataProvider edf_provider(have_edf ? range_plot.edf_sessions
                                                : nullptr,
                                       have_edf ? range_plot.edf_session_count
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    ReportResolvedPlan &plan = resolve.plan();
    if (!resolver.build_plan(indexed_night, from_ms, to_ms, plan)) {
        return false;
    }
    if (!materialize_plan(indexed_night, plan)) {
        return false;
    }

    range_plot.index = therapy_index;
    range_plot.from_ms = from_ms;
    range_plot.to_ms = to_ms;
    range_plot.bucket_ms = std::max<int64_t>(
        1,
        (to_ms - from_ms) /
            static_cast<int64_t>(AC_REPORT_RANGE_PLOT_BUCKETS));
    range_plot.bytes = std::make_shared<ReportSpoolBuffer>();
    if (!range_plot.bytes) {
        fail("range_alloc_failed");
        return false;
    }

    ReportSpoolBuffer &out = *range_plot.bytes;
    out.set_max_size(AC_REPORT_RANGE_PLOT_MAX_BYTES);
    range_plot.tmp.set_max_size(768 * 1024);
    range_plot.seen_events.set_max_size(16 * 1024);

    range_plot.ok =
        out.reserve_capacity(64 * 1024) &&
        range_plot.seen_events.reserve_capacity(2 * 1024) &&
        bin_put_u32(out, PLOT_BIN_MAGIC) &&
        bin_put_u16(out, PLOT_BIN_VERSION) &&
        bin_put_u16(out, 0) &&
        bin_put_i64(out, from_ms);
    if (!range_plot.ok) {
        fail("range_alloc_failed");
        return false;
    }

    range_plot.active = true;
    range_plot.started_ms = millis();
    range_plot.input_chunks = 0;
    range_plot.input_bytes = 0;
    range_plot.phase = ReportPlotBuildPhase::Events;
    return true;
}

}  // namespace aircannect
