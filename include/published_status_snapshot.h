#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <new>
#include <stdint.h>

#include "memory_manager.h"

namespace aircannect {

template <typename Status>
class PublishedStatusSnapshot {
public:
    ~PublishedStatusSnapshot() {
        if (snapshot_) {
            snapshot_->~Status();
            Memory::free(snapshot_);
        }
        if (lock_) vSemaphoreDelete(lock_);
    }

    PublishedStatusSnapshot() = default;
    PublishedStatusSnapshot(const PublishedStatusSnapshot &) = delete;
    PublishedStatusSnapshot &operator=(const PublishedStatusSnapshot &) = delete;

    bool begin(const Status &initial) {
        if (lock_ && snapshot_) return true;

        if (!lock_) lock_ = xSemaphoreCreateMutex();
        if (!lock_) return false;

        if (!snapshot_) {
            void *storage = Memory::alloc_large(sizeof(Status), false);
            if (!storage) return false;
            snapshot_ = new (storage) Status(initial);
        }

        return true;
    }

    bool publish(const Status &status) const {
        if (!lock_ || !snapshot_) return false;
        if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return false;

        *snapshot_ = status;
        xSemaphoreGive(lock_);
        return true;
    }

    bool read(Status &out, uint32_t timeout_ms = 20) const {
        if (!lock_ || !snapshot_ ||
            xSemaphoreTake(lock_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
            return false;
        }

        out = *snapshot_;
        xSemaphoreGive(lock_);
        return true;
    }

private:
    SemaphoreHandle_t lock_ = nullptr;
    Status *snapshot_ = nullptr;
};

}  // namespace aircannect
