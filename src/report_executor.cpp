#include "report_executor.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <utility>

#include "board_report.h"
#include "memory_manager.h"
#include "report_records.h"

namespace aircannect {
namespace {

bool source_kind(ReportReadOperationKind operation_kind,
                 NightCatalogFileKind file_kind,
                 EdfInventoryFileKind &out) {
    if (operation_kind == ReportReadOperationKind::ScoredEvents &&
        file_kind == NightCatalogFileKind::Eve) {
        out = EdfInventoryFileKind::Eve;
        return true;
    }
    if (operation_kind == ReportReadOperationKind::CsrEvents &&
        file_kind == NightCatalogFileKind::Csl) {
        out = EdfInventoryFileKind::Csl;
        return true;
    }
    return false;
}

bool fallback_kind(ReportReadOperationKind kind) {
    return kind == ReportReadOperationKind::FallbackSeries ||
           kind == ReportReadOperationKind::FallbackEvents;
}

}  // namespace

bool ReportExecutorStatus::active() const {
    return state == ReportExecutorState::SubmitRead ||
           state == ReportExecutorState::WaitRead ||
           state == ReportExecutorState::DecodeRecords ||
           state == ReportExecutorState::FinishingOperation;
}

bool ReportExecutorStatus::terminal() const {
    return state == ReportExecutorState::Complete ||
           state == ReportExecutorState::Failed ||
           state == ReportExecutorState::Cancelled;
}

ReportExecutor::~ReportExecutor() {
    reset();
}

void ReportExecutor::begin(StorageReadPort &read_port) {
    reset();
    read_port_ = &read_port;
}

OperationAdmission ReportExecutor::start(
    std::shared_ptr<const ReportReadPlan> plan,
    ReportExecutionSink &sink,
    uint32_t generation) {
    if (status().active()) return OperationAdmission::Busy;

    reset();
    if (!read_port_ || !plan || generation == 0) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidArgument);
        return OperationAdmission::Rejected;
    }

    plan_ = std::move(plan);
    sink_ = &sink;
    generation_ = generation;
    operation_count_ = plan_->operation_count();
    if (operation_count_ == 0) {
        finish(ReportExecutorState::Complete, ReportExecutorError::None);
        return OperationAdmission::Accepted;
    }

    if (!allocate_scratch(plan_->fallback_read_capacity(),
                          plan_->decoder_capacity())) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::AllocationFailed);
        return OperationAdmission::Rejected;
    }

    state_ = ReportExecutorState::SubmitRead;
    return OperationAdmission::Accepted;
}

bool ReportExecutor::poll(size_t record_budget) {
    bool progressed = false;
    while (status().active()) {
        if (state_ == ReportExecutorState::SubmitRead) {
            if (!submit_read()) break;
            progressed = true;
            continue;
        }
        if (state_ == ReportExecutorState::WaitRead) {
            if (!poll_read()) break;
            progressed = true;
            continue;
        }
        if (state_ == ReportExecutorState::FinishingOperation) {
            bool sink_progressed = false;
            if (!poll_operation_end(sink_progressed)) {
                progressed = sink_progressed || progressed;
                break;
            }
            progressed = true;
            continue;
        }

        if (state_ != ReportExecutorState::DecodeRecords ||
            record_budget == 0) {
            break;
        }

        bool sink_progressed = false;
        if (!sink_->ready(&sink_progressed)) {
            progressed = sink_progressed || progressed;
            if (sink_->failure_reason()) {
                finish(ReportExecutorState::Failed,
                       ReportExecutorError::SinkRejected);
                progressed = true;
            }
            break;
        }

        const ReportReadOperation *operation = operation_context_.prepared
            ? operation_context_.operation
            : plan_->operation(operation_index_);
        const bool fallback = operation && fallback_kind(operation->kind);
        if (!decode_record()) break;
        progressed = true;
        record_budget = fallback ? 0 : record_budget - 1;
    }
    return progressed;
}

void ReportExecutor::cancel() {
    if (!status().active()) return;
    finish(ReportExecutorState::Cancelled, ReportExecutorError::None);
}

