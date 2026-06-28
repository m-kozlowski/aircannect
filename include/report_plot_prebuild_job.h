#pragma once

#include <stdint.h>

#include "background_worker.h"

namespace aircannect {

class ReportManager;

class ReportPlotPrebuildJob : public BackgroundJob {
public:
    explicit ReportPlotPrebuildJob(ReportManager &report) : report_(&report) {}
    const char *name() const override { return "report_plot_prebuild"; }
    JobStep step() override;
    void on_preempt() override;

private:
    ReportManager *report_ = nullptr;
};

}  // namespace aircannect
