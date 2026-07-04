#pragma once

#include <stdint.h>

#include "large_text_buffer.h"
#include "report_spool_types.h"

namespace aircannect {

bool result_plot_cache_exists_for_etag(uint64_t night_start_ms,
                                       const char *etag);
bool load_result_json_cache_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     LargeTextBuffer &out);
bool load_result_json_cache_path(const char *path, LargeTextBuffer &out);
bool load_result_plot_cache_for_etag(uint64_t night_start_ms,
                                     const char *etag,
                                     ReportSpoolBuffer &out);
bool load_result_plot_cache_path(const char *path, ReportSpoolBuffer &out);

}  // namespace aircannect
