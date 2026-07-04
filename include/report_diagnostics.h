#pragma once

#include <stddef.h>

namespace aircannect {

void log_report_alloc_failed(const char *context, size_t bytes);

}  // namespace aircannect
