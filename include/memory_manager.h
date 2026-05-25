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

enum class HeapTraceMode : uint8_t {
    Leaks,
    All,
};

struct HeapTraceStatus {
    bool build_enabled = false;
    bool backend_available = false;
    bool initialized = false;
    bool running = false;
    HeapTraceMode mode = HeapTraceMode::Leaks;
    size_t count = 0;
    size_t capacity = 0;
    size_t total_allocations = 0;
    size_t total_frees = 0;
    size_t high_water_mark = 0;
    bool overflowed = false;
    const char *last_error = "";
};

struct HeapTraceRecord {
    uintptr_t address = 0;
    size_t size = 0;
    bool freed = false;
    uintptr_t alloc_pc = 0;
    uintptr_t free_pc = 0;
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
void *alloc_internal(size_t size);
void free(void *ptr);

HeapTraceStatus heap_trace_status();
bool heap_trace_start(HeapTraceMode mode);
bool heap_trace_stop();
bool heap_trace_clear();
bool heap_trace_record(size_t index, HeapTraceRecord &record);
const char *heap_trace_mode_name(HeapTraceMode mode);

}  // namespace Memory
}  // namespace aircannect
