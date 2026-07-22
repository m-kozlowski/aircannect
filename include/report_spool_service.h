#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_spool_port.h"
#include "report_spool_runtime.h"
#include "rpc_request_port.h"

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#endif

namespace aircannect {

class ReportSpoolService final : public ReportSpoolPort {
public:
    explicit ReportSpoolService(RpcRequestPort &rpc) : runtime_(rpc) {}
    ~ReportSpoolService() override;

    ReportSpoolService(const ReportSpoolService &) = delete;
    ReportSpoolService &operator=(const ReportSpoolService &) = delete;

    bool begin();

    OperationSubmission request_fetch(
        const ReportSpoolFetchCommand &command) override;
    bool cancel(OperationTicket ticket) override;
    bool take_round(OperationTicket ticket,
                    ReportSpoolFetchRound &round) override;
    bool take_completion(
        OperationTicket ticket,
        ReportSpoolFetchCompletion &completion) override;

    bool enqueue_notification(const char *payload, size_t payload_len);
    bool poll(bool transport_backpressure_active,
              uint32_t rx_queue_full_alerts);
    bool active() const;

private:
    bool lock(uint32_t timeout_ms = 10) const;
    void unlock() const;

    bool take_queued(ReportSpoolFetchCommand &command,
                     OperationTicket &ticket);
    bool take_cancel_request(OperationTicket &ticket);
    bool round_waiting() const;
    bool publish_completed_round();
    void clear_published_round(OperationTicket ticket);
    void publish_completion(OperationTicket ticket,
                            OperationOutcome outcome,
                            ReportSpoolResult *result,
                            const char *error);

    ReportSpoolRuntime runtime_;
    ReportSpoolFetchCommand queued_command_;
    OperationTicket queued_ticket_;
    OperationTicket active_ticket_;
    OperationTicket cancel_ticket_;
    ReportSpoolFetchRound round_;
    ReportSpoolFetchCompletion completion_;
    uint32_t next_ticket_id_ = 1;
    bool queued_ = false;
    bool round_ready_ = false;
    bool completion_ready_ = false;
    bool initialized_ = false;

#ifdef ARDUINO
    mutable SemaphoreHandle_t mutex_ = nullptr;
#endif
};

}  // namespace aircannect
