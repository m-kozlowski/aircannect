#pragma once

#include <stddef.h>

namespace aircannect {

struct ReportPendingResultPrepare {
    bool active = false;
    bool refresh_cache = false;
    size_t therapy_index = 0;
};

class ReportPendingResultPrepareState {
public:
    void set(size_t therapy_index, bool refresh_cache);
    bool take(ReportPendingResultPrepare &out);
    void clear();

    bool active() const { return pending_.active; }

private:
    ReportPendingResultPrepare pending_;
};

}  // namespace aircannect
