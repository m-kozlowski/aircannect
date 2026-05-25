#include "memory_manager.h"

#if AC_MEMORY_HEAP_TRACE_ENABLED
#include <esp_err.h>
#include <esp_heap_trace.h>
#endif

#include <esp_heap_caps.h>
#include <string.h>

namespace aircannect {
namespace Memory {
namespace {

bool initialized = false;
bool psram_detected = false;

#if AC_MEMORY_HEAP_TRACE_ENABLED
extern "C" esp_err_t heap_trace_init_standalone(
    heap_trace_record_t *record_buffer,
    size_t num_records) __attribute__((weak));
extern "C" esp_err_t heap_trace_start(
    heap_trace_mode_t mode) __attribute__((weak));
extern "C" esp_err_t heap_trace_stop() __attribute__((weak));
extern "C" size_t heap_trace_get_count() __attribute__((weak));
extern "C" esp_err_t heap_trace_get(
    size_t index,
    heap_trace_record_t *record) __attribute__((weak));
extern "C" esp_err_t heap_trace_summary(
    heap_trace_summary_t *summary) __attribute__((weak));

heap_trace_record_t *heap_trace_records = nullptr;
bool heap_trace_initialized = false;
bool heap_trace_running = false;
HeapTraceMode heap_trace_current_mode = HeapTraceMode::Leaks;
const char *heap_trace_last_error = "";

const char *trace_error_name(esp_err_t err) {
    switch (err) {
        case ESP_OK: return "";
        case ESP_ERR_NOT_SUPPORTED: return "not_supported";
        case ESP_ERR_INVALID_STATE: return "invalid_state";
        case ESP_ERR_INVALID_ARG: return "invalid_arg";
        case ESP_ERR_NO_MEM: return "no_mem";
        default: return "esp_error";
    }
}

bool heap_trace_backend_available() {
    return heap_trace_init_standalone && heap_trace_start &&
           heap_trace_stop && heap_trace_get_count && heap_trace_get;
}

bool heap_trace_alloc_records() {
    if (heap_trace_records) return true;
    heap_trace_records = static_cast<heap_trace_record_t *>(
        heap_caps_calloc(AC_MEMORY_HEAP_TRACE_RECORDS,
                         sizeof(heap_trace_record_t),
                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!heap_trace_records) {
        heap_trace_last_error = "no_mem";
        return false;
    }
    return true;
}

void heap_trace_free_records() {
    if (!heap_trace_records) return;
    ::free(heap_trace_records);
    heap_trace_records = nullptr;
}

void heap_trace_init_if_needed() {
    if (heap_trace_initialized) return;
    if (!heap_trace_backend_available()) {
        heap_trace_last_error = "backend_unavailable";
        return;
    }
    if (!heap_trace_alloc_records()) return;

    const esp_err_t err = heap_trace_init_standalone(
        heap_trace_records, AC_MEMORY_HEAP_TRACE_RECORDS);
    heap_trace_initialized = err == ESP_OK;
    heap_trace_last_error = trace_error_name(err);
    if (!heap_trace_initialized) heap_trace_free_records();
}
#endif

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
#if AC_MEMORY_HEAP_TRACE_ENABLED
    heap_trace_init_if_needed();
#endif
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

const char *heap_trace_mode_name(HeapTraceMode mode) {
    switch (mode) {
        case HeapTraceMode::Leaks: return "leaks";
        case HeapTraceMode::All: return "all";
        default: return "unknown";
    }
}

HeapTraceStatus heap_trace_status() {
    ensure_begin();
    HeapTraceStatus out;
    out.build_enabled = AC_MEMORY_HEAP_TRACE_ENABLED != 0;
    out.mode = HeapTraceMode::Leaks;
#if AC_MEMORY_HEAP_TRACE_ENABLED
    out.backend_available = heap_trace_backend_available();
    out.initialized = heap_trace_initialized;
    out.running = heap_trace_running;
    out.mode = heap_trace_current_mode;
    out.capacity = AC_MEMORY_HEAP_TRACE_RECORDS;
    out.last_error = heap_trace_last_error;
    if (out.backend_available && heap_trace_initialized) {
        if (heap_trace_summary) {
            heap_trace_summary_t summary = {};
            const esp_err_t err = heap_trace_summary(&summary);
            if (err == ESP_OK) {
                out.count = summary.count;
                out.capacity = summary.capacity;
                out.total_allocations = summary.total_allocations;
                out.total_frees = summary.total_frees;
                out.high_water_mark = summary.high_water_mark;
                out.overflowed = summary.has_overflowed != 0;
            } else {
                heap_trace_last_error = trace_error_name(err);
                out.last_error = heap_trace_last_error;
            }
        } else {
            out.count = heap_trace_get_count();
        }
    }
#else
    out.last_error = "disabled";
#endif
    return out;
}

bool heap_trace_start(HeapTraceMode mode) {
    ensure_begin();
#if AC_MEMORY_HEAP_TRACE_ENABLED
    heap_trace_init_if_needed();
    if (!heap_trace_backend_available()) {
        heap_trace_last_error = "backend_unavailable";
        return false;
    }
    if (!heap_trace_initialized) return false;

    const heap_trace_mode_t native_mode =
        mode == HeapTraceMode::All ? HEAP_TRACE_ALL : HEAP_TRACE_LEAKS;
    const esp_err_t err = ::heap_trace_start(native_mode);
    heap_trace_last_error = trace_error_name(err);
    if (err != ESP_OK) return false;
    heap_trace_current_mode = mode;
    heap_trace_running = true;
    return true;
#else
    (void)mode;
    return false;
#endif
}

bool heap_trace_stop() {
    ensure_begin();
#if AC_MEMORY_HEAP_TRACE_ENABLED
    if (!heap_trace_backend_available()) {
        heap_trace_last_error = "backend_unavailable";
        return false;
    }
    if (!heap_trace_initialized) {
        heap_trace_last_error = "not_initialized";
        return false;
    }
    const esp_err_t err = ::heap_trace_stop();
    heap_trace_last_error = trace_error_name(err);
    if (err != ESP_OK) return false;
    heap_trace_running = false;
    return true;
#else
    return false;
#endif
}

bool heap_trace_clear() {
    ensure_begin();
#if AC_MEMORY_HEAP_TRACE_ENABLED
    if (heap_trace_running) {
        const esp_err_t stop_err = ::heap_trace_stop();
        if (stop_err != ESP_OK) {
            heap_trace_last_error = trace_error_name(stop_err);
            return false;
        }
        heap_trace_running = false;
    }
    if (!heap_trace_backend_available()) {
        heap_trace_last_error = "backend_unavailable";
        return false;
    }
    if (heap_trace_initialized && heap_trace_init_standalone) {
        heap_trace_init_standalone(nullptr, 0);
    }
    heap_trace_free_records();
    heap_trace_initialized = false;
    heap_trace_last_error = "";
    return true;
#else
    return false;
#endif
}

bool heap_trace_record(size_t index, HeapTraceRecord &record) {
    ensure_begin();
    record = {};
#if AC_MEMORY_HEAP_TRACE_ENABLED
    if (!heap_trace_backend_available()) {
        heap_trace_last_error = "backend_unavailable";
        return false;
    }
    if (!heap_trace_initialized) {
        heap_trace_last_error = "not_initialized";
        return false;
    }
    heap_trace_record_t native = {};
    const esp_err_t err = heap_trace_get(index, &native);
    heap_trace_last_error = trace_error_name(err);
    if (err != ESP_OK) return false;
    record.address = reinterpret_cast<uintptr_t>(native.address);
    record.size = native.size;
    record.freed = native.freed;
#if CONFIG_HEAP_TRACING_STACK_DEPTH > 0
    record.alloc_pc = reinterpret_cast<uintptr_t>(native.alloced_by[0]);
    record.free_pc = reinterpret_cast<uintptr_t>(native.freed_by[0]);
#endif
    return native.address != nullptr;
#else
    (void)index;
    return false;
#endif
}

}  // namespace Memory
}  // namespace aircannect
