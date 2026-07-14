#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_night_index_snapshot.h"

namespace aircannect {

class ReportDurableNightIndex {
public:
    ~ReportDurableNightIndex();

    bool begin();
    bool load();
    bool snapshot(ReportNightIndexSnapshotRef &out) const;
    void schedule_save(const ReportNightIndexSnapshotRef &snapshot) const;
    bool service_writer();

private:
    void retry_later_locked(uint32_t delay_ms) const;

    mutable SemaphoreHandle_t lock_ = nullptr;
    mutable ReportNightIndexSnapshotRef index_;
    mutable bool index_valid_ = false;

    mutable ReportNightIndexSnapshotRef save_;
    mutable bool save_pending_ = false;
    mutable uint32_t save_requested_ms_ = 0;
};

}  // namespace aircannect
