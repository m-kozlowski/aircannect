#pragma once

#include <stdint.h>

#include "large_text_buffer.h"

namespace aircannect {

class ReportSummarySnapshot {
public:
    void publish(LargeTextBuffer &build_buffer);
    void build_json(LargeTextBuffer &json) const;
    bool available() const { return snapshot_.length() > 0; }
    bool progress_due(uint32_t now_ms, uint32_t interval_ms);

private:
    LargeTextBuffer snapshot_;
    uint32_t next_progress_ms_ = 0;
};

}  // namespace aircannect
