#pragma once

#include <stdint.h>

#include "as11_device_state.h"
#include "as11_event_frame.h"
#include "report_spool_port.h"

namespace aircannect {

struct As11BleRecoveryActions {
    bool refresh = false;
    bool boot_confirmed = false;
};

class As11BleRecovery {
public:
    bool begin(ReportSpoolPort &spool);

    void observe_link(bool selected,
                      bool authenticated,
                      const As11DeviceState &device,
                      uint32_t now_ms);
    void observe_event(const As11EventFrame &frame);
    void poll(uint32_t now_ms);

    As11BleRecoveryActions take_actions();

private:
    enum class FetchState : uint8_t {
        Idle,
        AwaitingSubmission,
        Fetching,
        Cancelling,
    };

    // Link lifecycle
    void select_transport(bool selected);
    void note_disconnect(const As11DeviceState &device, uint32_t now_ms);
    void note_reconnect(uint32_t now_ms);
    void capture_disconnect_boundary(const As11DeviceState &device,
                                     uint32_t now_ms);

    // Reboot classification
    void submit_fetch(uint32_t now_ms);
    void consume_fetch_completion();
    void classify_result(ReportSpoolResult &result);
    void confirm_boot(int64_t event_ms);
    void finish_classification();
    void cancel_fetch();

    uint32_t next_generation();

    ReportSpoolPort *spool_ = nullptr;

    bool selected_ = false;
    bool link_state_known_ = false;
    bool authenticated_ = false;
    bool ready_seen_ = false;
    bool disconnected_ = false;

    bool classification_pending_ = false;
    bool disconnect_boundary_valid_ = false;
    int64_t disconnect_boundary_device_ms_ = 0;
    int64_t latest_activity_device_ms_ = 0;
    uint32_t submit_due_ms_ = 0;
    uint32_t submit_deadline_ms_ = 0;

    FetchState fetch_state_ = FetchState::Idle;
    OperationTicket fetch_ticket_;
    uint32_t generation_ = 0;

    As11BleRecoveryActions actions_;
};

}  // namespace aircannect
