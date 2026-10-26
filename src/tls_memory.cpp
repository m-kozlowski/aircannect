#include "tls_memory.h"

#include <atomic>
#include <stdint.h>
#include <stdlib.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>

#include "memory_manager.h"

#if AC_STACK_PROFILE_ENABLED
#include <Arduino.h>
#include <esp_memory_utils.h>
#include "debug_log.h"
#endif

namespace aircannect {
namespace TlsMemory {
namespace {

static constexpr size_t TLS_LARGE_ALLOC_THRESHOLD = 4096;
static constexpr size_t TLS_MIN_INTERNAL_FREE = 60 * 1024;
static constexpr size_t TLS_MIN_INTERNAL_LARGEST = 36 * 1024;
static constexpr size_t TLS_MIN_INTERNAL_FREE_WITH_PSRAM = 40 * 1024;
static constexpr size_t TLS_MIN_INTERNAL_LARGEST_WITH_PSRAM = 8 * 1024;

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

#if AC_STACK_PROFILE_ENABLED
struct RegionProfile {
    size_t live = 0;
    size_t peak = 0;
    uint32_t count[4] = {};
    size_t bytes[4] = {};
};

struct WindowProfile {
    bool active = false;
    uint32_t started_ms = 0;
    uint32_t sequence = 0;
    size_t free_before = 0;
    size_t largest_before = 0;
    RegionProfile before[2];
    uint32_t failures_before = 0;
    uint32_t fallback_before = 0;
};

portMUX_TYPE profile_mux = portMUX_INITIALIZER_UNLOCKED;
RegionProfile regions[2];
WindowProfile window;

void record_allocation(void *ptr, size_t requested) {
    const size_t size = heap_caps_get_allocated_size(ptr);
    const unsigned region = esp_ptr_external_ram(ptr) ? 1 : 0;
    const unsigned bucket = requested < 256 ? 0 : requested < 1024 ? 1 :
                            requested < 4096 ? 2 : 3;

    portENTER_CRITICAL(&profile_mux);
    RegionProfile &profile = regions[region];
    profile.live += size;
    if (profile.live > profile.peak) profile.peak = profile.live;
    ++profile.count[bucket];
    profile.bytes[bucket] += size;
    portEXIT_CRITICAL(&profile_mux);
}

void record_free(void *ptr) {
    const size_t size = heap_caps_get_allocated_size(ptr);
    const unsigned region = esp_ptr_external_ram(ptr) ? 1 : 0;

    portENTER_CRITICAL(&profile_mux);
    regions[region].live -= size;
    portEXIT_CRITICAL(&profile_mux);
}
#endif

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
#if AC_STACK_PROFILE_ENABLED
                record_allocation(ptr, total);
#endif
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
#if AC_STACK_PROFILE_ENABLED
            record_allocation(ptr, total);
#endif
            return ptr;
        }

        bump(large_fail);
        return nullptr;
    }

    void *ptr = alloc_internal_zeroed(count, size);
    if (ptr) {
        bump(small_internal);
#if AC_STACK_PROFILE_ENABLED
        record_allocation(ptr, total);
#endif
        return ptr;
    }

    bump(small_fail);
    return nullptr;
}

void tls_free(void *ptr) {
    if (!ptr) return;
#if AC_STACK_PROFILE_ENABLED
    record_free(ptr);
#endif
    bump(frees);
    ::free(ptr);
}

}  // namespace

#if AC_STACK_PROFILE_ENABLED
Measurement::~Measurement() {
    stop();
}

void Measurement::start(const char *owner) {
    if (owner_) return;

    portENTER_CRITICAL(&profile_mux);
    const bool busy = window.active;
    if (!busy) window.active = true;
    portEXIT_CRITICAL(&profile_mux);

    if (busy) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[TLS MEM] owner=%s skipped=measurement_busy\n", owner);
        return;
    }

    const esp_err_t result = heap_caps_monitor_local_minimum_free_size_start();
    if (result != ESP_OK) {
        portENTER_CRITICAL(&profile_mux);
        window.active = false;
        portEXIT_CRITICAL(&profile_mux);
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[TLS MEM] owner=%s monitor_error=%d\n", owner, result);
        return;
    }

    owner_ = owner;
    window.started_ms = millis();
    ++window.sequence;
    window.free_before = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    window.largest_before = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    window.failures_before = large_fail.load() + small_fail.load();
    window.fallback_before = large_internal_fallback.load();

    portENTER_CRITICAL(&profile_mux);
    for (unsigned i = 0; i < 2; ++i) {
        window.before[i] = regions[i];
        regions[i].peak = regions[i].live;
    }
    portEXIT_CRITICAL(&profile_mux);
}

