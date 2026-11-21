#include "prepared_byte_transfer.h"

namespace aircannect {

void PreparedByteTransfer::bind(uint8_t *storage, size_t capacity) {
    ring_.bind(storage, capacity);
}

bool PreparedByteTransfer::commit_write(size_t length) {
    if (!ring_.commit_write(length)) return false;
    if (length) notify_reader();
    return true;
}

void PreparedByteTransfer::arm(const std::shared_ptr<PreparedReadSignal> &signal,
                               uint32_t generation) {
    if (!signal_) signal_ = signal;
    waiting_.exchange(generation, std::memory_order_acq_rel);
    if (readable() || producer_done()) notify_reader();
}

void PreparedByteTransfer::notify_reader() {
    // Even an unarmed publication synchronizes with the next arm and recheck.
    const uint32_t generation = waiting_.exchange(0, std::memory_order_acq_rel);
    if (generation) signal_->notify(generation);
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
    // Observe completion before reading: its release publishes the final bytes.
    // An empty read followed by a newly published completion is not EOF.
    const bool producer_finished = producer_done();
    result.bytes = ring_.read(buffer, max_length);
    if (result.bytes > 0) {
        consumed_.store(consumed + static_cast<uint32_t>(result.bytes),
                        std::memory_order_release);
        result.state = PreparedByteReadState::Data;
    } else if (!producer_finished) {
        result.state = PreparedByteReadState::Retry;
        result.wait = this;
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
