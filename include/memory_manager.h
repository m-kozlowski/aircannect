#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct MemoryRegionStatus {
    size_t free_bytes = 0;
    size_t allocated_bytes = 0;
    size_t largest_free_block = 0;
    size_t minimum_free_bytes = 0;
    size_t allocated_blocks = 0;
    size_t free_blocks = 0;
    size_t total_blocks = 0;
};

struct MemoryStatus {
    bool psram_available = false;
    size_t heap_total = 0;
    size_t heap_free = 0;
    size_t heap_max_alloc = 0;
    size_t psram_total = 0;
    size_t psram_free = 0;
    size_t psram_max_alloc = 0;
};

struct MemoryDetailStatus {
    MemoryStatus summary;
    MemoryRegionStatus default_8bit;
    MemoryRegionStatus internal_8bit;
    MemoryRegionStatus internal_dma;
    MemoryRegionStatus psram_8bit;
};

namespace Memory {

void begin();

// Largest-block queries walk matching heaps under their spinlocks.
// Pass false to skip these queries and leave the largest-block fields zero.
MemoryStatus status(bool include_largest_free_block = true);
MemoryDetailStatus detail_status();
#ifdef ARDUINO
MemoryRegionStatus region_status(uint32_t caps);
#endif
bool psram_available();

// Select a smaller working set on boards with at most 2 MiB PSRAM.
// Uses boot-time capacity, not transient free heap or a heap walk.
size_t buffer_budget(size_t normal_bytes, size_t small_psram_bytes);

void *alloc_dma(size_t size);
void *alloc_large(size_t size, bool allow_internal_fallback = true);

// Preserves the old allocation on failure. On esp32, allocate/copy/free keeps
// payload copies outside the heap lock; both allocations coexist briefly.
void *realloc_large(void *ptr,
                    size_t size,
                    bool allow_internal_fallback = true);

void *calloc_large(size_t count,
                   size_t size,
                   bool allow_internal_fallback = true);
void free(void *ptr);

}  // namespace Memory
}  // namespace aircannect
