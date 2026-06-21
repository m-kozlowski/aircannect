#include "tls_memory.h"

#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include "memory_manager.h"

namespace aircannect {
namespace TlsMemory {
namespace {

static constexpr size_t TLS_LARGE_ALLOC_THRESHOLD = 4096;

std::atomic<bool> installed{false};
std::atomic<bool> psram_enabled{false};
std::atomic<int> install_result{0};
std::atomic<uint32_t> large_psram{0};
std::atomic<uint32_t> large_internal_fallback{0};
std::atomic<uint32_t> large_internal_no_psram{0};
std::atomic<uint32_t> large_fail{0};
std::atomic<uint32_t> small_internal{0};
std::atomic<uint32_t> small_fail{0};
std::atomic<uint32_t> frees{0};

void bump(std::atomic<uint32_t> &counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}

void *alloc_internal_zeroed(size_t count, size_t size) {
    return heap_caps_calloc(count, size, MALLOC_CAP_INTERNAL |
                                         MALLOC_CAP_8BIT);
}

void *tls_calloc(size_t count, size_t size) {
    if (count == 0 || size == 0) return nullptr;
    if (count > SIZE_MAX / size) {
        bump(large_fail);
        return nullptr;
    }

    const size_t total = count * size;
    if (total >= TLS_LARGE_ALLOC_THRESHOLD) {
        if (psram_enabled.load(std::memory_order_relaxed)) {
            void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM |
                                                      MALLOC_CAP_8BIT);
            if (ptr) {
                bump(large_psram);
                return ptr;
            }
        }

        void *ptr = alloc_internal_zeroed(count, size);
        if (ptr) {
            if (psram_enabled.load(std::memory_order_relaxed)) {
                bump(large_internal_fallback);
            } else {
                bump(large_internal_no_psram);
            }
            return ptr;
        }

        bump(large_fail);
        return nullptr;
    }

    void *ptr = alloc_internal_zeroed(count, size);
    if (ptr) {
        bump(small_internal);
        return ptr;
    }

    bump(small_fail);
    return nullptr;
}

void tls_free(void *ptr) {
    if (!ptr) return;
    bump(frees);
    ::free(ptr);
}

}  // namespace

bool begin() {
    if (installed.load(std::memory_order_acquire)) return true;

    Memory::begin();
    psram_enabled.store(Memory::psram_available(), std::memory_order_release);

    const int rc = mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    install_result.store(rc, std::memory_order_release);
    installed.store(rc == 0, std::memory_order_release);
    return rc == 0;
}

TlsMemoryStatus status() {
    TlsMemoryStatus out;
    out.installed = installed.load(std::memory_order_acquire);
    out.psram_enabled = psram_enabled.load(std::memory_order_acquire);
    out.install_result = install_result.load(std::memory_order_acquire);
    out.large_threshold = TLS_LARGE_ALLOC_THRESHOLD;
    out.large_psram = large_psram.load(std::memory_order_relaxed);
    out.large_internal_fallback =
        large_internal_fallback.load(std::memory_order_relaxed);
    out.large_internal_no_psram =
        large_internal_no_psram.load(std::memory_order_relaxed);
    out.large_fail = large_fail.load(std::memory_order_relaxed);
    out.small_internal = small_internal.load(std::memory_order_relaxed);
    out.small_fail = small_fail.load(std::memory_order_relaxed);
    out.frees = frees.load(std::memory_order_relaxed);
    return out;
}

}  // namespace TlsMemory
}  // namespace aircannect
