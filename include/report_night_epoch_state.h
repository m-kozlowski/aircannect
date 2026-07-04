#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_manager_internal_types.h"
#include "report_manager_limits.h"

namespace aircannect {

class ReportNightEpochState {
public:
    using NightEpoch = report_manager_internal::NightEpoch;

    ~ReportNightEpochState();

    bool begin();
    void clear();
    void note_chunk_committed(uint64_t night_start_ms);
    void remove_night(uint64_t night_start_ms);

    uint32_t data_epoch() const { return data_epoch_; }
    uint32_t night_epoch(uint64_t night_start_ms) const;

private:
    NightEpoch *nights_ = nullptr;
    size_t count_ = 0;
    uint32_t data_epoch_ = 0;
};

}  // namespace aircannect
