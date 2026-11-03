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

inline const char *summary_fetch_reason_name(SummaryFetchReason reason) {
    switch (reason) {
        case SummaryFetchReason::Unspecified: return "unspecified";
        case SummaryFetchReason::Catalog: return "catalog";
        case SummaryFetchReason::PostTherapy: return "post_therapy";
        case SummaryFetchReason::ManualStr: return "manual_str";
    }
    return "unknown";
}

}  // namespace aircannect
