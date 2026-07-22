#pragma once

#include <memory>

#include "report_artifacts.h"
#include "report_engine.h"

namespace aircannect {

class ReportNightArtifactBuilder final : public ReportArtifactAssembler {
public:
    ReportNightArtifactBuilder();
    ~ReportNightArtifactBuilder() override;

    ReportNightArtifactBuilder(const ReportNightArtifactBuilder &) = delete;
    ReportNightArtifactBuilder &operator=(
        const ReportNightArtifactBuilder &) = delete;

    bool begin_build(const ReportArtifactRequest &request,
                     const ReportReadPlan &plan) override;
    bool accept_series(uint16_t session_index,
                       const EdfReportSignalLayout &layout,
                       const ReportSeriesSample &sample) override;
    bool accept_event(uint16_t session_index,
                      const ReportEventRecord &event) override;
    bool finish_build() override;
    void discard_build() override;

    std::shared_ptr<const ReportArtifactBundle> completed() const;
    std::shared_ptr<const ReportArtifactBundle> take_completed();

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
};

}  // namespace aircannect
