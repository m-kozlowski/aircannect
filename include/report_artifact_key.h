#pragma once

#include <stddef.h>
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

struct ReportArtifactKey {
    SleepDayId sleep_day;
    SourceRevision source_revision;

    static ReportArtifactKey result(SleepDayId sleep_day,
                                    SourceRevision source_revision);

    bool valid() const {
        return sleep_day.valid() && source_revision.valid();
    }

    friend bool operator==(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs) {
        return lhs.sleep_day == rhs.sleep_day &&
               lhs.source_revision == rhs.source_revision;
    }
    friend bool operator!=(const ReportArtifactKey &lhs,
                           const ReportArtifactKey &rhs) {
        return !(lhs == rhs);
    }
};

}  // namespace aircannect
