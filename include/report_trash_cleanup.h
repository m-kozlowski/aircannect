#pragma once

#include <stdint.h>

namespace aircannect {

class ReportTrashCleanup {
public:
    void service(bool realtime_active, bool report_busy);

private:
    uint32_t next_cleanup_ms_ = 0;
};

}  // namespace aircannect
