#pragma once

#include <memory>

#include "report_executor.h"
#include "report_request_queue.h"
#include "report_signal_store.h"
#include "report_signal_store_service.h"

namespace aircannect {

// Materializes the source-neutral executor stream. EDF and fallback decoding
// remain owned by ReportExecutor; storage owns all file I/O.
class ReportSignalStoreBuilder final : public ReportExecutionSink {
public:
    ReportSignalStoreBuilder();
    ~ReportSignalStoreBuilder() override;

    ReportSignalStoreBuilder(const ReportSignalStoreBuilder &) = delete;
    ReportSignalStoreBuilder &operator=(
        const ReportSignalStoreBuilder &) = delete;

    void begin(ReportSignalStoreService &store);
    bool begin_build(const ReportArtifactRequest &request,
                     const ReportReadPlan &plan,
                     uint32_t store_generation,
                     std::shared_ptr<const LargeByteBuffer> previous = {},
                     std::shared_ptr<const LargeByteBuffer> checkpoint = {});
    bool configure_series(const ReportSeriesDescriptor &series,
                          const EdfSignalScale &scale) override;
    bool ready() override;
    bool end_operation() override;
    bool accept_series(uint16_t session_index,
                       const ReportSeriesDescriptor &series,
                       const ReportSeriesSample &sample) override;
    bool accept_series_span(
        uint16_t session_index,
        const ReportSeriesDescriptor &series,
        const EdfReportSeriesSpan &span) override;
    bool accept_event(uint16_t session_index,
                      const ReportEventRecord &event) override;
    bool finish_build();
    void discard_build();

    std::shared_ptr<ReportSignalStoreBundle> take_completed();
    const char *failure_reason() const override { return failure_reason_; }

private:
    struct Runtime;
    bool accept_raw_sample(uint16_t session_index,
                           const ReportSeriesDescriptor &series,
                           int64_t timestamp_ms,
                           int16_t raw,
                           const EdfSignalScale *scale = nullptr);
    bool accept_raw_run(uint16_t session_index,
                        const ReportSeriesDescriptor &series,
                        const EdfReportSeriesSpan &span,
                        size_t begin,
                        size_t end);
    bool flush_blocks(bool include_partial);
    Runtime *runtime_ = nullptr;
    ReportSignalStoreService *store_ = nullptr;
    const char *failure_reason_ = nullptr;
};

}  // namespace aircannect