void ReportExecutor::reset() {
    release_run_resources();
    state_ = ReportExecutorState::Idle;
    error_ = ReportExecutorError::None;
    storage_error_[0] = '\0';
    generation_ = 0;
    operation_index_ = 0;
    operation_count_ = 0;
    record_index_ = 0;
}

ReportExecutorStatus ReportExecutor::status() const {
    ReportExecutorStatus out;
    out.state = state_;
    out.error = error_;
    out.generation = generation_;
    out.operation_index = operation_index_;
    out.operation_count = operation_count_;
    out.record_index = record_index_;
    const ReportReadOperation *operation =
        plan_ && operation_index_ < operation_count_
            ? plan_->operation(operation_index_)
            : nullptr;
    out.record_count = operation ? operation->record_count : 0;
    return out;
}

bool ReportExecutor::allocate_scratch(size_t record_capacity,
                                      size_t decoder_capacity) {
    if (record_capacity > 0) {
        fallback_buffer_ = static_cast<uint8_t *>(
            Memory::calloc_large(record_capacity, 1, false));
        if (!fallback_buffer_) return false;
        fallback_capacity_ = record_capacity;
    }

    if (decoder_capacity == 0) return true;
    decoders_ = static_cast<EdfReportSeriesDecoder *>(
        Memory::calloc_large(decoder_capacity,
                             sizeof(EdfReportSeriesDecoder),
                             false));
    if (!decoders_) return false;
    decoder_capacity_ = decoder_capacity;
    for (size_t i = 0; i < decoder_capacity_; ++i) {
        new (&decoders_[i]) EdfReportSeriesDecoder();
    }
    return true;
}

bool ReportExecutor::submit_read() {
    if (!prepare_operation()) {
        finish(ReportExecutorState::Failed,
               sink_rejected_ ? ReportExecutorError::SinkRejected
                              : ReportExecutorError::InvalidPlan);
        return true;
    }

    const ReportReadOperation *operation = operation_context_.operation;
    const char *path = operation_context_.path;
    if (!operation || !path) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return true;
    }

    StorageReadCommand command;
    command.path = path;
    command.offset = operation->offset;
    command.length = operation->length;
    command.lane = StorageReadLane::Report;
    command.generation = generation_;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageRejected);
        return true;
    }

    ticket_ = submission.ticket;
    state_ = ReportExecutorState::WaitRead;
    return true;
}

bool ReportExecutor::poll_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(ticket_, completion)) return false;
    ticket_ = {};

    if (completion.outcome.disposition == OperationDisposition::Cancelled) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        finish(ReportExecutorState::Cancelled, ReportExecutorError::None);
        return true;
    }
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid()) {
        snprintf(storage_error_, sizeof(storage_error_), "%s", completion.error);
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageFailed);
        return true;
    }

    const ReportReadOperation *operation = operation_context_.operation;
    prepared_ = completion.prepared;
    if (!operation || prepared_.length != operation->length) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageShortRead);
        return true;
    }
    state_ = ReportExecutorState::DecodeRecords;
    return true;
}

