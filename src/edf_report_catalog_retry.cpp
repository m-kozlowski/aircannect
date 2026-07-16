#include "edf_report_catalog_retry.h"

#include <stddef.h>
#include <string.h>

namespace aircannect {
namespace {

static constexpr uint32_t RETRY_DELAYS_MS[] = {
    2000,
    5000,
    10000,
    30000,
    60000,
};

bool retryable_error(const char *error) {
    return error &&
        (strcmp(error, "storage_unavailable") == 0 ||
         strcmp(error, "alloc_failed") == 0);
}

}  // namespace

bool EdfReportCatalogRetry::schedule_for_error(const char *error,
                                                uint32_t now_ms) {
    if (!retryable_error(error)) {
        reset();
        return false;
    }

    if (attempts_ < 255) attempts_++;
    const size_t delay_count =
        sizeof(RETRY_DELAYS_MS) / sizeof(RETRY_DELAYS_MS[0]);
    const size_t delay_index = attempts_ < delay_count
        ? attempts_ - 1
        : delay_count - 1;
    due_ms_ = now_ms + RETRY_DELAYS_MS[delay_index];
    if (due_ms_ == 0) due_ms_ = 1;
    return true;
}

void EdfReportCatalogRetry::mark_started() {
    due_ms_ = 0;
}

void EdfReportCatalogRetry::reset() {
    due_ms_ = 0;
    attempts_ = 0;
}

bool EdfReportCatalogRetry::due(uint32_t now_ms) const {
    return due_ms_ != 0 &&
           static_cast<int32_t>(now_ms - due_ms_) >= 0;
}

uint32_t EdfReportCatalogRetry::remaining_ms(uint32_t now_ms) const {
    if (!pending() || due(now_ms)) return 0;
    return due_ms_ - now_ms;
}

}  // namespace aircannect
