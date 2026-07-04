#pragma once

#include <stddef.h>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_night_index.h"

namespace aircannect {

class ReportDurableNightIndex {
public:
    ~ReportDurableNightIndex();

    bool begin();
    bool load();
    bool seed(ReportNightIndex &index) const;
    void schedule_save(const ReportIndexedNight *src,
                       size_t count,
                       uint32_t content_crc) const;
    bool service_writer();

private:
    bool ensure_index_buffer_locked() const;
    bool ensure_save_buffer() const;
    void retry_later_locked(uint32_t delay_ms) const;

    mutable SemaphoreHandle_t lock_ = nullptr;
    mutable ReportIndexedNight *index_ = nullptr;
    mutable size_t index_count_ = 0;
    mutable bool index_valid_ = false;
    mutable uint32_t index_crc_ = 0;

    mutable ReportIndexedNight *save_ = nullptr;
    mutable size_t save_count_ = 0;
    mutable uint32_t save_crc_ = 0;
    mutable bool save_pending_ = false;
    mutable uint32_t save_requested_ms_ = 0;
};

}  // namespace aircannect
