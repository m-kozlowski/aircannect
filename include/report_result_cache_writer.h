#pragma once

#include <memory>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_manager_internal_types.h"
#include "report_night_index.h"

namespace aircannect {

class ReportResultCacheWriter {
public:
    using ResultCacheWritePhase =
        report_manager_internal::ResultCacheWritePhase;
    using ResultCacheWriteJob = report_manager_internal::ResultCacheWriteJob;

    ~ReportResultCacheWriter();

    bool begin();
    bool enqueue(const ReportIndexedNight &night,
                 const char *etag,
                 const std::shared_ptr<ReportSpoolBuffer> &result_json,
                 const std::shared_ptr<ReportSpoolBuffer> &plot);
    bool active() const;
    bool service();

private:
    void reset_locked();

    SemaphoreHandle_t lock_ = nullptr;
    ResultCacheWriteJob job_;
};

}  // namespace aircannect
