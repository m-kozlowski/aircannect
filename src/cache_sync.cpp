#include <cstddef>
#include <cstdint>
#include <sys/lock.h>

#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <sdkconfig.h>

extern "C" esp_err_t __real_esp_cache_msync(void *addr, size_t size, int flags);

namespace {

constexpr size_t CHUNK_BYTES = 2048;
constexpr size_t CACHE_LINE_BYTES = CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE;
constexpr int WRITEBACK_FLAGS = ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED;
static_assert(CHUNK_BYTES % CACHE_LINE_BYTES == 0);

DRAM_ATTR _lock_t sync_lock;

}  // namespace

extern "C" esp_err_t IRAM_ATTR __wrap_esp_cache_msync(
    void *addr, size_t size, int flags) {
    if (xPortInIsrContext() || !xPortCanYield()) {
        return __real_esp_cache_msync(addr, size, flags);
    }

    // Match IDF's chunked mode: serialize task calls, not ISR/critical calls.
    // Each real call retains the SDK spinlock and ROM cache errata workarounds.
    _lock_acquire(&sync_lock);

    const uintptr_t start = reinterpret_cast<uintptr_t>(addr);
    uintptr_t end = 0;
    const bool overflow = __builtin_add_overflow(start, size, &end);
    const bool aligned = (start % CACHE_LINE_BYTES == 0) &&
                         (size % CACHE_LINE_BYTES == 0);
    const bool chunked = size > CHUNK_BYTES && !overflow &&
                         (flags & ~WRITEBACK_FLAGS) == 0 &&
                         (aligned || (flags & ESP_CACHE_MSYNC_FLAG_UNALIGNED)) &&
                         esp_ptr_external_ram(addr) &&
                         esp_ptr_external_ram(reinterpret_cast<void *>(end - 1));

    esp_err_t result = ESP_OK;
    if (chunked) {
        // Keep intermediate boundaries aligned; only the original edges may
        // be unaligned. Invalid ranges/flags go unchanged to SDK validation.
        uintptr_t cursor = start;
        size_t remaining = size;
        while (remaining) {
            const size_t limit = CHUNK_BYTES - cursor % CACHE_LINE_BYTES;
            const size_t count = remaining < limit ? remaining : limit;
            result = __real_esp_cache_msync(
                reinterpret_cast<void *>(cursor), count, flags);

            if (result != ESP_OK) break;

            cursor += count;
            remaining -= count;
        }
    } else {
        result = __real_esp_cache_msync(addr, size, flags);
    }

    _lock_release(&sync_lock);
    return result;
}
