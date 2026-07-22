#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <utility>

#include "fixed_queue.h"

namespace aircannect {

// Bounded handoff for commands produced by another task and consumed by the
// main loop. The contained FixedQueue destroys each popped owning value.
template <typename T, size_t Capacity>
class MainLoopInbox {
public:
    bool begin() {
        if (!mutex_) {
            mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
        }
        return mutex_ != nullptr;
    }

    bool push(T &&value, TickType_t wait = 0) {
        if (!mutex_ || xSemaphoreTake(mutex_, wait) != pdTRUE) return false;

        const bool accepted = queue_.push(std::move(value));
        xSemaphoreGive(mutex_);
        return accepted;
    }

    bool pop(T &value) {
        if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return false;

        const bool present = queue_.pop(value);
        xSemaphoreGive(mutex_);
        return present;
    }

    void clear(TickType_t wait = 0) {
        if (!mutex_ || xSemaphoreTake(mutex_, wait) != pdTRUE) return;

        queue_.clear();
        xSemaphoreGive(mutex_);
    }

private:
    FixedQueue<T, Capacity> queue_;
    StaticSemaphore_t mutex_storage_ = {};
    SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace aircannect
