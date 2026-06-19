#include "report_cache_writer_job.h"

#include "report_manager.h"

namespace aircannect {

JobStep ReportCacheWriterJob::step() {
    if (!report_) return JobStep::Idle;
    return report_->service_cache_writer() ? JobStep::Working : JobStep::Idle;
}

}  // namespace aircannect
