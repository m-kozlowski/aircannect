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
    size_ = 0;
    offered_write_ = 0;
}

uint8_t *PreparedByteRing::write_span(size_t &length) {
    length = 0;
    offered_write_ = 0;
    if (!storage_ || size_ >= capacity_) return nullptr;

    const size_t tail = (head_ + size_) % capacity_;
    length = std::min(capacity_ - size_, capacity_ - tail);
    offered_write_ = length;
    return storage_ + tail;
}

bool PreparedByteRing::commit_write(size_t length) {
    if (length > offered_write_ || length > writable()) return false;

    size_ += length;
    offered_write_ = 0;
    return true;
}

size_t PreparedByteRing::read(uint8_t *destination, size_t max_length) {
    if (!destination || max_length == 0 || size_ == 0) return 0;

    const size_t total = std::min(max_length, size_);
    const size_t first = std::min(total, capacity_ - head_);
    memcpy(destination, storage_ + head_, first);
    if (first < total) {
        memcpy(destination + first, storage_, total - first);
    }

    head_ = (head_ + total) % capacity_;
    size_ -= total;
    return total;
}

}  // namespace aircannect
