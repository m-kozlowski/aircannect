#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stddef.h>
#include <type_traits>
#include <utility>

#include "fixed_queue.h"
#include "large_object.h"

namespace aircannect {

enum class InboxStorage { Internal, Psram };

// Bounded handoff for commands produced by another task and consumed by the
// main loop. The contained FixedQueue destroys each popped owning value.
template <typename T, size_t Capacity,
          InboxStorage Storage = InboxStorage::Internal>
class MainLoopInbox {
public:
    MainLoopInbox() = default;
    MainLoopInbox(const MainLoopInbox &) = delete;
    MainLoopInbox &operator=(const MainLoopInbox &) = delete;

    ~MainLoopInbox() {
        if constexpr (Storage == InboxStorage::Psram) {
            LargeObject::destroy(queue_);
        }
    }

    bool begin() {
        if constexpr (Storage == InboxStorage::Psram) {
            if (!queue_) queue_ = LargeObject::create<Queue>();
            if (!queue_) return false;
        }

        if (!mutex_) {
            mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
        }
        return mutex_ != nullptr;
    }

    bool push(T &&value, TickType_t wait = 0) {
        if (!mutex_ || xSemaphoreTake(mutex_, wait) != pdTRUE) return false;

        const bool accepted = queue()->push(std::move(value));
        xSemaphoreGive(mutex_);
        return accepted;
    }

    bool pop(T &value) {
        if (!mutex_ || xSemaphoreTake(mutex_, 0) != pdTRUE) return false;

        const bool present = queue()->pop(value);
        xSemaphoreGive(mutex_);
        return present;
    }

    void clear(TickType_t wait = 0) {
        if (!mutex_ || xSemaphoreTake(mutex_, wait) != pdTRUE) return;

        queue()->clear();
        xSemaphoreGive(mutex_);
    }

private:
    using Queue = FixedQueue<T, Capacity>;

    Queue *queue() {
        if constexpr (Storage == InboxStorage::Psram) return queue_;
        else return &queue_;
    }

    std::conditional_t<Storage == InboxStorage::Psram, Queue *, Queue>
        queue_{};
    StaticSemaphore_t mutex_storage_ = {};
    SemaphoreHandle_t mutex_ = nullptr;
};

}  // namespace aircannect
