#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "prepared_byte_ring.h"

namespace aircannect {

class PreparedByteTransfer {
public:
    PreparedByteTransfer() = default;
    PreparedByteTransfer(const PreparedByteTransfer &) = delete;
    PreparedByteTransfer &operator=(const PreparedByteTransfer &) = delete;

    // producer
    void bind(uint8_t *storage, size_t capacity);
    uint8_t *write_span(size_t &length) { return ring_.write_span(length); }
    bool commit_write(size_t length) { return ring_.commit_write(length); }
    size_t readable() const { return ring_.readable(); }
    size_t writable() const { return ring_.writable(); }
    void mark_producer_done() {
        producer_done_.store(true, std::memory_order_release);
    }
    bool producer_done() const {
        return producer_done_.load(std::memory_order_acquire);
    }

    // consumer
    void attach(uint32_t now_ms);
    PreparedByteRead read(uint8_t *buffer, size_t max_length, size_t offset, uint32_t now_ms);
    void finish(bool complete);
    uint64_t consumed() const {
        return consumed_.load(std::memory_order_acquire);
    }
    uint32_t consumer_activity_ms() const {
        return consumer_activity_ms_.load(std::memory_order_acquire);
    }
    bool consumer_attached() const {
        return consumer_attached_.load(std::memory_order_acquire);
    }
    bool consumer_closed() const {
        return consumer_closed_.load(std::memory_order_acquire);
    }

    // cancellation
    void request_cancel();
    bool cancel_requested() const;

private:
    PreparedByteRing ring_;
    std::atomic<uint32_t> consumed_{0};
    std::atomic<uint32_t> consumer_activity_ms_{0};
    std::atomic<bool> consumer_attached_{false};
    std::atomic<bool> consumer_closed_{false};
    std::atomic<bool> producer_done_{false};
    std::atomic<bool> cancel_requested_{false};
};

}  // namespace aircannect