bool ReportExecutor::prepare_operation() {
    if (operation_context_.prepared) return true;

    OperationContext context;
    const ReportReadOperation *operation =
        plan_ ? plan_->operation(operation_index_) : nullptr;
    if (!operation) return false;

    context.operation = operation;
    context.path = plan_->source_path(*operation);
    if (!context.path || !context.path[0]) return false;

    context.mappings = plan_->mappings(*operation, context.mapping_count);
    if (context.mapping_count > 0 && !context.mappings) return false;

    record_index_ = 0;
    fallback_loaded_ = false;
    if (fallback_kind(operation->kind)) {
        context.fallback_file = plan_->fallback_file(*operation);
        context.fallback_section = plan_->fallback_section(*operation);
        if (!context.fallback_file || !context.fallback_section) return false;

        if (operation->kind == ReportReadOperationKind::FallbackSeries) {
            if (context.mapping_count != 1) return false;
        } else if (context.mapping_count != 0) {
            return false;
        }

        context.prepared = true;
        operation_context_ = context;
        return true;
    }

    context.source_file = plan_->source_file(*operation);
    if (!context.source_file) return false;
    const NightCatalogSourceFile &file = *context.source_file;

    if (operation->kind == ReportReadOperationKind::Numeric) {
        if (context.mapping_count == 0 ||
            context.mapping_count > decoder_capacity_) {
            return false;
        }

        for (size_t i = 0; i < context.mapping_count; ++i) {
            decoders_[i] = EdfReportSeriesDecoder(
                context.mappings[i].layout,
                file.record_start_ms,
                file.record_duration_ms,
                file.record_size,
                file.complete_records,
                context.mappings[i].output_window.start_ms,
                context.mappings[i].output_window.end_ms);
            if (!sink_->configure_series(context.mappings[i].series,
                                         decoders_[i].signal_scale)) {
                sink_rejected_ = true;
                return false;
            }
        }
        context.prepared = true;
        operation_context_ = context;
        return true;
    }

    if (context.mapping_count != 0) return false;
    if (!source_kind(operation->kind,
                     file.kind,
                     context.event_source.kind)) {
        return false;
    }
    context.event_source.header_start_ms = file.record_start_ms;
    context.event_source_valid = true;

    if (operation->kind == ReportReadOperationKind::CsrEvents) {
        const bool contiguous = event_context_valid_ &&
            event_file_index_ == operation->catalog_file_index &&
            event_next_record_ == operation->first_record;
        if (!contiguous) event_context_ = {};
        event_context_valid_ = true;
        event_file_index_ = operation->catalog_file_index;
    } else {
        event_context_ = {};
        event_context_valid_ = false;
        event_file_index_ = UINT16_MAX;
        event_next_record_ = 0;
    }

    context.prepared = true;
    operation_context_ = context;
    return true;
}

bool ReportExecutor::decode_record() {
    const OperationContext &operation_context = operation_context_;
    const ReportReadOperation *operation = operation_context.operation;
    if (!operation_context.prepared || !operation) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return false;
    }
    if (fallback_kind(operation->kind)) {
        return decode_fallback_operation();
    }

    const NightCatalogSourceFile *file = operation_context.source_file;
    if (!operation || !file || record_index_ >= operation->record_count) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return false;
    }

    const size_t prepared_offset =
        static_cast<size_t>(record_index_) * file->record_size;
    if (!prepared_view_data_) {
        const StoragePreparedReadView view =
            read_port_->view_prepared(prepared_);
        if (view.state == PreparedByteReadState::Retry) return false;
        if (view.state != PreparedByteReadState::Data || !view.data) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::StorageShortRead);
            return false;
        }
        prepared_view_data_ = view.data;
        prepared_view_length_ = view.length;
    }
    if (prepared_view_length_ != prepared_.length ||
        prepared_offset > prepared_view_length_ ||
        file->record_size > prepared_view_length_ - prepared_offset) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageShortRead);
        return false;
    }

    const uint8_t *record = prepared_view_data_ + prepared_offset;
    sink_rejected_ = false;
    callback_operation_ = operation;
    const uint32_t source_record_index =
        operation->first_record + record_index_;
    if (operation->kind == ReportReadOperationKind::Numeric) {
        for (size_t i = 0; i < operation_context.mapping_count; ++i) {
            const ReportReadMapping *mappings = operation_context.mappings;
            callback_mapping_ = &mappings[i];
            const EdfReportSeriesStatus decode_status =
                edf_report_decode_series_record_spans(
                    decoders_[i],
                    record,
                    file->record_size,
                    source_record_index,
                    emit_series_span,
                    this);
            if (decode_status != EdfReportSeriesStatus::Ok) {
                finish(ReportExecutorState::Failed,
                       sink_rejected_
                           ? ReportExecutorError::SinkRejected
                           : ReportExecutorError::DecodeFailed);
                return false;
            }
        }
    } else {
        if (!operation_context.event_source_valid) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::InvalidPlan);
            return false;
        }

        EdfReportEventDecodeContext *event_context =
            operation->kind == ReportReadOperationKind::CsrEvents
                ? &event_context_
                : nullptr;
        const EdfReportEventStatus decode_status =
            edf_report_decode_annotation_record(operation_context.event_source,
                                                record,
                                                file->record_size,
                                                true,
                                                emit_event,
                                                this,
                                                event_context);
        if (decode_status != EdfReportEventStatus::Ok) {
            finish(ReportExecutorState::Failed,
                   sink_rejected_
                       ? ReportExecutorError::SinkRejected
                       : ReportExecutorError::DecodeFailed);
            return false;
        }
    }

    callback_mapping_ = nullptr;
    callback_operation_ = nullptr;
    ++record_index_;
    if (record_index_ == operation->record_count) finish_operation();
    return true;
}

