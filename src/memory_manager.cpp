#include "memory_manager.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace aircannect {
namespace Memory {
namespace {

bool initialized = false;
bool psram_detected = false;

bool detect_psram() {
    return psramFound() && ESP.getPsramSize() > 0;
}

void ensure_begin() {
    if (!initialized) begin();
}

}  // namespace

void begin() {
    psram_detected = detect_psram();
    initialized = true;
}

MemoryStatus status() {
    ensure_begin();
    MemoryStatus out;
    out.heap_total = ESP.getHeapSize();
    out.heap_free = ESP.getFreeHeap();
    out.heap_max_alloc =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                         MALLOC_CAP_8BIT);
    out.psram_available = psram_detected && detect_psram();
    if (out.psram_available) {
        out.psram_total = ESP.getPsramSize();
        out.psram_free = ESP.getFreePsram();
        out.psram_max_alloc =
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                             MALLOC_CAP_8BIT);
    }
    return out;
}

MemoryRegionStatus region_status(uint32_t caps) {
    MemoryRegionStatus out;
    multi_heap_info_t info = {};
    heap_caps_get_info(&info, caps);
    out.free_bytes = info.total_free_bytes;
    out.allocated_bytes = info.total_allocated_bytes;
    out.largest_free_block = info.largest_free_block;
    out.minimum_free_bytes = info.minimum_free_bytes;
    out.allocated_blocks = info.allocated_blocks;
    out.free_blocks = info.free_blocks;
    out.total_blocks = info.total_blocks;
    return out;
}

MemoryDetailStatus detail_status() {
    ensure_begin();
    MemoryDetailStatus out;
    out.summary = status();
    out.default_8bit = region_status(MALLOC_CAP_8BIT);
    out.internal_8bit = region_status(MALLOC_CAP_INTERNAL |
                                      MALLOC_CAP_8BIT);
    out.internal_dma = region_status(MALLOC_CAP_INTERNAL |
                                    MALLOC_CAP_DMA);
    out.psram_8bit = region_status(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return out;
}

bool psram_available() {
    return status().psram_available;
}

void *alloc_large(size_t size, bool allow_internal_fallback) {
    ensure_begin();
    if (size == 0) return nullptr;

    if (psram_detected && detect_psram()) {
        void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM |
                                           MALLOC_CAP_8BIT);
        if (ptr) return ptr;
    }

    if (!allow_internal_fallback) return nullptr;
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *calloc_large(size_t count,
                   size_t size,
                   bool allow_internal_fallback) {
    if (count == 0 || size == 0) return nullptr;
    if (count > SIZE_MAX / size) return nullptr;

    const size_t bytes = count * size;
    void *ptr = alloc_large(bytes, allow_internal_fallback);
    if (ptr) memset(ptr, 0, bytes);
    return ptr;
}

void *alloc_internal(size_t size) {
    if (size == 0) return nullptr;
    return heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void free(void *ptr) {
    if (ptr) ::free(ptr);
}

}  // namespace Memory
}  // namespace aircannect
