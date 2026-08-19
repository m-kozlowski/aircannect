#pragma once

#include <new>
#include <utility>

#include "memory_manager.h"

namespace aircannect {
namespace LargeObject {

template <typename T, typename... Args>
T *create(Args &&...args) {
    void *memory = Memory::alloc_large(sizeof(T), false);
    return memory
        ? new (memory) T(std::forward<Args>(args)...)
        : nullptr;
}

template <typename T>
void destroy(T *value) {
    if (!value) return;

    value->~T();
    Memory::free(value);
}

}  // namespace LargeObject
}  // namespace aircannect
