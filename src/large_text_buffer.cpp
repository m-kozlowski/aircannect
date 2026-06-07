#include "large_text_buffer.h"

#include <string.h>

#include "memory_manager.h"

namespace aircannect {

LargeTextBuffer::~LargeTextBuffer() {
    Memory::free(data_);
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

LargeTextBuffer &LargeTextBuffer::operator+=(const String &text) {
    append(text.c_str(), text.length());
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

    char *next = static_cast<char *>(Memory::alloc_large(target + 1));
    if (!next) return false;
    if (data_ && length_) memcpy(next, data_, length_);
    next[length_] = 0;
    Memory::free(data_);
    data_ = next;
    capacity_ = target;
    return true;
}

}  // namespace aircannect
