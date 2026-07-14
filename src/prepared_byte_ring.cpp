#include "prepared_byte_ring.h"

#include <algorithm>
#include <string.h>

namespace aircannect {

void PreparedByteRing::bind(uint8_t *storage, size_t capacity) {
    storage_ = storage;
    capacity_ = storage ? capacity : 0;
    clear();
}

void PreparedByteRing::clear() {
    head_ = 0;
    tail_ = 0;
    offered_write_ = 0;
    size_.store(0, std::memory_order_relaxed);
}

size_t PreparedByteRing::readable() const {
    return size_.load(std::memory_order_acquire);
}

size_t PreparedByteRing::writable() const {
    return capacity_ - readable();
}

uint8_t *PreparedByteRing::write_span(size_t &length) {
    length = 0;
    offered_write_ = 0;
    if (!storage_) return nullptr;

    const size_t available = writable();
    if (available == 0) return nullptr;

    length = std::min(available, capacity_ - tail_);
    offered_write_ = length;
    return storage_ + tail_;
}

bool PreparedByteRing::commit_write(size_t length) {
    if (!storage_ || capacity_ == 0 || length > offered_write_) return false;

    tail_ = (tail_ + length) % capacity_;
    offered_write_ = 0;
    size_.fetch_add(static_cast<uint32_t>(length),
                    std::memory_order_release);
    return true;
}

size_t PreparedByteRing::read(uint8_t *destination, size_t max_length) {
    if (!destination || max_length == 0) return 0;

    const size_t total = std::min(max_length, readable());
    if (total == 0) return 0;

    const size_t first = std::min(total, capacity_ - head_);
    memcpy(destination, storage_ + head_, first);
    if (first < total) {
        memcpy(destination + first, storage_, total - first);
    }

    head_ = (head_ + total) % capacity_;
    size_.fetch_sub(static_cast<uint32_t>(total),
                    std::memory_order_release);
    return total;
}

}  // namespace aircannect
