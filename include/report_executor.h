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
    // When non-null, progressed reports bounded sink work that happened while
    // the sink was still unable to accept another record.
    virtual bool ready(bool *progressed = nullptr) {
        if (progressed) *progressed = false;
        return true;
    }

    virtual bool accept_series(uint16_t session_index,
                               const ReportSeriesDescriptor &series,
                               const ReportSeriesSample &sample) = 0;

    // EDF-only fast path. The span is borrowed until this call returns;
    // the default preserves existing sinks by adapting valid words to the
    // sample contract. Fallback decoding continues through accept_series().
    virtual bool accept_series_span(
        uint16_t session_index,
        const ReportSeriesDescriptor &series,
        const EdfReportSeriesSpan &span) {
        if (!span.valid()) return false;
        for (uint32_t i = 0; i < span.sample_count; ++i) {
            if (span.missing_at(i)) continue;

            ReportSeriesSample sample;
            sample.timestamp_ms = span.timestamp_at(i);
            sample.value_milli = span.value_milli_at(i);
            sample.raw = span.raw_at(i);
            sample.raw_valid = true;
            if (!accept_series(session_index, series, sample)) return false;
        }
        return true;
    }
    virtual bool accept_event(uint16_t session_index,
                              const ReportEventRecord &event) = 0;

    // Polled after releasing the operation's prepared read, even if ready()
    // is false. Complete required source-boundary work; false yields, true
    // permits the next operation/source. Sinks may retain bounded buffers
    // until final publication. When non-null, progressed reports bounded
    // work completed while the source boundary is still pending. Must
    // tolerate repeated calls.
    // Cancellation/failure releases executor resources without draining.
    virtual bool end_operation(bool *progressed = nullptr) {
        if (progressed) *progressed = false;
        return true;
    }

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
    bool poll_operation_end(bool &progressed);
    void finish(ReportExecutorState state, ReportExecutorError error);
    void release_run_resources();
    void release_prepared();
    void free_scratch();

    static bool emit_series(void *context,
                            const ReportSeriesSample &sample);
    static bool emit_series_span(void *context,
                                 const EdfReportSeriesSpan &span);
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
    const uint8_t *prepared_view_data_ = nullptr;
    size_t prepared_view_length_ = 0;

    // Fallback payload scratch; EDF records borrow prepared storage directly.
    uint8_t *fallback_buffer_ = nullptr;
    size_t fallback_capacity_ = 0;
    EdfReportSeriesDecoder *decoders_ = nullptr;
    size_t decoder_capacity_ = 0;
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
