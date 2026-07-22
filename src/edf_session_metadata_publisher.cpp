#include "edf_session_metadata_publisher.h"

#include <string.h>
#include <utility>

#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t RETRY_MS = 1000;

}  // namespace

void EdfSessionMetadataPublisher::begin(StorageAtomicWritePort &storage) {
    storage_ = &storage;
}

EdfSessionMetadataPublication EdfSessionMetadataPublisher::publish(
    const EdfSessionMetadata &metadata) {
    if (!storage_ || !edf_session_metadata_valid(metadata)) return {};

    PendingPublication publication;
    publication.metadata = metadata;
    publication.generation = next_generation();

    if (active_.generation == 0 && pending_.empty()) {
        active_ = publication;
    } else if (!pending_.push(publication)) {
        return {};
    }

    return {publication.generation};
}

void EdfSessionMetadataPublisher::poll(uint32_t now_ms) {
    if (!storage_) return;

    if (ticket_.valid()) poll_active(now_ms);
    if (ticket_.valid()) return;

    if (active_.generation == 0 && !load_next()) return;
    if (static_cast<int32_t>(now_ms - retry_at_ms_) < 0) return;

    submit_active(now_ms);
}

bool EdfSessionMetadataPublisher::completed(
    EdfSessionMetadataPublication publication) const {
    if (!publication.valid() || completed_generation_ == 0) return false;
    return static_cast<int32_t>(completed_generation_ -
                                publication.generation) >= 0;
}

uint32_t EdfSessionMetadataPublisher::next_generation() {
    generation_++;
    if (generation_ == 0) generation_++;
    return generation_;
}

bool EdfSessionMetadataPublisher::load_next() {
    return pending_.pop(active_);
}

void EdfSessionMetadataPublisher::submit_active(uint32_t now_ms) {
    std::shared_ptr<const LargeByteBuffer> bytes =
        EdfSessionMetadataCodec::encode(active_.metadata);
    char path[96] = {};
    if (!bytes || !edf_session_metadata_path(active_.metadata,
                                             path,
                                             sizeof(path))) {
        retry_active(now_ms, "metadata_encode_failed");
        return;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = std::move(bytes);
    command.lane = StorageAtomicWriteLane::Foreground;
    command.generation = active_.generation;

    const OperationSubmission submission = storage_->request_write(command);
    if (!submission.accepted()) {
        retry_at_ms_ = now_ms + RETRY_MS;
        return;
    }

    ticket_ = submission.ticket;
}

void EdfSessionMetadataPublisher::poll_active(uint32_t now_ms) {
    StorageAtomicWriteCompletion completion;
    if (!storage_->take_completion(ticket_, completion)) return;

    ticket_ = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        retry_active(now_ms,
                     completion.error[0]
                         ? completion.error
                         : "metadata_write_failed");
        return;
    }

    completed_generation_ = active_.generation;
    active_ = {};
    retry_at_ms_ = 0;
    last_error_[0] = 0;
}

void EdfSessionMetadataPublisher::retry_active(uint32_t now_ms,
                                               const char *error) {
    copy_cstr(last_error_, sizeof(last_error_), error);
    retry_at_ms_ = now_ms + RETRY_MS;
}

}  // namespace aircannect
