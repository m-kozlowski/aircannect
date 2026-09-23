#pragma once

#include <ArduinoJson.h>

#include "memory_manager.h"

namespace aircannect {

class LargeJsonAllocator final : public ArduinoJson::Allocator {
public:
    void *allocate(size_t size) override {
        return Memory::alloc_large(size, false);
    }

    void deallocate(void *pointer) override {
        Memory::free(pointer);
    }

    void *reallocate(void *pointer, size_t size) override {
        return Memory::realloc_large(pointer, size, false);
    }
};

}  // namespace aircannect
