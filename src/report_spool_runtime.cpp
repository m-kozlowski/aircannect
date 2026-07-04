#include "report_spool_runtime.h"

namespace aircannect {

void ReportSpoolRuntime::observe_idle(RpcArbiter &arbiter) {
    if (!client_.active()) pressure_.observe_idle(arbiter);
}

void ReportSpoolRuntime::log_pressure_if_changed(RpcArbiter &arbiter) {
    pressure_.log_if_changed(arbiter, client_);
}

}  // namespace aircannect
