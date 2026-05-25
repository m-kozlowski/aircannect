#include "stream_broker.h"

#include <string.h>

namespace aircannect {

StreamAcquireResult StreamBroker::acquire(const std::string &params_json,
                                          uint8_t source) {
    StreamAcquireResult result;
    if (params_json.empty()) return result;

    const size_t consumers = consumer_count();
    if (consumers > 0 && params_json_ != params_json) {
        result.status = StreamAcquireStatus::Incompatible;
        return result;
    }

    // A matching consumer may attach while StartStream is pending; the in-flight
    // command satisfies all consumers that share params_json_.
    const int slot = find_free_slot();
    if (slot < 0) {
        result.status = StreamAcquireStatus::Full;
        return result;
    }
    if (!ensure_frame_pool()) {
        result.status = StreamAcquireStatus::Rejected;
        return result;
    }

    consumers_[slot].active = true;
    consumers_[slot].source = source;
    params_json_ = params_json;
    clear_error();

    result.handle = static_cast<StreamConsumerHandle>(slot);
    result.status =
        consumers == 0 ? StreamAcquireStatus::Acquired
                       : StreamAcquireStatus::AlreadyActive;
    return result;
}

StreamAcquireResult StreamBroker::update(StreamConsumerHandle handle,
                                         const std::string &params_json) {
    StreamAcquireResult result;
    if (!consumer_active(handle) || params_json.empty()) return result;
    if (pending()) {
        result.status = StreamAcquireStatus::Busy;
        return result;
    }

    if (params_json_ == params_json) {
        result.handle = handle;
        result.status = StreamAcquireStatus::AlreadyActive;
        return result;
    }

    if (consumer_count() > 1) {
        result.status = StreamAcquireStatus::Incompatible;
        return result;
    }

    params_json_ = params_json;
    actual_active_ = false;
    consumers_[handle].queue.clear();
    clear_error();

    result.handle = handle;
    result.status = StreamAcquireStatus::Acquired;
    return result;
}

void StreamBroker::release(StreamConsumerHandle handle) {
    if (!consumer_active(handle)) return;
    consumers_[handle].queue.clear();
    consumers_[handle] = {};
    if (consumer_count() == 0) {
        clear_error();
        frame_pool_.release_storage();
    }
}

bool StreamBroker::consumer_active(StreamConsumerHandle handle) const {
    return handle >= 0 &&
           handle < static_cast<StreamConsumerHandle>(AC_STREAM_CONSUMERS_MAX) &&
           consumers_[handle].active;
}

size_t StreamBroker::consumer_count() const {
    size_t count = 0;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        if (consumers_[i].active) count++;
    }
    return count;
}

StreamCommand StreamBroker::next_command(uint32_t now_ms,
                                         uint32_t retry_interval_ms) const {
    StreamCommand command;
    if (pending() || error_) return command;
    if (static_cast<int32_t>(now_ms - last_command_ms_) <
        static_cast<int32_t>(retry_interval_ms)) {
        return command;
    }

    if (desired_active() && !actual_active_) {
        command.type = StreamCommandType::Start;
        command.params_json = params_json_;
    } else if (!desired_active() && actual_active_) {
        command.type = StreamCommandType::Stop;
        command.params_json =
            "{\"dataIds\":[],\"sampleIntervalMs\":200,\"reportIntervalMs\":1000}";
    }
    return command;
}

void StreamBroker::mark_command_queued(StreamCommandType type,
                                       uint32_t now_ms) {
    if (type == StreamCommandType::None) return;
    pending_ = type;
    last_command_ms_ = now_ms;
}

void StreamBroker::mark_command_deferred(uint32_t now_ms) {
    last_command_ms_ = now_ms;
}

void StreamBroker::mark_command_timeout(uint32_t now_ms) {
    pending_ = StreamCommandType::None;
    last_command_ms_ = now_ms;
}

void StreamBroker::mark_command_response(StreamCommandType type,
                                         bool is_error,
                                         uint32_t now_ms) {
    if (pending_ != type) return;
    pending_ = StreamCommandType::None;
    last_command_ms_ = now_ms;

    if (is_error) {
        error_ = true;
        error_command_ = type;
        if (type == StreamCommandType::Start) actual_active_ = false;
        return;
    }

    clear_error();
    if (type == StreamCommandType::Start) {
        actual_active_ = true;
    } else if (type == StreamCommandType::Stop) {
        actual_active_ = false;
        if (consumer_count() == 0) frame_pool_.release_storage();
    }
}

