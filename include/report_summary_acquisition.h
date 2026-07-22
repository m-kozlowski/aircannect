#pragma once

#include <memory>
#include <stdint.h>

#include "night_catalog_summary_snapshot.h"
#include "operation_outcome.h"
#include "report_spool_port.h"

namespace aircannect {

enum class ReportSummaryAcquisitionState : uint8_t {
    Idle,
    Waiting,
    Ready,
    Error,
};

struct ReportSummaryAcquisitionStatus {
    ReportSummaryAcquisitionState state =
        ReportSummaryAcquisitionState::Idle;
    uint32_t generation = 0;
    size_t records = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class ReportSummaryAcquisition {
public:
    void begin(ReportSpoolPort &spool_port);

    OperationAdmission request(uint32_t generation);
    bool poll();
    void cancel();

    bool active() const {
        return status_.state == ReportSummaryAcquisitionState::Waiting;
    }
    const ReportSummaryAcquisitionStatus &status() const { return status_; }
    std::shared_ptr<const NightCatalogSummarySnapshot> snapshot() const {
        return snapshot_;
    }
    void seed(std::shared_ptr<const NightCatalogSummarySnapshot> snapshot);

private:
    void fail(const char *error);

    ReportSpoolPort *spool_port_ = nullptr;
    OperationTicket ticket_;
    ReportSummaryAcquisitionStatus status_;
    std::shared_ptr<const NightCatalogSummarySnapshot> snapshot_;
    bool cancel_requested_ = false;
};

}  // namespace aircannect