bool ReportExecutor::decode_fallback_operation() {
    const OperationContext &operation_context = operation_context_;
    const ReportReadOperation *operation = operation_context.operation;
    const NightCatalogFallbackFile *file = operation_context.fallback_file;
    const NightCatalogFallbackSection *section =
        operation_context.fallback_section;
    if (!operation_context.prepared || !operation || !file || !section ||
        record_index_ >= operation->record_count ||
        operation->length > fallback_capacity_) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return false;
    }

    if (!fallback_loaded_) {
        const PreparedByteRead read = read_port_->read_prepared(
            prepared_, 0, fallback_buffer_, operation->length);

        if (read.state == PreparedByteReadState::Retry) return false;
        if (read.state != PreparedByteReadState::Data ||
            read.bytes != operation->length) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::StorageShortRead);
            return false;
        }
        fallback_loaded_ = true;
    }

    const uint8_t *data = fallback_buffer_;
    const size_t data_size = operation->length;
    const uint32_t batch_count = std::min<uint32_t>(
        32, operation->record_count - record_index_);

    sink_rejected_ = false;
    callback_operation_ = operation;
    if (operation->kind == ReportReadOperationKind::FallbackSeries) {
        callback_mapping_ = operation_context.mappings;
        const bool decoded = report_for_each_series_sample_range(
            section->payload_schema,
            section->coverage.start_ms,
            data,
            data_size,
            section->record_count,
            operation->first_record + record_index_,
            batch_count,
            emit_series,
            this);
        if (!decoded) {
            finish(ReportExecutorState::Failed,
                   sink_rejected_
                       ? ReportExecutorError::SinkRejected
                       : ReportExecutorError::DecodeFailed);
            return false;
        }
    } else {
        for (uint32_t i = record_index_;
             i < record_index_ + batch_count;
             ++i) {
            ReportEventRecord event;
            if (!report_read_event_record(data, data_size, i,
                                          event) ||
                !report_adjust_event_time(event, file->time_adjust_ms)) {
                finish(ReportExecutorState::Failed,
                       ReportExecutorError::DecodeFailed);
                return false;
            }
            const uint8_t source_mask = report_event_source_mask(event);
            if (source_mask == 0) {
                finish(ReportExecutorState::Failed,
                       ReportExecutorError::DecodeFailed);
                return false;
            }
            if ((source_mask & operation->event_mask) != 0 &&
                !emit_event(this, event)) {
                finish(ReportExecutorState::Failed,
                       sink_rejected_
                           ? ReportExecutorError::SinkRejected
                           : ReportExecutorError::DecodeFailed);
                return false;
            }
        }
    }

    callback_mapping_ = nullptr;
    callback_operation_ = nullptr;
    record_index_ += batch_count;
    if (record_index_ == operation->record_count) finish_operation();
    return true;
}

void ReportExecutor::finish_operation() {
    const ReportReadOperation *operation = operation_context_.operation;
    if (operation && operation->kind == ReportReadOperationKind::CsrEvents) {
        event_next_record_ = operation->first_record + operation->record_count;
    }

    release_prepared();
    state_ = ReportExecutorState::FinishingOperation;
}

