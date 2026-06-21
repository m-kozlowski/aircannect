#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

struct TlsMemoryStatus {
    bool installed = false;
    bool psram_enabled = false;
    int install_result = 0;
    size_t large_threshold = 0;
    uint32_t large_psram = 0;
    uint32_t large_internal_fallback = 0;
    uint32_t large_internal_no_psram = 0;
    uint32_t large_fail = 0;
    uint32_t small_internal = 0;
    uint32_t small_fail = 0;
    uint32_t frees = 0;
};

namespace TlsMemory {

bool begin();
TlsMemoryStatus status();

}  // namespace TlsMemory
}  // namespace aircannect
