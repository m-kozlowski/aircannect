#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_data_provider.h"
#include "report_materializer.h"
#include "report_result_runtime.h"

namespace aircannect {

class ReportResultMaterializationSink : public ReportMaterializerSink {
public:
    using ReportResultChunk = ReportResultRuntime::ReportResultChunk;

    ReportResultMaterializationSink(ReportResultRuntime &runtime,
                                    const ReportDataProvider &spool_provider);

    const char *error() const { return error_; }

    bool begin_materialization(const ReportIndexedNight &night,
                               const ReportResolvedPlan &plan) override;
    bool add_materialized_stream(const ReportResolvedStream &stream,
                                 size_t &result_stream_index) override;
    bool add_materialized_segment(const ReportResolvedSegment &segment,
                                  size_t result_stream_index) override;
    void finish_materialization(const ReportResolvedPlan &plan) override;

private:
    bool ensure_chunks();
    void set_error(const char *message);

    bool add_stream(ReportStoreChunkKind kind,
                    ReportSourceId source,
                    ReportSignalId signal,
                    const char *name,
                    bool required,
                    bool complete,
                    size_t &stream_index);
    bool add_provider_chunks_to_stream(const ReportDataProvider &provider,
                                       const ReportResolvedSegment &segment,
                                       int64_t night_start_ms,
                                       size_t stream_index);
    bool add_provider_chunk(const ReportProviderChunk &provider_chunk,
                            bool required,
                            size_t stream_index);
    static bool collect_chunk(void *context,
                              const ReportProviderChunk &info);

    ReportResultRuntime &runtime_;
    const ReportDataProvider &spool_provider_;
    const char *error_ = nullptr;
};

}  // namespace aircannect
