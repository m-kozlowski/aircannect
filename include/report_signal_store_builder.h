#pragma once

#include <memory>

#include "report_executor.h"
#include "report_request_queue.h"
#include "report_signal_store.h"

namespace aircannect {

// Materializes the source-neutral executor stream. EDF and fallback decoding
// remain owned by ReportExecutor; this builder only sees canonical samples.
class ReportSignalStoreBuilder final : public ReportExecutionSink {
public:
    ReportSignalStoreBuilder();
    ~ReportSignalStoreBuilder() override;

    ReportSignalStoreBuilder(const ReportSignalStoreBuilder &) = delete;
    ReportSignalStoreBuilder &operator=(
        const ReportSignalStoreBuilder &) = delete;

    bool begin_build(const ReportArtifactRequest &request,
                     const ReportReadPlan &plan,
                     uint32_t store_generation);
    bool accept_series(uint16_t session_index,
                       const ReportSeriesDescriptor &series,
                       const ReportSeriesSample &sample) override;
    bool accept_event(uint16_t session_index,
                      const ReportEventRecord &event) override;
    bool finish_build();
    void discard_build();

    std::shared_ptr<ReportSignalStoreBundle> take_completed();
    const char *failure_reason() const { return failure_reason_; }

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
    const char *failure_reason_ = nullptr;
};

}  // namespace aircannect
