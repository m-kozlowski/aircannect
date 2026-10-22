#pragma once

#include <stdint.h>

namespace aircannect {

enum class SummaryFetchReason : uint8_t {
    // Persisted in archive observations; keep existing numeric values stable.
    Unspecified = 0,
    Catalog = 1,
    PostTherapy = 2,
    ManualStr = 3,
};

}  // namespace aircannect
