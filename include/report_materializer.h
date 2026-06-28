#pragma once

#include <stddef.h>

namespace aircannect {

struct ReportSummaryRecord;
struct ReportIndexedNight;
struct ReportResolvedPlan;
struct ReportResolvedStream;
struct ReportResolvedSegment;

class ReportMaterializerSink {
public:
    virtual ~ReportMaterializerSink() = default;

    virtual bool begin_materialization(const ReportIndexedNight &night,
                                       const ReportResolvedPlan &plan) = 0;
    virtual bool add_materialized_stream(const ReportResolvedStream &stream,
                                         size_t &result_stream_index) = 0;
    virtual bool add_materialized_segment(const ReportResolvedSegment &segment,
                                          size_t result_stream_index) = 0;
    virtual void finish_materialization(const ReportResolvedPlan &plan) = 0;
};

class ReportMaterializer {
public:
    bool materialize(const ReportIndexedNight &night,
                     const ReportResolvedPlan &plan,
                     ReportMaterializerSink &sink) const;
};

}  // namespace aircannect
