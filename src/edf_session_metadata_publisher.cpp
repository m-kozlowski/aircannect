#include "edf_session_metadata_publisher.h"

#include "string_util.h"

namespace aircannect {
namespace {

constexpr uint32_t RETRY_MS = 1000;

}  // namespace

EdfSessionMetadataPublication EdfSessionMetadataPublisher::publish(
    const EdfSessionMetadata &metadata) {
    if (!edf_session_metadata_valid(metadata)) return {};

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
    if (handle_.valid()) poll_active(now_ms);
    if (handle_.valid()) return;

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
    uint8_t bytes[EdfSessionMetadataCodec::RecordBytes] = {};
    char path[96] = {};
    if (!EdfSessionMetadataCodec::encode(active_.metadata,
                                         bytes,
                                         sizeof(bytes)) ||
        !edf_session_metadata_path(active_.metadata,
                                   path,
                                   sizeof(path))) {
        retry_active(now_ms, "metadata_encode_failed");
        return;
    }

    if (!EdfStorageWorker::enqueue_session_metadata(
            path, bytes, sizeof(bytes), &handle_)) {
        retry_at_ms_ = now_ms + RETRY_MS;
    }
}

void EdfSessionMetadataPublisher::poll_active(uint32_t now_ms) {
    EdfStorageMetadataResult result;
    if (!EdfStorageWorker::metadata_result(handle_, result) ||
        !result.complete) {
        return;
    }

    handle_ = {};
    if (!result.success) {
        retry_active(now_ms,
                     result.error[0]
                         ? result.error
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
