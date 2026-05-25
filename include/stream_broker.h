#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "fixed_queue.h"
#include "stream_frame.h"

namespace aircannect {

using StreamConsumerHandle = int8_t;
static constexpr StreamConsumerHandle STREAM_CONSUMER_INVALID = -1;

enum class StreamAcquireStatus {
    Acquired,
    AlreadyActive,
    Incompatible,
    Full,
    Busy,
    Rejected,
};

enum class StreamCommandType {
    None,
    Start,
    Stop,
};

struct StreamAcquireResult {
    StreamAcquireStatus status = StreamAcquireStatus::Rejected;
    StreamConsumerHandle handle = STREAM_CONSUMER_INVALID;
};

struct StreamCommand {
    StreamCommandType type = StreamCommandType::None;
    std::string params_json;
};

struct StreamPublishResult {
    size_t targets = 0;
    size_t drops = 0;
    bool accepted = false;
    bool parse_error = false;
    bool pool_exhausted = false;
    bool raw_truncated = false;
    bool values_truncated = false;
};

using StreamFrameObserver = void (*)(void *context,
                                     const StreamFrameData &frame,
                                     uint32_t now_ms);

class StreamBroker {
public:
    StreamAcquireResult acquire(const std::string &params_json,
                                uint8_t source = 0);
    StreamAcquireResult update(StreamConsumerHandle handle,
                               const std::string &params_json);
    void release(StreamConsumerHandle handle);

    bool consumer_active(StreamConsumerHandle handle) const;
    size_t consumer_count() const;
    bool desired_active() const { return consumer_count() > 0; }
    bool actual_active() const { return actual_active_; }
    bool pending() const { return pending_ != StreamCommandType::None; }
    bool pending_start() const { return pending_ == StreamCommandType::Start; }
    bool pending_stop() const { return pending_ == StreamCommandType::Stop; }
    bool error() const { return error_; }
    StreamCommandType error_command() const { return error_command_; }
    const std::string &params_json() const { return params_json_; }

    StreamCommand next_command(uint32_t now_ms,
                               uint32_t retry_interval_ms) const;
    void mark_command_queued(StreamCommandType type, uint32_t now_ms);
    void mark_command_deferred(uint32_t now_ms);
    void mark_command_timeout(uint32_t now_ms);
    void mark_command_response(StreamCommandType type,
                               bool is_error,
                               uint32_t now_ms);
    void mark_reattach();

    void note_stream_data(uint32_t stream_id,
                          const std::string &start_time,
                          uint32_t now_ms);
    void set_frame_observer(StreamFrameObserver observer, void *context);
    StreamPublishResult publish_stream_data(const std::string &payload,
                                            uint32_t now_ms);
    bool next_frame(StreamConsumerHandle handle, StreamFrameRef &frame);
    bool next_payload(StreamConsumerHandle handle, std::string &payload);
    size_t consumer_queue_count(StreamConsumerHandle handle) const;
    uint32_t consumer_queue_drops(StreamConsumerHandle handle) const;
    uint8_t consumer_source(StreamConsumerHandle handle) const;
    uint32_t published_payloads() const { return published_payloads_; }
    uint32_t fanout_targets() const { return fanout_targets_; }
    uint32_t total_queue_drops() const { return total_queue_drops_; }
    uint32_t parse_errors() const { return parse_errors_; }
    uint32_t pool_exhaustions() const { return pool_exhaustions_; }
    uint32_t truncated_frames() const { return truncated_frames_; }
    size_t frame_pool_capacity() const { return frame_pool_.capacity(); }
    size_t frame_pool_free() const { return frame_pool_.free_count(); }
    size_t frame_pool_in_use() const { return frame_pool_.in_use_count(); }
    uint32_t frame_pool_allocation_failures() const {
        return frame_pool_.allocation_failures();
    }
    void reset_counters();

    uint32_t last_stream_id() const { return last_stream_id_; }
    const std::string &last_start_time() const { return last_start_time_; }
    uint32_t last_notification_ms() const { return last_notification_ms_; }

private:
    struct Consumer {
        bool active = false;
        uint8_t source = 0;
        FixedQueue<StreamFrameRef, AC_STREAM_CONSUMER_QUEUE_DEPTH> queue;
        uint32_t queue_drops = 0;
    };

    int find_free_slot() const;
    void clear_error();
    bool ensure_frame_pool();

    StreamFramePool frame_pool_;
    Consumer consumers_[AC_STREAM_CONSUMERS_MAX];

    std::string params_json_;
    std::string last_start_time_;
    uint32_t last_stream_id_ = 0;
    uint32_t last_notification_ms_ = 0;
    uint32_t last_command_ms_ = 0;
    uint32_t published_payloads_ = 0;
    uint32_t fanout_targets_ = 0;
    uint32_t total_queue_drops_ = 0;
    uint32_t parse_errors_ = 0;
    uint32_t pool_exhaustions_ = 0;
    uint32_t truncated_frames_ = 0;
    StreamFrameObserver frame_observer_ = nullptr;
    void *frame_observer_context_ = nullptr;

    bool actual_active_ = false;
    bool error_ = false;
    StreamCommandType pending_ = StreamCommandType::None;
    StreamCommandType error_command_ = StreamCommandType::None;
};

}  // namespace aircannect
