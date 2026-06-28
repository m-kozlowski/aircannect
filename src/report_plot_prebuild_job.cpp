#include "report_plot_prebuild_job.h"

#include "report_manager.h"

namespace aircannect {

JobStep ReportPlotPrebuildJob::step() {
    if (!report_) return JobStep::Idle;
    switch (report_->request_idle_plot_prebuild()) {
        case ReportManager::PlotPrebuildResult::Queued:
        case ReportManager::PlotPrebuildResult::AlreadyQueued:
        case ReportManager::PlotPrebuildResult::Waiting:
            return JobStep::Waiting;
        case ReportManager::PlotPrebuildResult::Scanned:
            return JobStep::Working;
        case ReportManager::PlotPrebuildResult::Drained:
        case ReportManager::PlotPrebuildResult::Unavailable:
        default:
            return JobStep::Idle;
    }
}

void ReportPlotPrebuildJob::on_preempt() {
    if (report_) report_->preempt_idle_plot_prebuild();
}

}  // namespace aircannect
