#pragma once

#include <new>

#include "checked_size.h"
#include "memory_manager.h"

namespace aircannect {

template <typename T>
class LargeScratchArray {
public:
    LargeScratchArray() = default;
    LargeScratchArray(const LargeScratchArray &) = delete;
    LargeScratchArray &operator=(const LargeScratchArray &) = delete;

    ~LargeScratchArray() {
        for (size_t i = 0; i < capacity_; ++i) values_[i].~T();
        Memory::free(values_);
    }

    bool allocate(size_t capacity) {
        if (capacity == 0) return true;

        size_t bytes = 0;
        if (values_ ||
            !CheckedSize::multiply(capacity, sizeof(T), bytes)) {
            return false;
        }

        values_ = static_cast<T *>(
            Memory::calloc_large(1, bytes, false));
        if (!values_) return false;

        capacity_ = capacity;
        for (size_t i = 0; i < capacity_; ++i) new (&values_[i]) T();
        return true;
    }

    T *append() {
        return size_ < capacity_ ? &values_[size_++] : nullptr;
    }

    T *data() { return values_; }
    const T *data() const { return values_; }
    size_t size() const { return size_; }

private:
    T *values_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

}  // namespace aircannect
