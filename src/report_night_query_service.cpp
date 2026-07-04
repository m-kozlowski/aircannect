#include "report_night_query_service.h"

#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_index_scratch.h"
#include "report_manager_limits.h"
#include "report_summary_json.h"

namespace aircannect {

ReportNightQueryService::ReportNightQueryService(
    ReportNightIndexService &night_index)
    : night_index_(night_index) {}

bool ReportNightQueryService::night_etag(size_t therapy_index,
                                         char *out,
                                         size_t out_size) const {
    ScopedIndexedNight night("night_etag_index");
    if (!night ||
        !night_index_.by_therapy_index(therapy_index, night.get())) {
        return false;
    }

    night_index_.format_result_etag(night.get(), out, out_size);
    return true;
}

bool ReportNightQueryService::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    if (!callback) return false;

    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "summary_night_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t count = 0;
    if (!night_index_.build(snapshot, AC_REPORT_SUMMARY_RECORD_MAX, count)) {
        Memory::free(snapshot);
        return false;
    }

    bool any = false;
    size_t therapy_index = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t summary_index = i - 1;
        const ReportIndexedNight &indexed = snapshot[summary_index];
        if (!report_indexed_night_visible_in_summary(indexed)) continue;

        ReportSummaryRecord record = indexed.summary;
        record.duration_min =
            report_indexed_night_display_duration_min(indexed);

        ReportSummaryNight night;
        night.summary_index = summary_index;
        night.therapy_index = therapy_index++;
        night.record = record;
        any = true;
        if (!callback(context, night)) break;
    }

    Memory::free(snapshot);
    return any;
}

bool ReportNightQueryService::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    ScopedIndexedNight night("summary_night_index");
    if (!night ||
        !night_index_.by_therapy_index(therapy_index, night.get())) {
        return false;
    }

    out = night->summary;
    out.duration_min = report_indexed_night_display_duration_min(night.get());
    return true;
}

bool ReportNightQueryService::latest_summary_night(
    ReportSummaryRecord &out) const {
    return summary_night_by_therapy_index(0, out);
}

}  // namespace aircannect
