#include "psu_can_identity.h"

#include "can_rpc_link.h"
#include "debug_log.h"

namespace aircannect {

void PsuCanIdentity::set_enabled(bool enabled) {
    if (enabled == enabled_) return;

    enabled_ = enabled;
    pending_ = enabled;
    failure_logged_ = false;
}

void PsuCanIdentity::note_device_boot() {
    pending_ = enabled_;
    failure_logged_ = false;
}

void PsuCanIdentity::poll(bool suspended) {
    if (!pending_ || suspended) return;

    // Air11 identifies this 16-byte payload as a 90 W AC supply.
    static constexpr char identity[16] = {};
    const RpcLinkSendResult result = can_.send_datagram(
        RpcPayloadView(identity, sizeof(identity)), 0x259);

    if (result == RpcLinkSendResult::Accepted) {
        pending_ = false;
        failure_logged_ = false;
        Log::logf(CAT_CAN, LOG_INFO, "[PSU] 90 W AC identification queued\n");
    } else if (result == RpcLinkSendResult::Failed && !failure_logged_) {
        failure_logged_ = true;
        Log::logf(CAT_CAN, LOG_ERROR,
                  "[PSU] identification enqueue failed; will retry\n");
    }
}

}  // namespace aircannect
