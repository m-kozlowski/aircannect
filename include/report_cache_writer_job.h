#pragma once

#include "background_worker.h"

namespace aircannect {

class ReportManager;

class ReportCacheWriterJob : public BackgroundJob {
public:
    explicit ReportCacheWriterJob(ReportManager &report) : report_(&report) {}
    const char *name() const override { return "report_cache_writer"; }
    JobStep step() override;
    bool run_when_foreground_busy() const override { return true; }
    bool drain_before_regular_jobs() const override { return true; }

private:
    ReportManager *report_ = nullptr;
};

}  // namespace aircannect
