#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_limits.h"
#include "report_proto.h"
#include "report_result_types.h"
#include "report_sources.h"
#include "spool_client.h"

namespace aircannect {

struct ReportCacheSourcePlan {
    ReportSourceId source = ReportSourceId::Summary;
    int64_t from_ms = 0;
};

class ReportCacheFetchState {
public:
    void reset_for_night(const ReportSummaryRecord &night);

    bool active() const { return active_; }
    bool finalizing_source() const { return source_finalizing_; }

    const ReportSummaryRecord &night() const { return night_; }
    const ReportCacheFetchStatus &status() const { return status_; }

    bool add_source(ReportSourceId source, int64_t from_ms);
    bool has_sources() const { return source_count_ > 0; }
    size_t source_count() const { return source_count_; }
    size_t source_index() const { return source_index_; }

    const ReportCacheSourcePlan *current_source() const;
    const ReportCacheSourcePlan &finalizing_plan() const {
        return finalizing_plan_;
    }

    void mark_no_work();
    void activate();
    void update_active_source(ReportSourceId source,
                              const SpoolClientStatus &spool);
    void update_spool(const SpoolClientStatus &spool);
    void note_chunk_written();
    void set_error(const char *message);
    void begin_finalizing_current_source();
    void complete_finalizing_source();

    void finish(const SpoolClientStatus &spool);
    void fail(const char *message, const SpoolClientStatus &spool);
    void cancel(const char *message, const SpoolClientStatus &spool);

private:
    bool active_ = false;
    bool source_finalizing_ = false;
    ReportSummaryRecord night_;
    ReportCacheSourcePlan plans_[AC_REPORT_CACHE_SOURCE_MAX] = {};
    size_t source_count_ = 0;
    size_t source_index_ = 0;
    ReportCacheSourcePlan finalizing_plan_;
    ReportCacheFetchStatus status_;
};

}  // namespace aircannect
