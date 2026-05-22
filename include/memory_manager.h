#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct MemoryStatus {
    bool psram_available = false;
    size_t heap_total = 0;
    size_t heap_free = 0;
    size_t heap_max_alloc = 0;
    size_t psram_total = 0;
    size_t psram_free = 0;
    size_t psram_max_alloc = 0;
};

namespace Memory {

void begin();
MemoryStatus status();
bool psram_available();

void *alloc_large(size_t size, bool allow_internal_fallback = true);
void *calloc_large(size_t count,
                   size_t size,
                   bool allow_internal_fallback = true);
void *alloc_internal(size_t size);
void free(void *ptr);

}  // namespace Memory
}  // namespace aircannect
