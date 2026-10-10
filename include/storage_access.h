#pragma once

#include <atomic>

namespace aircannect {

// Check under the port's admission mutex. Sealing does not cancel accepted work.
inline std::atomic<bool> storage_local_requests_enabled{true};

}  // namespace aircannect
