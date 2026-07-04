#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "report_manager_internal_types.h"
#include "report_manager_limits.h"
#include "report_store.h"

namespace aircannect {

class ReportCacheWriteQueue {
public:
    using CacheCoalesceBuffer = report_manager_internal::CacheCoalesceBuffer;
    using CacheWriteEnqueueResult =
        report_manager_internal::CacheWriteEnqueueResult;
    using CacheWriteQueueSlot = report_manager_internal::CacheWriteQueueSlot;

    ~ReportCacheWriteQueue();

    bool begin();
    bool ensure_slots();

    CacheWriteEnqueueResult enqueue(CacheCoalesceBuffer &buf,
                                    const ReportStoreChunkKey &key,
                                    const ReportStoreChunkMeta &meta);
    void begin_fetch();
    void abort_fetch();
    bool pending_for_active_fetch() const;
    bool failed_for_active_fetch(std::string &error) const;
    bool backpressure_active() const;

    bool take_next(CacheWriteQueueSlot &job);
    bool complete(const CacheWriteQueueSlot &job, bool ok);

private:
    void reset_fetch_state_locked();

    CacheWriteQueueSlot *queue_ = nullptr;
    SemaphoreHandle_t lock_ = nullptr;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    uint32_t fetch_id_ = 0;
    uint32_t pending_ = 0;
    uint32_t failed_fetch_id_ = 0;
    std::string error_;
};

}  // namespace aircannect
