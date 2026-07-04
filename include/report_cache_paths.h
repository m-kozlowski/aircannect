#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr const char *REPORT_CACHE_BASE_DIR = "/aircannect/report/v5";
static constexpr const char *REPORT_PLOT_CACHE_DIR =
    "/aircannect/report/v4/plots/v2";
static constexpr const char *REPORT_RESULT_JSON_CACHE_DIR =
    "/aircannect/report/v4/results/v1";
static constexpr size_t REPORT_CACHE_PATH_MAX = 192;
static constexpr size_t REPORT_RESULT_JSON_CACHE_MAX = 32 * 1024;

bool cache_child_path(const char *dir,
                      const char *child_name,
                      char *out,
                      size_t out_size);
bool plot_cache_name_for_night(const char *name, uint64_t night_start_ms);

}  // namespace aircannect