void StreamBroker::mark_reattach() {
    pending_ = StreamCommandType::None;
    actual_active_ = false;
    last_command_ms_ = 0;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        consumers_[i].queue.clear();
    }
    clear_error();
}

void StreamBroker::note_stream_data(uint32_t stream_id,
                                    const std::string &start_time,
                                    uint32_t now_ms) {
    last_stream_id_ = stream_id;
    if (!start_time.empty()) last_start_time_ = start_time;
    last_notification_ms_ = now_ms;
}

void StreamBroker::set_frame_observer(StreamFrameObserver observer,
                                      void *context) {
    frame_observer_ = observer;
    frame_observer_context_ = context;
}

StreamPublishResult StreamBroker::publish_stream_data(
    const std::string &payload,
    uint32_t now_ms) {
    StreamPublishResult result;

    StreamFrameMetadata metadata;
    if (stream_parse_metadata(payload, metadata)) {
        note_stream_data(metadata.stream_id, metadata.start_time, now_ms);
    }

    if (consumer_count() == 0) return result;

    if (!ensure_frame_pool()) {
        result.pool_exhausted = true;
        result.drops = consumer_count();
        pool_exhaustions_++;
        total_queue_drops_ += result.drops;
        return result;
    }
    StreamFrameRef frame = frame_pool_.allocate(now_ms);
    if (!frame) {
        result.pool_exhausted = true;
        result.drops = consumer_count();
        pool_exhaustions_++;
        total_queue_drops_ += result.drops;
        return result;
    }

    char error[96] = {};
    if (!stream_parse_frame(payload, now_ms, *frame.mutable_data(),
                            error, sizeof(error))) {
        (void)error;
        result.parse_error = true;
        parse_errors_++;
        return result;
    }

    if (frame->stream_id || frame->start_time[0]) {
        note_stream_data(frame->stream_id, frame->start_time, now_ms);
    }
    result.accepted = true;
    result.raw_truncated = frame->raw_truncated;
    result.values_truncated = frame->values_truncated;
    if (result.raw_truncated || result.values_truncated) truncated_frames_++;
    published_payloads_++;
    if (frame_observer_) {
        frame_observer_(frame_observer_context_, *frame, now_ms);
    }

    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        Consumer &consumer = consumers_[i];
        if (!consumer.active) continue;

        if (consumer.queue.full()) {
            StreamFrameRef discarded;
            consumer.queue.pop(discarded);
            consumer.queue_drops++;
            total_queue_drops_++;
            result.drops++;
        }

        if (consumer.queue.push(frame)) {
            fanout_targets_++;
            result.targets++;
        } else {
            consumer.queue_drops++;
            total_queue_drops_++;
            result.drops++;
        }
    }

    return result;
}

bool StreamBroker::next_frame(StreamConsumerHandle handle,
                              StreamFrameRef &frame) {
    if (!consumer_active(handle)) return false;
    return consumers_[handle].queue.pop(frame);
}

bool StreamBroker::next_payload(StreamConsumerHandle handle,
                                std::string &payload) {
    StreamFrameRef frame;
    if (!next_frame(handle, frame) || !frame) return false;
    payload.assign(frame->raw_json, frame->raw_json + strlen(frame->raw_json));
    return true;
}

size_t StreamBroker::consumer_queue_count(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return 0;
    return consumers_[handle].queue.count();
}

uint32_t StreamBroker::consumer_queue_drops(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return 0;
    return consumers_[handle].queue_drops;
}

uint8_t StreamBroker::consumer_source(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return 0;
    return consumers_[handle].source;
}

void StreamBroker::reset_counters() {
    published_payloads_ = 0;
    fanout_targets_ = 0;
    total_queue_drops_ = 0;
    parse_errors_ = 0;
    pool_exhaustions_ = 0;
    truncated_frames_ = 0;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        consumers_[i].queue_drops = 0;
    }
}

int StreamBroker::find_free_slot() const {
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        if (!consumers_[i].active) return static_cast<int>(i);
    }
    return -1;
}

void StreamBroker::clear_error() {
    error_ = false;
    error_command_ = StreamCommandType::None;
}

bool StreamBroker::ensure_frame_pool() {
    return frame_pool_.begin();
}

}  // namespace aircannect
