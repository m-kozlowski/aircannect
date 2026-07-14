#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

bool report_result_json_cache_payload_valid(const uint8_t *data, size_t size);
bool report_result_plot_cache_payload_valid(const uint8_t *data, size_t size);

}  // namespace aircannect
