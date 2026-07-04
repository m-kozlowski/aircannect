#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_night_index_cache.h"

namespace aircannect {

class ReportPlotPrebuildState {
public:
    bool key_matches(const ReportNightIndexCacheKey &key) const;
    void reset_for_key(const ReportNightIndexCacheKey &key);

    bool rescan_delay_active(uint32_t now_ms) const;
    void mark_drained(uint32_t now_ms, uint32_t delay_ms);

    size_t cursor() const { return cursor_; }
    void advance_cursor() { cursor_++; }
    void rewind_cursor();

private:
    bool key_valid_ = false;
    ReportNightIndexCacheKey key_;
    size_t cursor_ = 0;
    uint32_t next_scan_ms_ = 0;
};

}  // namespace aircannect
