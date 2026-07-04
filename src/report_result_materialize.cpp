#include "report_result_build_service.h"

#include "debug_log.h"
#include "edf_report_provider.h"
#include "report_materializer.h"
#include "report_result_materialization_sink.h"
#include "report_source_resolver.h"

namespace aircannect {
namespace {

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

}  // namespace

bool ReportResultBuildService::resolve_and_materialize_for_night(
    const ReportIndexedNight &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *edf_pending_out) {
    if (edf_pending_out) *edf_pending_out = false;
    bool edf_pending = false;
    bool have_edf =
        find_edf_sessions_for_night(night.summary,
                                    range_start_ms,
                                    range_end_ms,
                                    &edf_pending);
    if (edf_pending) {
        if (edf_pending_out) *edf_pending_out = true;
        runtime_.status().state = ReportResultState::Preparing;
        runtime_.status().error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night.summary.start_ms),
                  static_cast<long long>(range_start_ms),
                  static_cast<long long>(range_end_ms));
        return true;
    }
    if (!runtime_.scratch().ensure_resolve_buffers()) {
        fail_prepare("result_plan_alloc_failed");
        return false;
    }

    EdfReportDataProvider edf_provider(have_edf ? runtime_.scratch().edf_sessions()
                                                : nullptr,
                                       have_edf ? runtime_.scratch().edf_session_count()
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  *runtime_.scratch().resolve_scratch());
    ReportResolvedPlan &plan = *runtime_.scratch().resolved_plan();
    if (!resolver.build_plan(night, range_start_ms, range_end_ms, plan)) {
        fail_prepare("source_resolve_failed");
        return false;
    }

    ReportMaterializer materializer;
    ReportResultMaterializationSink sink(runtime_,
                                         spool_report_provider());
    if (!materializer.materialize(night, plan, sink)) {
        fail_prepare(sink.error() ? sink.error()
                                  : "source_materialize_failed");
        return false;
    }
    return true;
}

}  // namespace aircannect