void Measurement::stop() {
    if (!owner_) return;

    RegionProfile after[2];
    portENTER_CRITICAL(&profile_mux);
    for (unsigned i = 0; i < 2; ++i) after[i] = regions[i];
    portEXIT_CRITICAL(&profile_mux);

    // IDF sums each heap region's minimum; their lows need not be simultaneous.
    const size_t minimum = heap_caps_get_minimum_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    const size_t remaining = heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    Log::logf(CAT_GENERAL, LOG_INFO,
              "[TLS MEM] owner=%s id=%lu ms=%lu heap_before=%u "
              "heap_min_sum=%u heap_after=%u largest=%u/%u\n",
              owner_, static_cast<unsigned long>(window.sequence),
              static_cast<unsigned long>(millis() - window.started_ms),
              static_cast<unsigned>(window.free_before),
              static_cast<unsigned>(minimum), static_cast<unsigned>(remaining),
              static_cast<unsigned>(window.largest_before),
              static_cast<unsigned>(largest));

    for (unsigned i = 0; i < 2; ++i) {
        const RegionProfile &before = window.before[i];
        const RegionProfile &end = after[i];
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[TLS MEM] id=%lu region=%s live_before=%u peak=%u "
                  "live_after=%u\n",
                  static_cast<unsigned long>(window.sequence),
                  i ? "psram" : "internal",
                  static_cast<unsigned>(before.live),
                  static_cast<unsigned>(end.peak),
                  static_cast<unsigned>(end.live));

        for (unsigned b = 0; b < 4; ++b) {
            const uint32_t count = end.count[b] - before.count[b];
            if (!count) continue;

            static const char *const buckets[] = {
                "lt256", "256_1023", "1024_4095", "ge4096"};
            Log::logf(CAT_GENERAL, LOG_INFO,
                      "[TLS MEM] id=%lu region=%s bucket=%s count=%lu "
                      "allocated_bytes=%u\n",
                      static_cast<unsigned long>(window.sequence),
                      i ? "psram" : "internal", buckets[b],
                      static_cast<unsigned long>(count),
                      static_cast<unsigned>(end.bytes[b] - before.bytes[b]));
        }
    }

    Log::logf(CAT_GENERAL, LOG_INFO,
              "[TLS MEM] id=%lu alloc_failures=%lu internal_fallbacks=%lu\n",
              static_cast<unsigned long>(window.sequence),
              static_cast<unsigned long>(large_fail.load() + small_fail.load() -
                                         window.failures_before),
              static_cast<unsigned long>(large_internal_fallback.load() -
                                         window.fallback_before));

    owner_ = nullptr;
    portENTER_CRITICAL(&profile_mux);
    window.active = false;
    portEXIT_CRITICAL(&profile_mux);
}
#endif

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

TlsMemoryAdmission admission() {
    const TlsMemoryStatus tls = status();

    TlsMemoryAdmission out;
    out.psram_allocator =
        tls.installed && tls.psram_enabled && tls.install_result == 0;
    out.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    out.internal_largest = heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    out.minimum_free = out.psram_allocator
        ? TLS_MIN_INTERNAL_FREE_WITH_PSRAM
        : TLS_MIN_INTERNAL_FREE;
    out.minimum_largest = out.psram_allocator
        ? TLS_MIN_INTERNAL_LARGEST_WITH_PSRAM
        : TLS_MIN_INTERNAL_LARGEST;
    out.available = out.internal_free >= out.minimum_free &&
        out.internal_largest >= out.minimum_largest;
    return out;
}

}  // namespace TlsMemory
}  // namespace aircannect
