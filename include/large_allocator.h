#pragma once

#include <new>
#include <stdint.h>

#include "memory_manager.h"

namespace aircannect {

// STL ownership records for large, non-timing-critical payloads belong in
// the same memory region as their data, not the internal heap.
template <typename T>
struct LargeAllocator {
    using value_type = T;

    LargeAllocator() = default;
    template <typename U> LargeAllocator(const LargeAllocator<U> &) {}

    T *allocate(size_t count) {
        void *memory = count <= SIZE_MAX / sizeof(T)
            ? Memory::alloc_large(count * sizeof(T), false) : nullptr;
        if (memory) return static_cast<T *>(memory);
        throw std::bad_alloc();
    }

    void deallocate(T *value, size_t) { Memory::free(value); }

    template <typename U>
    bool operator==(const LargeAllocator<U> &) const { return true; }
    template <typename U>
    bool operator!=(const LargeAllocator<U> &) const { return false; }
};

}  // namespace aircannect