bool ReportExecutor::poll_operation_end(bool &progressed) {
    if (!sink_->end_operation(&progressed)) {
        if (!sink_->failure_reason()) return false;

        finish(ReportExecutorState::Failed, ReportExecutorError::SinkRejected);
        return true;
    }

    ++operation_index_;
    record_index_ = 0;
    operation_context_ = {};

    if (operation_index_ >= operation_count_) {
        finish(ReportExecutorState::Complete, ReportExecutorError::None);
    } else {
        state_ = ReportExecutorState::SubmitRead;
    }
    return true;
}

void ReportExecutor::finish(ReportExecutorState state,
                            ReportExecutorError error) {
    release_run_resources();
    state_ = state;
    error_ = error;
}

void ReportExecutor::release_run_resources() {
    if (read_port_ && ticket_.valid()) {
        (void)read_port_->abandon(ticket_);
    }
    ticket_ = {};
    release_prepared();
    free_scratch();
    plan_.reset();
    sink_ = nullptr;
    callback_mapping_ = nullptr;
    callback_operation_ = nullptr;
    operation_context_ = {};
    event_context_ = {};
    event_file_index_ = UINT16_MAX;
    event_next_record_ = 0;
    event_context_valid_ = false;
    sink_rejected_ = false;
    fallback_loaded_ = false;
}

void ReportExecutor::release_prepared() {
    const StoragePreparedRead prepared = prepared_;
    prepared_ = {};
    prepared_view_data_ = nullptr;
    prepared_view_length_ = 0;
    if (!read_port_ || !prepared.valid()) return;
    read_port_->release_prepared(prepared);
}

void ReportExecutor::free_scratch() {
    for (size_t i = 0; i < decoder_capacity_; ++i) {
        decoders_[i].~EdfReportSeriesDecoder();
    }
    Memory::free(decoders_);
    decoders_ = nullptr;
    decoder_capacity_ = 0;
    Memory::free(fallback_buffer_);
    fallback_buffer_ = nullptr;
    fallback_capacity_ = 0;
}

bool ReportExecutor::emit_series(void *context,
                                 const ReportSeriesSample &sample) {
    ReportExecutor *executor = static_cast<ReportExecutor *>(context);
    if (!executor || !executor->sink_ || !executor->callback_mapping_ ||
        !executor->callback_operation_) {
        return false;
    }
    if (sample.timestamp_ms <
            executor->callback_mapping_->output_window.start_ms ||
        sample.timestamp_ms >=
            executor->callback_mapping_->output_window.end_ms) {
        return true;
    }

    if (!executor->sink_->accept_series(
            executor->callback_operation_->session_index,
            executor->callback_mapping_->series,
            sample)) {
        executor->sink_rejected_ = true;
        return false;
    }
    return true;
}

bool ReportExecutor::emit_series_span(
    void *context,
    const EdfReportSeriesSpan &span) {
    ReportExecutor *executor = static_cast<ReportExecutor *>(context);
    if (!executor || !executor->sink_ || !executor->callback_mapping_ ||
        !executor->callback_operation_) {
        return false;
    }

    if (!executor->sink_->accept_series_span(
            executor->callback_operation_->session_index,
            executor->callback_mapping_->series,
            span)) {
        executor->sink_rejected_ = true;
        return false;
    }
    return true;
}

bool ReportExecutor::emit_event(void *context,
                                const ReportEventRecord &event) {
    ReportExecutor *executor = static_cast<ReportExecutor *>(context);
    if (!executor || !executor->sink_ || !executor->callback_operation_) {
        return false;
    }
    if (!report_event_overlaps_window(
            event,
            executor->callback_operation_->event_filter.start_ms,
            executor->callback_operation_->event_filter.end_ms)) {
        return true;
    }

    if (!executor->sink_->accept_event(
            executor->callback_operation_->session_index,
            event)) {
        executor->sink_rejected_ = true;
        return false;
    }
    return true;
}

}  // namespace aircannect
