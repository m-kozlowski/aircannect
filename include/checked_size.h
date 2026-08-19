#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {
namespace CheckedSize {

inline bool add(size_t lhs, size_t rhs, size_t &out) {
    if (lhs > SIZE_MAX - rhs) return false;

    out = lhs + rhs;
    return true;
}

inline bool add_to(size_t &value, size_t increment) {
    return add(value, increment, value);
}

inline bool multiply(size_t count, size_t width, size_t &out) {
    if (width != 0 && count > SIZE_MAX / width) return false;

    out = count * width;
    return true;
}

inline bool add_array(size_t &total, size_t count, size_t item_size) {
    size_t bytes = 0;
    return multiply(count, item_size, bytes) && add_to(total, bytes);
}

inline bool align_up(size_t value, size_t alignment, size_t &out) {
    if (alignment == 0) return false;

    const size_t remainder = value % alignment;
    if (remainder == 0) {
        out = value;
        return true;
    }

    return add(value, alignment - remainder, out);
}

inline bool reserve_array(size_t &total,
                          size_t count,
                          size_t item_size,
                          size_t alignment,
                          size_t &offset) {
    size_t bytes = 0;
    if (!align_up(total, alignment, offset) ||
        !multiply(count, item_size, bytes)) {
        return false;
    }

    return add(offset, bytes, total);
}

template <typename T>
bool reserve_array(size_t &total, size_t count, size_t &offset) {
    return reserve_array(total, count, sizeof(T), alignof(T), offset);
}

}  // namespace CheckedSize
}  // namespace aircannect
