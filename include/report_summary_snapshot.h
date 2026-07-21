#pragma once

#include <stdint.h>

#include "large_text_buffer.h"

namespace aircannect {

class ReportSummarySnapshot {
public:
    void request_publish();
    void publish(LargeTextBuffer &build_buffer);
    void publish_fallback(LargeTextBuffer &build_buffer);
    void build_json(LargeTextBuffer &json) const;
    bool available() const { return snapshot_.length() > 0; }
    bool publish_pending() const {
        return requested_generation_ != published_generation_;
    }
    uint32_t requested_generation() const { return requested_generation_; }
    void begin_progress(uint32_t now_ms, uint32_t interval_ms);
    bool progress_due(uint32_t now_ms, uint32_t interval_ms);

private:
    LargeTextBuffer snapshot_;
    uint32_t requested_generation_ = 0;
    uint32_t published_generation_ = 0;
    uint32_t next_progress_ms_ = 0;
};

}  // namespace aircannect
