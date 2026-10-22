#pragma once

#include <stdint.h>

namespace aircannect {

enum class SummaryFetchReason : uint8_t {
    Unspecified, Catalog, PostTherapy, ManualStr,
};

}  // namespace aircannect
