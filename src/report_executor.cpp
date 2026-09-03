#include "report_executor.h"

#include <algorithm>
#include <new>
#include <utility>

#include "board_report.h"
#include "crc32.h"
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
           state == ReportExecutorState::DecodeRecords;
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
    clear_fallback_cache();
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

    size_t record_capacity = 0;
    size_t decoder_capacity = 0;
    if (!validate_plan(record_capacity, decoder_capacity)) {
        finish(ReportExecutorState::Failed, ReportExecutorError::InvalidPlan);
        return OperationAdmission::Rejected;
    }
    if (!allocate_scratch(record_capacity, decoder_capacity)) {
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
        if (state_ != ReportExecutorState::DecodeRecords ||
            record_budget == 0) {
            break;
        }

        if (!decode_record()) break;
        progressed = true;
        --record_budget;
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

bool ReportExecutor::validate_plan(size_t &record_capacity,
                                   size_t &decoder_capacity) const {
    record_capacity = 0;
    decoder_capacity = 0;
    if (!plan_) return false;

    for (size_t i = 0; i < plan_->operation_count(); ++i) {
        const ReportReadOperation *operation = plan_->operation(i);
        if (!operation || operation->record_count == 0 ||
            operation->length == 0 ||
            operation->length > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
            return false;
        }

        const char *path = plan_->source_path(*operation);
        if (!path || !path[0]) return false;

        size_t mapping_count = 0;
        const ReportReadMapping *mappings =
            plan_->mappings(*operation, mapping_count);
        if (fallback_kind(operation->kind)) {
            const NightCatalogFallbackFile *file =
                plan_->fallback_file(*operation);
            const NightCatalogFallbackSection *section =
                plan_->fallback_section(*operation);
            if (!file || !section || operation->offset != section->data_offset ||
                operation->length != section->data_size ||
                operation->first_record > section->record_count ||
                operation->record_count >
                    section->record_count - operation->first_record) {
                return false;
            }

            if (operation->kind == ReportReadOperationKind::FallbackSeries) {
                if (section->kind != ReportFallbackSectionKind::Series ||
                    operation->record_count == 0 ||
                    !mappings || mapping_count != 1 ||
                    !mappings[0].output_window.valid() ||
                    mappings[0].series.signal != section->signal ||
                    mappings[0].series.source != section->source ||
                    mappings[0].series.sample_interval_ms !=
                        section->sample_interval_ms ||
                    operation->event_mask != 0) {
                    return false;
                }
            } else if (operation->first_record != 0 ||
                       operation->record_count != section->record_count ||
                       section->kind != ReportFallbackSectionKind::Events ||
                       mapping_count != 0 ||
                       !operation->event_filter.valid() ||
                       operation->event_mask == 0 ||
                       (operation->event_mask & ~section->event_mask) != 0) {
                return false;
            }

            record_capacity = std::max(
                record_capacity,
                static_cast<size_t>(operation->length));
            continue;
        }

        const NightCatalogSourceFile *file =
            plan_->source_file(*operation);
        if (!file || file->record_size == 0 ||
            operation->first_record > file->complete_records ||
            operation->record_count >
                file->complete_records - operation->first_record) {
            return false;
        }

        if (file->data_offset > UINT64_MAX - file->data_size) return false;

        const uint64_t expected_length =
            static_cast<uint64_t>(operation->record_count) *
            file->record_size;
        const uint64_t data_end = file->data_offset + file->data_size;
        if (expected_length != operation->length ||
            operation->offset < file->data_offset ||
            operation->offset > data_end ||
            operation->length > data_end - operation->offset) {
            return false;
        }

        if (operation->kind == ReportReadOperationKind::Numeric) {
            if (!mappings || mapping_count == 0 ||
                mapping_count > static_cast<size_t>(ReportSignalId::Count)) {
                return false;
            }
            for (size_t mapping_index = 0;
                 mapping_index < mapping_count;
                 ++mapping_index) {
                const EdfReportSignalLayout &layout =
                    mappings[mapping_index].layout;
                const ReportSeriesDescriptor &series =
                    mappings[mapping_index].series;
                const uint64_t signal_end =
                    static_cast<uint64_t>(layout.byte_offset_in_record) +
                    static_cast<uint64_t>(layout.samples_per_record) * 2u;
                if (!mappings[mapping_index].output_window.valid() ||
                    report_signal_bit(layout.signal) == 0 ||
                    series.signal != layout.signal ||
                    series.source != layout.source ||
                    series.sample_interval_ms != layout.sample_interval_ms ||
                    series.primary != layout.primary ||
                    layout.samples_per_record == 0 ||
                    signal_end > file->record_size) {
                    return false;
                }
            }
        } else {
            EdfInventoryFileKind ignored;
            if (mapping_count != 0 ||
                !operation->event_filter.valid() ||
                !source_kind(operation->kind, file->kind, ignored)) {
                return false;
            }
        }

        record_capacity = std::max(record_capacity,
                                   static_cast<size_t>(file->record_size));
        decoder_capacity = std::max(decoder_capacity, mapping_count);
    }
    return record_capacity > 0;
}

bool ReportExecutor::allocate_scratch(size_t record_capacity,
                                      size_t decoder_capacity) {
    record_buffer_ = static_cast<uint8_t *>(
        Memory::calloc_large(record_capacity, 1, false));
    if (!record_buffer_) return false;
    record_capacity_ = record_capacity;

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
    const ReportReadOperation *operation =
        plan_ ? plan_->operation(operation_index_) : nullptr;
    const char *path = operation ? plan_->source_path(*operation) : nullptr;
    if (!operation || !path) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return true;
    }

    if (fallback_kind(operation->kind)) {
        const NightCatalogFallbackFile *file =
            plan_->fallback_file(*operation);
        const NightCatalogFallbackSection *section =
            plan_->fallback_section(*operation);
        if (!file || !section) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::InvalidPlan);
            return true;
        }

        active_fallback_ = find_cached_fallback(*file, *section);
        if (active_fallback_) {
            if (!prepare_operation()) {
                finish(ReportExecutorState::Failed,
                       ReportExecutorError::InvalidPlan);
                return true;
            }

            state_ = ReportExecutorState::DecodeRecords;
            return true;
        }
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
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageFailed);
        return true;
    }

    const ReportReadOperation *operation = plan_->operation(operation_index_);
    prepared_ = completion.prepared;
    if (!operation || prepared_.length != operation->length) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageShortRead);
        return true;
    }
    if (!prepare_operation()) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return true;
    }

    state_ = ReportExecutorState::DecodeRecords;
    return true;
}

