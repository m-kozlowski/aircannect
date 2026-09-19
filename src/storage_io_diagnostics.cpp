#include "storage_io_diagnostics.h"

#include <errno.h>

#include "debug_log.h"

namespace aircannect::Storage {

void log_io_error(const char *operation, const char *path, int error,
                  size_t actual, size_t requested) {
    if (error == ENOENT || error == ENOTDIR || error == EEXIST) return;

    const int saved_errno = errno;
    static bool reported = false;
    static uint32_t last_report_ms = 0;
    static uint32_t suppressed = 0;
    const uint32_t now = millis();

    if (reported && static_cast<uint32_t>(now - last_report_ms) < 1000) {
        ++suppressed;
        errno = saved_errno;
        return;
    }

    Log::logf_without_file(CAT_STORAGE, LOG_ERROR,
              "I/O op=%s errno=%d bytes=%u/%u suppressed=%lu path=%s\n",
              operation, error, static_cast<unsigned>(actual),
              static_cast<unsigned>(requested),
              static_cast<unsigned long>(suppressed), path ? path : "--");

    reported = true;
    last_report_ms = now;
    suppressed = 0;
    errno = saved_errno;
}

}  // namespace aircannect::Storage
