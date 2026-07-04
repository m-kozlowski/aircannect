#include "report_summary_status_store.h"

namespace aircannect {

ReportSummaryStatusStore::~ReportSummaryStatusStore() {
    release();
}

void ReportSummaryStatusStore::begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
}

void ReportSummaryStatusStore::release() {
    if (!lock_) return;

    vSemaphoreDelete(lock_);
    lock_ = nullptr;
}

bool ReportSummaryStatusStore::take(TickType_t timeout) const {
    return !lock_ || xSemaphoreTake(lock_, timeout) == pdTRUE;
}

void ReportSummaryStatusStore::give() const {
    if (lock_) xSemaphoreGive(lock_);
}

void ReportSummaryStatusStore::reset_status() {
    status_ = {};
}

void ReportSummaryStatusStore::publish_revision() {
    revision_pub_.store(status_.revision);
}

uint32_t ReportSummaryStatusStore::revision() const {
    return revision_pub_.load();
}

}  // namespace aircannect
