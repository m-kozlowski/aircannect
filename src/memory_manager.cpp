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
