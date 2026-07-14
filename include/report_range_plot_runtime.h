#pragma once

#include <stdint.h>

#include "report_range_plot_state.h"

namespace aircannect {

class ReportRangePlotRuntime {
public:
    ReportRangePlotBuildState &state() { return state_; }
    const ReportRangePlotBuildState &state() const { return state_; }

    bool active() const { return state_.active; }

    bool matches(size_t index,
                 uint64_t night_start_ms,
                 const char *etag,
                 int64_t from_ms,
                 int64_t to_ms) const {
        return state_.matches(index, night_start_ms, etag, from_ms, to_ms);
    }

    bool ensure_buffers() { return state_.ensure_buffers(); }
    void reset() { state_.reset(); }

private:
    ReportRangePlotBuildState state_;
};

}  // namespace aircannect
