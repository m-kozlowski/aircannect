#pragma once

#include "as11_clock.h"
#include "edf_str_session.h"
#include "operation_outcome.h"
#include "rpc_request_port.h"
#include "sleep_day_id.h"
#include "storage_atomic_write_port.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

enum class AirMiniHistoryPhase : uint8_t {
    Idle,
    Waiting,
    Settings,
    LoggedData,
    ReadingSaved,
    ReadingEdf,
    Saving,
    Finalizing,
    ReadingReport,
    BuildingReport,
    SavingReport,
    RecordReady,
    Finishing,
    Complete,
    Failed,
};

struct AirMiniHistoryStatus {
    AirMiniHistoryPhase phase = AirMiniHistoryPhase::Idle;
    uint32_t generation = 0;
    SleepDayId start_day;
    SleepDayId end_day;
    SleepDayId current_day;
    uint32_t records_queued = 0;
    uint32_t empty = 0;
    bool transfer_complete = false;
    char error[80] = {};

    bool active() const {
        return phase >= AirMiniHistoryPhase::Waiting &&
               phase <= AirMiniHistoryPhase::Finishing;
    }
};

// Recorder-owned metadata acquisition. The existing RPC and storage owners
// execute requests; this service never opens files or owns a CAN connection.
class AirMiniHistoryService {
public:
    explicit AirMiniHistoryService(RpcRequestPort &rpc) : rpc_(rpc) {}
    ~AirMiniHistoryService();

    void begin(StorageReadPort &read, StorageAtomicWritePort &write,
               StorageScanPort &scan);
    OperationAdmission request(SleepDayId start, SleepDayId end,
                               uint32_t generation, uint32_t now_ms,
                               uint32_t delay_ms,
                               int32_t timezone_offset_minutes,
                               const As11ClockTransform &clock = {});
    void poll(uint32_t now_ms, bool rpc_available, bool therapy_active);
    void cancel(const char *reason);
    void enqueue_notification(const RpcPayloadRef &payload);

    const AirMiniHistoryStatus &status() const { return status_; }
    const EdfStrSessionAccumulator *record() const;
    void record_published();

private:
    struct Runtime;

    void finish(const char *error);
    void poll_finish();
    void cancel_rpc();
    void start_transfer(uint32_t now_ms);
    void poll_transfer(uint32_t now_ms);
    void start_day();
    void poll_day();
    void poll_report();
    bool poll_publication();

    RpcRequestPort &rpc_;
    StorageReadPort *read_ = nullptr;
    StorageAtomicWritePort *write_ = nullptr;
    StorageScanPort *scan_ = nullptr;
    Runtime *runtime_ = nullptr;
    AirMiniHistoryStatus status_;
};

const char *airmini_history_phase_name(AirMiniHistoryPhase phase);

}  // namespace aircannect
