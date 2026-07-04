#pragma once

#include <atomic>
#include <stdint.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_summary_types.h"

namespace aircannect {

class ReportSummaryStatusStore {
public:
    ~ReportSummaryStatusStore();

    void begin();
    void release();

    bool take(TickType_t timeout) const;
    void give() const;

    ReportSummaryStatus &status() { return status_; }
    const ReportSummaryStatus &status() const { return status_; }

    void reset_status();
    void publish_revision();
    uint32_t revision() const;

private:
    ReportSummaryStatus status_{};
    mutable SemaphoreHandle_t lock_ = nullptr;
    std::atomic<uint32_t> revision_pub_{0};
};

}  // namespace aircannect
