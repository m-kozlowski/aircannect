#include "report_night_query_service.h"

#include "report_index_scratch.h"
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
        night_index_.by_therapy_index(therapy_index, night.get()) !=
            ReportNightIndexLookupResult::Ready) {
        return false;
    }

    night_index_.format_result_etag(night.get(), out, out_size);
    return true;
}

bool ReportNightQueryService::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    if (!callback) return false;

    ReportNightIndexSnapshotRef snapshot;
    if (night_index_.snapshot(snapshot) !=
            ReportNightIndexSnapshotResult::Ready ||
        !snapshot) {
        return false;
    }

    ScopedIndexedNight indexed("summary_night_snapshot");
    if (!indexed) return false;

    bool any = false;
    size_t therapy_index = 0;
    for (size_t i = snapshot->count(); i > 0; --i) {
        const size_t summary_index = i - 1;
        if (!snapshot->materialize(summary_index, indexed.get())) return false;
        if (!report_indexed_night_visible_in_summary(indexed.get())) continue;

        ReportSummaryRecord record = indexed->summary;
        record.duration_min =
            report_indexed_night_display_duration_min(indexed.get());

        ReportSummaryNight night;
        night.summary_index = summary_index;
        night.therapy_index = therapy_index++;
        night.record = record;
        any = true;
        if (!callback(context, night)) break;
    }

    return any;
}

bool ReportNightQueryService::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    ScopedIndexedNight night("summary_night_index");
    if (!night ||
        night_index_.by_therapy_index(therapy_index, night.get()) !=
            ReportNightIndexLookupResult::Ready) {
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
