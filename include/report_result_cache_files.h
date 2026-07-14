#pragma once

#include <stdint.h>

namespace aircannect {

bool result_plot_cache_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag);

}  // namespace aircannect
