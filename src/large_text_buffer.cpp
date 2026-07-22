#include "large_text_buffer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *allocate_buffer(size_t size) {
#if AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO
    return Memory::alloc_large(size);
#else
    return ::malloc(size);
#endif
}

void free_buffer(void *ptr) {
#if AIRCANNECT_LARGE_TEXT_BUFFER_HAS_ARDUINO
    Memory::free(ptr);
#else
    ::free(ptr);
#endif
}

}  // namespace

LargeTextBuffer::~LargeTextBuffer() {
    free_buffer(data_);
}

bool LargeTextBuffer::reserve(size_t capacity) {
    return ensure_capacity(capacity);
}

void LargeTextBuffer::clear() {
    length_ = 0;
    overflowed_ = false;
    if (data_) data_[0] = 0;
}

void LargeTextBuffer::swap(LargeTextBuffer &other) {
    char *data = data_;
    data_ = other.data_;
    other.data_ = data;

    size_t length = length_;
    length_ = other.length_;
    other.length_ = length;

    size_t capacity = capacity_;
    capacity_ = other.capacity_;
    other.capacity_ = capacity;

    bool overflowed = overflowed_;
    overflowed_ = other.overflowed_;
    other.overflowed_ = overflowed;
}

LargeTextBuffer &LargeTextBuffer::operator=(const char *text) {
    clear();
    append(text);
    return *this;
}

LargeTextBuffer &LargeTextBuffer::operator+=(const char *text) {
    append(text);
    return *this;
}

LargeTextBuffer &LargeTextBuffer::operator+=(char c) {
    append(&c, 1);
    return *this;
}

bool LargeTextBuffer::append(const char *text) {
    return append(text, text ? strlen(text) : 0);
}

bool LargeTextBuffer::append(const char *text, size_t len) {
    if (!text || len == 0) return true;
    if (len > SIZE_MAX - length_ - 1) {
        overflowed_ = true;
        return false;
    }
    const size_t needed = length_ + len;
    if (!ensure_capacity(needed)) {
        overflowed_ = true;
        return false;
    }
    memcpy(data_ + length_, text, len);
    length_ = needed;
    data_[length_] = 0;
    return true;
}

bool LargeTextBuffer::ensure_capacity(size_t needed) {
    if (needed <= capacity_) return true;

    size_t target = capacity_ ? capacity_ : 256;
    while (target < needed) {
        if (target > SIZE_MAX / 2) {
            target = needed;
            break;
        }
        target *= 2;
    }

    char *next = static_cast<char *>(allocate_buffer(target + 1));
    if (!next) return false;
    if (data_ && length_) memcpy(next, data_, length_);
    next[length_] = 0;
    free_buffer(data_);
    data_ = next;
    capacity_ = target;
    return true;
}

}  // namespace aircannect
