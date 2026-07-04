#include "report_diagnostics.h"

#include "debug_log.h"

namespace aircannect {

void log_report_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_REPORT,
              LOG_ERROR,
              "allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

}  // namespace aircannect