bool ReportExecutor::prepare_operation() {
    const ReportReadOperation *operation = plan_->operation(operation_index_);
    if (!operation) return false;

    record_index_ = 0;
    decoder_count_ = 0;
    if (fallback_kind(operation->kind)) return true;

    const NightCatalogSourceFile *file = operation
        ? plan_->source_file(*operation)
        : nullptr;
    if (!file || file->record_size > record_capacity_) {
        return false;
    }

    if (operation->kind == ReportReadOperationKind::Numeric) {
        size_t mapping_count = 0;
        const ReportReadMapping *mappings =
            plan_->mappings(*operation, mapping_count);
        if (!mappings || mapping_count > decoder_capacity_) return false;

        for (size_t i = 0; i < mapping_count; ++i) {
            if (edf_report_series_decoder_init(
                    mappings[i].layout,
                    file->record_start_ms,
                    file->record_duration_ms,
                    file->record_size,
                    file->complete_records,
                    decoders_[i]) != EdfReportSeriesStatus::Ok) {
                return false;
            }
        }
        decoder_count_ = mapping_count;
        return true;
    }

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
    return true;
}

bool ReportExecutor::decode_record() {
    const ReportReadOperation *operation = plan_->operation(operation_index_);
    if (operation && fallback_kind(operation->kind)) {
        return decode_fallback_operation();
    }

    const NightCatalogSourceFile *file = operation
        ? plan_->source_file(*operation)
        : nullptr;
    if (!operation || !file || record_index_ >= operation->record_count) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return false;
    }

    const size_t prepared_offset =
        static_cast<size_t>(record_index_) * file->record_size;
    const PreparedByteRead read = read_port_->read_prepared(
        prepared_, prepared_offset, record_buffer_, file->record_size);
    if (read.state == PreparedByteReadState::Retry) return false;
    if (read.state != PreparedByteReadState::Data ||
        read.bytes != file->record_size) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::StorageShortRead);
        return false;
    }
    sink_rejected_ = false;
    callback_operation_ = operation;
    const uint32_t source_record_index =
        operation->first_record + record_index_;
    if (operation->kind == ReportReadOperationKind::Numeric) {
        size_t mapping_count = 0;
        const ReportReadMapping *mappings =
            plan_->mappings(*operation, mapping_count);
        for (size_t i = 0; i < mapping_count; ++i) {
            callback_mapping_ = &mappings[i];
            const EdfReportSeriesStatus decode_status =
                edf_report_decode_series_record(
                    decoders_[i],
                    record_buffer_,
                    file->record_size,
                    source_record_index,
                    mappings[i].output_window.start_ms,
                    mappings[i].output_window.end_ms,
                    emit_series,
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
        EdfInventoryFileKind kind;
        if (!source_kind(operation->kind, file->kind, kind)) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::InvalidPlan);
            return false;
        }

        const EdfReportEventSource source{kind, file->record_start_ms};
        EdfReportEventDecodeContext *event_context =
            operation->kind == ReportReadOperationKind::CsrEvents
                ? &event_context_
                : nullptr;
        const EdfReportEventStatus decode_status =
            edf_report_decode_annotation_record(source,
                                                record_buffer_,
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
    const ReportReadOperation *operation = plan_->operation(operation_index_);
    const NightCatalogFallbackFile *file = operation
        ? plan_->fallback_file(*operation)
        : nullptr;
    const NightCatalogFallbackSection *section = operation
        ? plan_->fallback_section(*operation)
        : nullptr;
    if (!operation || !file || !section || record_index_ != 0 ||
        operation->length > record_capacity_) {
        finish(ReportExecutorState::Failed,
               ReportExecutorError::InvalidPlan);
        return false;
    }

    const uint8_t *data = nullptr;
    size_t data_size = 0;
    if (active_fallback_) {
        data = active_fallback_->data();
        data_size = active_fallback_->size();
    } else {
        const PreparedByteRead read = read_port_->read_prepared(
            prepared_, 0, record_buffer_, operation->length);
        if (read.state == PreparedByteReadState::Retry) return false;
        if (read.state != PreparedByteReadState::Data ||
            read.bytes != operation->length) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::StorageShortRead);
            return false;
        }
        if (crc32_ieee(record_buffer_, read.bytes) != section->data_crc32) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::DecodeFailed);
            return false;
        }

        data = record_buffer_;
        data_size = read.bytes;
        cache_fallback(*file, *section, data);
    }

    sink_rejected_ = false;
    callback_operation_ = operation;
    if (operation->kind == ReportReadOperationKind::FallbackSeries) {
        size_t mapping_count = 0;
        const ReportReadMapping *mappings =
            plan_->mappings(*operation, mapping_count);
        if (!mappings || mapping_count != 1) {
            finish(ReportExecutorState::Failed,
                   ReportExecutorError::InvalidPlan);
            return false;
        }

        callback_mapping_ = mappings;
        if (!report_for_each_series_sample_range(
                section->payload_schema,
                section->coverage.start_ms,
                data,
                data_size,
                section->record_count,
                operation->first_record,
                operation->record_count,
                emit_series,
                this)) {
            finish(ReportExecutorState::Failed,
                   sink_rejected_
                       ? ReportExecutorError::SinkRejected
                       : ReportExecutorError::DecodeFailed);
            return false;
        }
    } else {
        for (size_t i = 0; i < section->record_count; ++i) {
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
    record_index_ = operation->record_count;
    finish_operation();
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportExecutor::find_cached_fallback(
    const NightCatalogFallbackFile &file,
    const NightCatalogFallbackSection &section) {
    if (!plan_ || plan_->key().kind != ReportArtifactKind::RangeTile ||
        section.data_size < AC_REPORT_FALLBACK_SECTION_CACHE_MIN_BYTES) {
        return {};
    }

    for (CachedFallbackSection &entry : fallback_cache_) {
        if (!entry.bytes || entry.file_identity != file.identity ||
            entry.data_offset != section.data_offset ||
            entry.data_size != section.data_size ||
            entry.data_crc32 != section.data_crc32) {
            continue;
        }

        fallback_cache_clock_++;
        if (fallback_cache_clock_ == 0) fallback_cache_clock_ = 1;
        entry.last_used = fallback_cache_clock_;
        return entry.bytes;
    }
    return {};
}

void ReportExecutor::cache_fallback(
    const NightCatalogFallbackFile &file,
    const NightCatalogFallbackSection &section,
    const uint8_t *data) {
    if (!data || !plan_ ||
        plan_->key().kind != ReportArtifactKind::RangeTile ||
        section.data_size < AC_REPORT_FALLBACK_SECTION_CACHE_MIN_BYTES) {
        return;
    }

#ifdef ARDUINO
    const MemoryStatus memory = Memory::status();
    if (!memory.psram_available ||
        memory.psram_free <
            section.data_size +
                AC_REPORT_FALLBACK_SECTION_CACHE_PSRAM_RESERVE) {
        return;
    }
#endif

    size_t target = 0;
    for (size_t i = 0; i < FallbackCacheCapacity; ++i) {
        if (!fallback_cache_[i].bytes) {
            target = i;
            break;
        }
        if (fallback_cache_[i].last_used <
            fallback_cache_[target].last_used) {
            target = i;
        }
    }

    std::shared_ptr<const LargeByteBuffer> bytes =
        LargeByteBuffer::copy_and_freeze(data, section.data_size);
    if (!bytes) return;

    fallback_cache_clock_++;
    if (fallback_cache_clock_ == 0) fallback_cache_clock_ = 1;
    fallback_cache_[target] = {
        file.identity,
        section.data_offset,
        section.data_size,
        section.data_crc32,
        fallback_cache_clock_,
        std::move(bytes),
    };
}

void ReportExecutor::clear_fallback_cache() {
    for (CachedFallbackSection &entry : fallback_cache_) entry = {};
    fallback_cache_clock_ = 0;
}

void ReportExecutor::finish_operation() {
    const ReportReadOperation *operation = plan_->operation(operation_index_);
    if (operation && operation->kind == ReportReadOperationKind::CsrEvents) {
        event_next_record_ = operation->first_record + operation->record_count;
    }

    release_prepared();
    active_fallback_.reset();
    ++operation_index_;
    record_index_ = 0;
    decoder_count_ = 0;

    if (operation_index_ >= operation_count_) {
        finish(ReportExecutorState::Complete, ReportExecutorError::None);
    } else {
        state_ = ReportExecutorState::SubmitRead;
    }
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
    active_fallback_.reset();
    free_scratch();
    plan_.reset();
    sink_ = nullptr;
    callback_mapping_ = nullptr;
    callback_operation_ = nullptr;
    event_context_ = {};
    event_file_index_ = UINT16_MAX;
    event_next_record_ = 0;
    event_context_valid_ = false;
    sink_rejected_ = false;
}

void ReportExecutor::release_prepared() {
    if (!read_port_ || !prepared_.valid()) return;
    const StoragePreparedRead prepared = prepared_;
    prepared_ = {};
    read_port_->release_prepared(prepared);
}

void ReportExecutor::free_scratch() {
    for (size_t i = 0; i < decoder_capacity_; ++i) {
        decoders_[i].~EdfReportSeriesDecoder();
    }
    Memory::free(decoders_);
    decoders_ = nullptr;
    decoder_capacity_ = 0;
    decoder_count_ = 0;

    Memory::free(record_buffer_);
    record_buffer_ = nullptr;
    record_capacity_ = 0;
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
