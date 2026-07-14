#include "prepared_byte_transfer.h"

namespace aircannect {

void PreparedByteTransfer::bind(uint8_t *storage, size_t capacity) {
    ring_.bind(storage, capacity);
    consumed_.store(0, std::memory_order_relaxed);
    consumer_activity_ms_.store(0, std::memory_order_relaxed);
    consumer_attached_.store(false, std::memory_order_relaxed);
    consumer_closed_.store(false, std::memory_order_relaxed);
    producer_done_.store(false, std::memory_order_relaxed);
    cancel_requested_.store(false, std::memory_order_relaxed);
}

void PreparedByteTransfer::attach(uint32_t now_ms) {
    consumer_activity_ms_.store(now_ms, std::memory_order_relaxed);
    consumer_attached_.store(true, std::memory_order_release);
}

PreparedByteRead PreparedByteTransfer::read(uint8_t *buffer, size_t max_length,
                                            size_t offset, uint32_t now_ms) {
    PreparedByteRead result;
    const uint32_t consumed = consumed_.load(std::memory_order_acquire);
    if (!buffer || max_length == 0 || !consumer_attached() ||
        consumer_closed() || offset != consumed) {
        return result;
    }

    consumer_activity_ms_.store(now_ms, std::memory_order_release);
    result.bytes = ring_.read(buffer, max_length);
    if (result.bytes > 0) {
        consumed_.store(consumed + static_cast<uint32_t>(result.bytes),
                        std::memory_order_release);
        result.state = PreparedByteReadState::Data;
    } else if (!producer_done()) {
        result.state = PreparedByteReadState::Retry;
    }
    return result;
}

void PreparedByteTransfer::finish(bool complete) {
    if (!complete) request_cancel();
    consumer_closed_.store(true, std::memory_order_release);
}

void PreparedByteTransfer::request_cancel() {
    cancel_requested_.store(true, std::memory_order_release);
}

bool PreparedByteTransfer::cancel_requested() const {
    return cancel_requested_.load(std::memory_order_acquire);
}

}  // namespace aircannect
