#pragma once

#include <stdint.h>

#include "sleep_day_id.h"

namespace aircannect {

class SourceRevision {
public:
    constexpr SourceRevision() = default;
    explicit constexpr SourceRevision(uint64_t value) : value_(value) {}

    constexpr bool valid() const { return value_ != 0; }
    constexpr uint64_t value() const { return value_; }

    friend constexpr bool operator==(SourceRevision lhs,
                                     SourceRevision rhs) {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr bool operator!=(SourceRevision lhs,
                                     SourceRevision rhs) {
        return !(lhs == rhs);
    }

private:
    uint64_t value_ = 0;
};

enum class ReportArtifactKind : uint8_t {
    Result,
    Overview,
    RangeTile,
};

struct ReportArtifactKey {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    ReportArtifactKind kind = ReportArtifactKind::Result;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;

    static ReportArtifactKey result(SleepDayId sleep_day,
                                    SourceRevision source_revision);
    static ReportArtifactKey overview(SleepDayId sleep_day,
                                      SourceRevision source_revision);
    static ReportArtifactKey range_tile(SleepDayId sleep_day,
                                        SourceRevision source_revision,
                                        int64_t range_start_ms,
                                        int64_t range_end_ms);

    bool valid() const;

    friend bool operator==(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs) {
        return lhs.sleep_day == rhs.sleep_day &&
               lhs.source_revision == rhs.source_revision &&
               lhs.kind == rhs.kind &&
               lhs.range_start_ms == rhs.range_start_ms &&
               lhs.range_end_ms == rhs.range_end_ms;
    }
    friend bool operator!=(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs) {
        return !(lhs == rhs);
    }
};

}  // namespace aircannect
