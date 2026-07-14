#include "prepared_byte_transfer.h"

namespace aircannect {

void PreparedByteTransfer::bind(uint8_t *storage, size_t capacity) {
    ring_.bind(storage, capacity);
    consumed_ = 0;
    consumer_activity_ms_ = 0;
    consumer_attached_ = false;
    consumer_closed_ = false;
    producer_done_ = false;
    cancel_requested_.store(false, std::memory_order_relaxed);
}

void PreparedByteTransfer::attach(uint32_t now_ms) {
    consumer_attached_ = true;
    consumer_activity_ms_ = now_ms;
}

PreparedByteRead PreparedByteTransfer::read(uint8_t *buffer, size_t max_length,
                                            size_t offset, uint32_t now_ms) {
    PreparedByteRead result;
    if (!buffer || max_length == 0 || !consumer_attached_ ||
        consumer_closed_ || offset != consumed_) {
        return result;
    }

    consumer_activity_ms_ = now_ms;
    result.bytes = ring_.read(buffer, max_length);
    if (result.bytes > 0) {
        consumed_ += result.bytes;
        result.state = PreparedByteReadState::Data;
    } else if (!producer_done_) {
        result.state = PreparedByteReadState::Retry;
    }
    return result;
}

void PreparedByteTransfer::finish(bool complete) {
    consumer_closed_ = true;
    if (!complete) request_cancel();
}

void PreparedByteTransfer::request_cancel() {
    cancel_requested_.store(true, std::memory_order_relaxed);
}

bool PreparedByteTransfer::cancel_requested() const {
    return cancel_requested_.load(std::memory_order_relaxed);
}

}  // namespace aircannect
