#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "board.h"

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
MemoryStatus status();
MemoryDetailStatus detail_status();
bool psram_available();

void *alloc_large(size_t size, bool allow_internal_fallback = true);
void *calloc_large(size_t count,
                   size_t size,
                   bool allow_internal_fallback = true);
void free(void *ptr);

}  // namespace Memory
}  // namespace aircannect
