#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_event_reader.h"
#include "edf_report_series_reader.h"
#include "large_byte_buffer.h"
#include "report_read_plan.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportExecutorState : uint8_t {
    Idle,
    SubmitRead,
    WaitRead,
    DecodeRecords,
    Complete,
    Failed,
    Cancelled,
    FinishingOperation,
};

enum class ReportExecutorError : uint8_t {
    None,
    InvalidArgument,
    InvalidPlan,
    AllocationFailed,
    StorageRejected,
    StorageFailed,
    StorageShortRead,
    DecodeFailed,
    SinkRejected,
};

struct ReportExecutorStatus {
    ReportExecutorState state = ReportExecutorState::Idle;
    ReportExecutorError error = ReportExecutorError::None;
    uint32_t generation = 0;
    size_t operation_index = 0;
    size_t operation_count = 0;
    uint32_t record_index = 0;
    uint32_t record_count = 0;

    bool active() const;
    bool terminal() const;
};

class ReportExecutionSink {
public:
    virtual ~ReportExecutionSink() = default;

    // Called once per EDF numeric mapping before its operation emits samples.
    // Scale is in original source units; the sink owns canonical conversion
    // of both scale and offset. False rejects the operation (not a retry).
    // Fallback milli-value samples have raw_valid=false and no EDF scale.
    virtual bool configure_series(const ReportSeriesDescriptor &,
                                  const EdfSignalScale &) { return true; }

    // Polled before each complete EDF record or fallback batch. False yields
    // without consuming input. True admits the entire record, all mappings.
    virtual bool ready() { return true; }

    virtual bool accept_series(uint16_t session_index,
                               const ReportSeriesDescriptor &series,
                               const ReportSeriesSample &sample) = 0;
    virtual bool accept_event(uint16_t session_index,
                              const ReportEventRecord &event) = 0;

    // Polled after releasing the operation's prepared read, even if ready()
    // is false. Complete required source-boundary work; false yields, true
    // permits the next operation/source. Sinks may retain bounded buffers
    // until final publication. Must tolerate repeated calls.
    // Cancellation/failure releases executor resources without draining.
    virtual bool end_operation() { return true; }

    // Non-null distinguishes a failed ready()/end_operation() from a wait.
    virtual const char *failure_reason() const { return nullptr; }
};

class ReportExecutor {
public:
    ReportExecutor() = default;
    ~ReportExecutor();

    ReportExecutor(const ReportExecutor &) = delete;
    ReportExecutor &operator=(const ReportExecutor &) = delete;

    void begin(StorageReadPort &read_port);
    OperationAdmission start(std::shared_ptr<const ReportReadPlan> plan,
                             ReportExecutionSink &sink,
                             uint32_t generation);
    // Fallback decoding consumes at most 32 sample/event positions per poll,
    // regardless of record_budget. A zero budget still polls operation end.
    bool poll(size_t record_budget = 1);
    void cancel();
    void reset();

    ReportExecutorStatus status() const;

private:
    bool validate_plan(size_t &record_capacity,
                       size_t &decoder_capacity) const;
    bool allocate_scratch(size_t record_capacity,
                          size_t decoder_capacity);
    bool submit_read();
    bool poll_read();
    bool prepare_operation();
    bool decode_record();
    bool decode_fallback_operation();
    void finish_operation();
    bool poll_operation_end();
    void finish(ReportExecutorState state, ReportExecutorError error);
    void release_run_resources();
    void release_prepared();
    void free_scratch();

    static bool emit_series(void *context,
                            const ReportSeriesSample &sample);
    static bool emit_event(void *context,
                           const ReportEventRecord &event);

    StorageReadPort *read_port_ = nullptr;
    std::shared_ptr<const ReportReadPlan> plan_;
    ReportExecutionSink *sink_ = nullptr;

    ReportExecutorState state_ = ReportExecutorState::Idle;
    ReportExecutorError error_ = ReportExecutorError::None;
    uint32_t generation_ = 0;
    size_t operation_index_ = 0;
    size_t operation_count_ = 0;
    uint32_t record_index_ = 0;

    OperationTicket ticket_;
    StoragePreparedRead prepared_;

    uint8_t *record_buffer_ = nullptr;
    size_t record_capacity_ = 0;
    EdfReportSeriesDecoder *decoders_ = nullptr;
    size_t decoder_capacity_ = 0;
    size_t decoder_count_ = 0;
    bool fallback_loaded_ = false;

    EdfReportEventDecodeContext event_context_;
    uint16_t event_file_index_ = UINT16_MAX;
    uint32_t event_next_record_ = 0;
    bool event_context_valid_ = false;
    bool sink_rejected_ = false;
    const ReportReadMapping *callback_mapping_ = nullptr;
    const ReportReadOperation *callback_operation_ = nullptr;
};

}  // namespace aircannect
