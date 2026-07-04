#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_proto.h"

namespace aircannect {

class ReportSummaryScratch {
public:
    ~ReportSummaryScratch();

    void begin();
    bool take(TickType_t timeout, ReportSummaryRecord *&out);
    void give();

private:
    ReportSummaryRecord *records_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
};

}  // namespace aircannect
