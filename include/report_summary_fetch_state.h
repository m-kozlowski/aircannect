#pragma once

#include <stdint.h>

namespace aircannect {

class ReportSummaryFetchState {
public:
    void start(uint32_t now_ms);
    void finish();

    bool active() const { return active_; }
    uint32_t elapsed_ms(uint32_t now_ms) const;

private:
    bool active_ = false;
    uint32_t started_ms_ = 0;
};

}  // namespace aircannect
