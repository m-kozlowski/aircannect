#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_artifacts.h"
#include "report_executor.h"

namespace aircannect {

struct ReportMetricStatistics {
    bool valid = false;
    int32_t mean_milli = 0;
    int32_t p50_milli = 0;
    int32_t p95_milli = 0;
};

struct ReportPlotAccumulatorSummary {
    ReportArtifactEventCounts events;
    uint32_t prefix_crc32 = 0;
    ReportMetricStatistics pressure;
    ReportMetricStatistics leak;
    ReportMetricStatistics minute_ventilation;
    ReportMetricStatistics respiratory_rate;
    ReportMetricStatistics tidal_volume;
    ReportMetricStatistics spo2;
    uint64_t csr_duration_ms = 0;
};

// Collects one plot window from the canonical report execution stream. The
// caller chooses only the output window and bucket budget; source selection
// and decoding remain entirely in ReportPlanner and ReportExecutor.
class ReportPlotAccumulator final : public ReportExecutionSink {
public:
    ReportPlotAccumulator();
    ~ReportPlotAccumulator() override;

    ReportPlotAccumulator(const ReportPlotAccumulator &) = delete;
    ReportPlotAccumulator &operator=(const ReportPlotAccumulator &) = delete;

    bool begin(const ReportReadPlan &plan,
               int64_t start_ms,
               int64_t end_ms,
               size_t bucket_budget,
               bool collect_metrics);
    bool accept_series(uint16_t session_index,
                       const ReportSeriesDescriptor &series,
                       const ReportSeriesSample &sample) override;
    bool accept_event(uint16_t session_index,
                      const ReportEventRecord &event) override;
    std::shared_ptr<const LargeByteBuffer> finish(
        ReportPlotAccumulatorSummary &summary);
    void clear();
    const char *failure_reason() const { return failure_reason_; }

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
    const char *failure_reason_ = nullptr;
};

}  // namespace aircannect
