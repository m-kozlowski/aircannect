#include "report_signal_store_service.h"

#include <utility>

#include "string_util.h"

namespace aircannect {
namespace {

bool publishing_state(ReportSignalStoreState state) {
    return state == ReportSignalStoreState::PublishingSignals ||
           state == ReportSignalStoreState::PublishingEvents ||
           state == ReportSignalStoreState::PublishingMetadata;
}

}  // namespace

bool ReportSignalStoreStatus::active() const {
    return publishing_state(state);
}

bool ReportSignalStoreStatus::terminal() const {
    return state == ReportSignalStoreState::Ready ||
           state == ReportSignalStoreState::Failed ||
           state == ReportSignalStoreState::Cancelled;
}

ReportSignalStoreService::~ReportSignalStoreService() {
    cancel();
}

void ReportSignalStoreService::begin(StorageAtomicWritePort &write_port) {
    write_port_ = &write_port;
}

OperationAdmission ReportSignalStoreService::start(
    std::shared_ptr<const ReportSignalStoreBundle> bundle,
    uint32_t operation_generation,
    StorageAtomicWriteLane lane) {
    if (phase_ != Phase::Idle) return OperationAdmission::Busy;
    if (!write_port_ || !bundle || !bundle->valid() ||
        operation_generation == 0) return OperationAdmission::Rejected;

    bundle_ = std::move(bundle);
    operation_generation_ = operation_generation;
    lane_ = lane;
    status_ = {};
    status_.sleep_day = bundle_->sleep_day;
    status_.signal_count = bundle_->signal_count();
    if (status_.signal_count > 0) {
        phase_ = Phase::SubmitSignal;
        status_.state = ReportSignalStoreState::PublishingSignals;
    } else {
        phase_ = Phase::SubmitEvents;
        status_.state = ReportSignalStoreState::PublishingEvents;
    }
    return OperationAdmission::Accepted;
}

std::shared_ptr<const LargeByteBuffer>
ReportSignalStoreService::current_bytes() const {
    if (!bundle_) return {};

    switch (phase_) {
        case Phase::SubmitSignal:
        case Phase::WaitSignal: {
            const ReportSignalStoreFilePayload *signal =
                bundle_->signal(status_.signal_index);
            return signal ? signal->bytes : nullptr;
        }
        case Phase::SubmitEvents:
        case Phase::WaitEvents:
            return bundle_->events;
        case Phase::SubmitMetadata:
        case Phase::WaitMetadata:
            return bundle_->metadata;
        default:
            return {};
    }
}

bool ReportSignalStoreService::current_path(
    char *path,
    size_t path_size) const {
    if (!bundle_) return false;

    switch (phase_) {
        case Phase::SubmitSignal:
        case Phase::WaitSignal: {
            const ReportSignalStoreFilePayload *signal =
                bundle_->signal(status_.signal_index);
            return signal && signal->path(path, path_size);
        }
        case Phase::SubmitEvents:
        case Phase::WaitEvents:
            return report_signal_store_events_path(
                bundle_->sleep_day, path, path_size);
        case Phase::SubmitMetadata:
        case Phase::WaitMetadata:
            return report_signal_store_night_path(
                bundle_->sleep_day, path, path_size);
        default:
            return false;
    }
}

bool ReportSignalStoreService::submit_current() {
    char path[AC_STORAGE_PATH_MAX] = {};
    std::shared_ptr<const LargeByteBuffer> bytes = current_bytes();
    if (!write_port_ || !bundle_ || !bytes ||
        !current_path(path, sizeof(path))) {
        fail("report_signal_store_publish_invalid");
        return true;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = std::move(bytes);
    command.lane = lane_;
    command.generation = operation_generation_;
    const OperationSubmission submission = write_port_->request_write(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("report_signal_store_write_rejected");
        return true;
    }

    write_ticket_ = submission.ticket;
    switch (phase_) {
        case Phase::SubmitSignal:
            phase_ = Phase::WaitSignal;
            break;
        case Phase::SubmitEvents:
            phase_ = Phase::WaitEvents;
            break;
        case Phase::SubmitMetadata:
            phase_ = Phase::WaitMetadata;
            break;
        default:
            fail("report_signal_store_publish_phase_invalid");
            break;
    }
    return true;
}

bool ReportSignalStoreService::finish_current() {
    if (!write_port_ || !bundle_ || !write_ticket_.valid()) {
        fail("report_signal_store_publish_invalid");
        return true;
    }

    StorageAtomicWriteCompletion completion;
    if (!write_port_->take_completion(write_ticket_, completion)) return false;
    write_ticket_ = {};

    const std::shared_ptr<const LargeByteBuffer> expected = current_bytes();
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !expected || completion.bytes_written != expected->size()) {
        fail(completion.error[0] ? completion.error
                                 : "report_signal_store_write_failed");
        return true;
    }
    status_.bytes_written += completion.bytes_written;

    switch (phase_) {
        case Phase::WaitSignal:
            ++status_.signal_index;
            if (status_.signal_index < status_.signal_count) {
                phase_ = Phase::SubmitSignal;
            } else {
                phase_ = Phase::SubmitEvents;
                status_.state = ReportSignalStoreState::PublishingEvents;
            }
            break;
        case Phase::WaitEvents:
            phase_ = Phase::SubmitMetadata;
            status_.state = ReportSignalStoreState::PublishingMetadata;
            break;
        case Phase::WaitMetadata:
            status_.metadata_modified = completion.modified;
            phase_ = Phase::Ready;
            status_.state = ReportSignalStoreState::Ready;
            break;
        default:
            fail("report_signal_store_publish_phase_invalid");
            break;
    }
    return true;
}

bool ReportSignalStoreService::poll() {
    switch (phase_) {
        case Phase::SubmitSignal:
        case Phase::SubmitEvents:
        case Phase::SubmitMetadata:
            return submit_current();
        case Phase::WaitSignal:
        case Phase::WaitEvents:
        case Phase::WaitMetadata:
            return finish_current();
        case Phase::Idle:
        case Phase::Ready:
        case Phase::Failed:
        case Phase::Cancelled:
            return false;
    }
    return false;
}

void ReportSignalStoreService::fail(const char *error) {
    if (write_ticket_.valid() && write_port_) {
        (void)write_port_->abandon(write_ticket_);
    }
    write_ticket_ = {};
    phase_ = Phase::Failed;
    status_.state = ReportSignalStoreState::Failed;
    copy_cstr(status_.error, sizeof(status_.error), error);
    bundle_.reset();
}

void ReportSignalStoreService::cancel() {
    if (phase_ == Phase::Idle || phase_ == Phase::Ready ||
        phase_ == Phase::Failed || phase_ == Phase::Cancelled) {
        return;
    }

    if (write_ticket_.valid() && write_port_) {
        (void)write_port_->abandon(write_ticket_);
    }
    write_ticket_ = {};
    phase_ = Phase::Cancelled;
    status_.state = ReportSignalStoreState::Cancelled;
    bundle_.reset();
}

void ReportSignalStoreService::clear_operation() {
    bundle_.reset();
    write_ticket_ = {};
    operation_generation_ = 0;
    lane_ = StorageAtomicWriteLane::Maintenance;
}

void ReportSignalStoreService::reset() {
    cancel();
    clear_operation();
    phase_ = Phase::Idle;
    status_ = {};
}

std::shared_ptr<const ReportSignalStoreBundle>
ReportSignalStoreService::published() const {
    return phase_ == Phase::Ready ? bundle_ : nullptr;
}

}  // namespace aircannect
