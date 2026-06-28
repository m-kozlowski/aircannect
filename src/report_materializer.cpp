#include "report_materializer.h"

#include "report_proto.h"
#include "report_night_index.h"
#include "report_source_resolver.h"

namespace aircannect {

bool ReportMaterializer::materialize(const ReportIndexedNight &night,
                                     const ReportResolvedPlan &plan,
                                     ReportMaterializerSink &sink) const {
    if (!sink.begin_materialization(night, plan)) return false;

    size_t result_stream_index[AC_REPORT_RESOLVED_STREAM_MAX];
    for (size_t i = 0; i < AC_REPORT_RESOLVED_STREAM_MAX; ++i) {
        result_stream_index[i] = SIZE_MAX;
    }

    for (size_t i = 0; i < plan.stream_count; ++i) {
        if (!sink.add_materialized_stream(plan.streams[i],
                                          result_stream_index[i])) {
            return false;
        }
    }

    for (size_t i = 0; i < plan.segment_count; ++i) {
        const ReportResolvedSegment &segment = plan.segments[i];
        if (segment.stream_index >= plan.stream_count) return false;
        if (!sink.add_materialized_segment(
                segment,
                result_stream_index[segment.stream_index])) {
            return false;
        }
    }

    sink.finish_materialization(plan);
    return true;
}

}  // namespace aircannect
