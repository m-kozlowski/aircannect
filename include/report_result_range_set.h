#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_night_index.h"
#include "report_source_resolver.h"

namespace aircannect {

class ReportResultRangeSet {
public:
    ReportResultRangeSet() = default;
    ~ReportResultRangeSet();
    ReportResultRangeSet(const ReportResultRangeSet &) = delete;
    ReportResultRangeSet &operator=(const ReportResultRangeSet &) = delete;

    void clear();
    bool set_from_indexed_night(const ReportIndexedNight &night);
    bool set_from_resolved_plan(const ReportResolvedPlan &plan);

    bool data_span(const ReportIndexedNight &night,
                   int64_t &span_start_ms,
                   int64_t &span_end_ms) const;
    bool contains_timestamp(int64_t timestamp_ms) const;

    const ReportSessionRange *data() const { return ranges_; }
    size_t count() const { return count_; }

    const ReportSessionRange &operator[](size_t index) const {
        return ranges_[index];
    }

private:
    bool ensure_ranges();
    bool append(int64_t start_ms, int64_t end_ms);
    void sort();

    ReportSessionRange *ranges_ = nullptr;
    size_t count_ = 0;
};

}  // namespace aircannect
