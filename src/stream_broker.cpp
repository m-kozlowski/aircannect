#include "stream_broker.h"

#include <string.h>

#include "as11_rpc.h"
#include "data_id_csv.h"
#include "json_cursor.h"

namespace aircannect {
namespace {

static constexpr DataIdCsvLimits STREAM_DATA_ID_LIMITS = {
    AC_STREAM_FRAME_SIGNAL_MAX,
    AC_STREAM_FRAME_SIGNAL_NAME_MAX - 1,
    AC_STREAM_FRAME_SIGNAL_MAX * AC_STREAM_FRAME_SIGNAL_NAME_MAX - 1,
};

}  // namespace

void StreamBroker::poll(RpcRequestPort &rpc, uint32_t now_ms) {
    RpcRequestCompletion completion;
    if (command_ticket_.valid() &&
        rpc.take_completion(command_ticket_, completion)) {
        command_ticket_ = {};
        complete_command(completion, now_ms);
        command_type_ = StreamCommandType::None;
    }

    if (command_ticket_.valid()) return;

    StreamCommand command = next_command(now_ms,
                                         AC_STREAM_RESYNC_INTERVAL_MS);
    if (command.type == StreamCommandType::None) return;

    RpcRequestCommand request;
    request.method = "StartStream";
    request.params_json = command.params_json;
    request.source = RpcSource::Internal;
    request.timeout_ms = AC_RPC_STREAM_TIMEOUT_MS;
    request.generation = next_request_generation();
    if (quiesce_requested_ && command.type == StreamCommandType::Stop) {
        request.admission = RpcRequestAdmission::QuiesceControl;
    }

    const OperationSubmission submitted = rpc.request(request);
    if (!submitted.accepted()) {
        mark_command_deferred(now_ms);
        return;
    }

    command_ticket_ = submitted.ticket;
    command_type_ = command.type;
    mark_command_queued(command.type, now_ms);
}

void StreamBroker::transport_reset(RpcRequestPort &rpc, uint32_t now_ms) {
    release_command_ticket(rpc);
    mark_reattach(now_ms);
}

StreamAcquireResult StreamBroker::acquire(const std::string &params_json,
                                          RpcSource source) {
    StreamAcquireResult result;
    if (params_json.empty()) return result;
    Subscription requested;
    if (!parse_subscription(params_json, requested) ||
        requested.data_id_count == 0) {
        return result;
    }

    Subscription desired;
    if (!build_desired_with_extra(requested, desired)) {
        result.status = StreamAcquireStatus::Incompatible;
        return result;
    }

    // A matching consumer may attach while StartStream is pending; the in-flight
    // command satisfies all consumers that share params_json_.
    const std::string desired_params = build_subscription_params(desired);
    if (pending() && desired_params != params_json_) {
        result.status = StreamAcquireStatus::Busy;
        return result;
    }

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
    consumers_[slot].subscription = requested;
    apply_desired_subscription(desired);
    clear_error();

    result.handle = static_cast<StreamConsumerHandle>(slot);
    result.status =
        desired_params == params_json_ && actual_active_
            ? StreamAcquireStatus::AlreadyActive
            : StreamAcquireStatus::Acquired;
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

    Subscription requested;
    if (!parse_subscription(params_json, requested) ||
        requested.data_id_count == 0) {
        return result;
    }

    Subscription desired;
    if (!build_desired_with_replacement(handle, requested, desired)) {
        result.status = StreamAcquireStatus::Incompatible;
        return result;
    }

    const std::string desired_params = build_subscription_params(desired);
    if (desired_params == params_json_ && actual_active_) {
        result.handle = handle;
        result.status = StreamAcquireStatus::AlreadyActive;
        return result;
    }

    consumers_[handle].subscription = requested;
    apply_desired_subscription(desired);
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
    Subscription desired;
    if (build_desired_subscription(desired)) {
        apply_desired_subscription(desired);
    } else {
        params_json_.clear();
        clear_subscription(desired_subscription_);
    }
    if (consumer_count() == 0) {
        clear_error();
        frame_pool_.release_storage();
    }
}

void StreamBroker::note_external_start(const std::string &params_json,
                                       uint32_t now_ms) {
    if (params_json.empty()) return;
    Subscription requested;
    if (!parse_subscription(params_json, requested)) return;
    last_owned_activity_ms_ = now_ms;
    external_active_ = true;
    external_subscription_ = requested;
    Subscription desired;
    if (build_desired_subscription(desired)) {
        const std::string desired_params = build_subscription_params(desired);
        const std::string external_params = build_subscription_params(requested);
        params_json_ = desired_params;
        desired_subscription_ = desired;
        actual_active_ = desired_params == external_params;
    } else {
        params_json_ = build_subscription_params(requested);
        desired_subscription_ = requested;
        actual_active_ = true;
    }
    if (pending_ == StreamCommandType::Stop) pending_ = StreamCommandType::None;
    clear_error();
}

void StreamBroker::note_external_stop(uint32_t now_ms,
                                      ExternalStreamStopMode mode) {
    last_owned_activity_ms_ = now_ms;
    const bool stop_required =
        external_active_ && actual_active_ &&
        mode == ExternalStreamStopMode::CommandRequired;
    external_active_ = false;
    clear_subscription(external_subscription_);
    Subscription desired;
    if (build_desired_subscription(desired)) {
        apply_desired_subscription(desired);
        actual_active_ = false;
    } else {
        params_json_.clear();
        clear_subscription(desired_subscription_);
        actual_active_ = stop_required;
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

    if (quiesce_requested_) {
        if (quiesced_) return command;
        if (!actual_active_ && accepted_subscription_.data_id_count == 0) {
            return command;
        }
        command.type = StreamCommandType::Stop;
        command.params_json =
            "{\"dataIds\":[],\"sampleIntervalMs\":200,\"reportIntervalMs\":1000}";
    } else if (desired_active() && !actual_active_) {
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

    if (type == StreamCommandType::Start) {
        start_requests_++;
    } else if (type == StreamCommandType::Stop) {
        stop_requests_++;
    }
    pending_ = type;
    last_command_ms_ = now_ms;
    last_owned_activity_ms_ = now_ms;
}

void StreamBroker::mark_command_deferred(uint32_t now_ms) {
    command_deferred_++;
    last_command_ms_ = now_ms;
    last_owned_activity_ms_ = now_ms;
}

void StreamBroker::mark_command_timeout(uint32_t now_ms) {
    pending_ = StreamCommandType::None;
    last_command_ms_ = now_ms;
    last_owned_activity_ms_ = now_ms;
}

void StreamBroker::mark_command_response(StreamCommandType type,
                                         bool is_error,
                                         const std::string &payload,
                                         uint32_t now_ms) {
    if (pending_ != type) return;
    pending_ = StreamCommandType::None;
    last_command_ms_ = now_ms;
    last_owned_activity_ms_ = now_ms;
    if (is_error) command_errors_++;

    if (is_error && quiesce_requested_ && type == StreamCommandType::Stop) {
        quiesced_ = false;
        return;
    }

    if (is_error) {
        error_ = true;
        error_command_ = type;
        if (type == StreamCommandType::Start) actual_active_ = false;
        if (type == StreamCommandType::Start ||
            type == StreamCommandType::Stop) {
            clear_subscription(accepted_subscription_);
        }
        return;
    }

    clear_error();
    if (type == StreamCommandType::Start) {
        actual_active_ = true;
        Subscription accepted;
        uint32_t stream_id = 0;
        if (parse_start_response(payload, accepted, stream_id)) {
            accepted.sample_ms = desired_subscription_.sample_ms;
            accepted.report_ms = desired_subscription_.report_ms;
            accepted_subscription_ = accepted;
            if (stream_id) last_stream_id_ = stream_id;
        } else {
            clear_subscription(accepted_subscription_);
        }
    } else if (type == StreamCommandType::Stop) {
        actual_active_ = false;
        clear_subscription(accepted_subscription_);
        if (quiesce_requested_) quiesced_ = true;
        if (consumer_count() == 0) frame_pool_.release_storage();
    }
}

void StreamBroker::mark_reattach(uint32_t now_ms) {
    pending_ = StreamCommandType::None;
    actual_active_ = false;
    last_command_ms_ = 0;
    if (desired_active()) last_owned_activity_ms_ = now_ms;
    clear_subscription(accepted_subscription_);
    quiesced_ = false;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        consumers_[i].queue.clear();
    }
    clear_error();
}

void StreamBroker::request_quiesce(uint32_t now_ms) {
    last_owned_activity_ms_ = now_ms;
    quiesce_requested_ = true;
    quiesced_ = !actual_active_ && accepted_subscription_.data_id_count == 0;
    clear_error();
    if (pending_ == StreamCommandType::Start) pending_ = StreamCommandType::None;
}

void StreamBroker::clear_quiesce() {
    quiesce_requested_ = false;
    quiesced_ = false;
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
    return publish_stream_data(payload.data(), payload.size(), now_ms);
}

StreamPublishResult StreamBroker::publish_stream_data(
    const char *payload,
    size_t payload_len,
    uint32_t now_ms) {
    StreamPublishResult result;

    const size_t consumers = consumer_count();
    if (consumers == 0) {
        StreamFrameMetadata metadata;
        if (stream_parse_metadata(payload, payload_len, metadata)) {
            note_stream_data(metadata.stream_id, metadata.start_time, now_ms);
        }
        return result;
    }

    if (!ensure_frame_pool()) {
        result.pool_exhausted = true;
        result.drops = consumers;
        pool_exhaustions_++;
        total_queue_drops_ += result.drops;
        return result;
    }
    StreamFrameRef frame = frame_pool_.allocate(now_ms);
    if (!frame) {
        result.pool_exhausted = true;
        result.drops = consumers;
        pool_exhaustions_++;
        total_queue_drops_ += result.drops;
        return result;
    }

    char error[96] = {};
    if (!stream_parse_frame(payload, payload_len, now_ms, *frame.mutable_data(),
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
    result.values_truncated = frame->values_truncated;
    if (result.values_truncated) truncated_frames_++;
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

size_t StreamBroker::consumer_queue_count(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return 0;
    return consumers_[handle].queue.count();
}

uint32_t StreamBroker::consumer_queue_drops(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return 0;
    return consumers_[handle].queue_drops;
}

RpcSource StreamBroker::consumer_source(StreamConsumerHandle handle) const {
    if (!consumer_active(handle)) return RpcSource::Internal;
    return consumers_[handle].source;
}

bool StreamBroker::activity_active(uint32_t now_ms, uint32_t quiet_ms) const {
    if (realtime_active()) return true;
    return last_owned_activity_ms_ &&
           now_ms - last_owned_activity_ms_ < quiet_ms;
}

bool StreamBroker::accepted_data_id(const char *data_id) const {
    if (!data_id || !*data_id) return false;
    return data_id_csv_contains(accepted_subscription_.data_ids_csv,
                                data_id,
                                strlen(data_id));
}

bool StreamBroker::accepted_data_ids_cover(const char *data_ids_csv) const {
    if (!data_ids_csv || !*data_ids_csv) return false;
    return data_id_csv_covers(accepted_subscription_.data_ids_csv,
                              data_ids_csv);
}

void StreamBroker::reset_counters() {
    published_payloads_ = 0;
    fanout_targets_ = 0;
    total_queue_drops_ = 0;
    parse_errors_ = 0;
    pool_exhaustions_ = 0;
    truncated_frames_ = 0;
    start_requests_ = 0;
    stop_requests_ = 0;
    command_deferred_ = 0;
    command_errors_ = 0;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        consumers_[i].queue_drops = 0;
    }
}

void StreamBroker::complete_command(
    const RpcRequestCompletion &completion,
    uint32_t now_ms) {
    if (completion.cause == RpcCompletionCause::Response) {
        mark_command_response(command_type_, completion.response_error,
                              completion.payload, now_ms);
        return;
    }

    mark_command_timeout(now_ms);
    if (quiesce_requested_) request_quiesce(now_ms);
}

uint32_t StreamBroker::next_request_generation() {
    request_generation_++;
    if (request_generation_ == 0) request_generation_++;
    return request_generation_;
}

void StreamBroker::release_command_ticket(RpcRequestPort &rpc) {
    if (!command_ticket_.valid()) return;

    (void)rpc.cancel(command_ticket_);
    RpcRequestCompletion completion;
    (void)rpc.take_completion(command_ticket_, completion);
    command_ticket_ = {};
    command_type_ = StreamCommandType::None;
}

bool StreamBroker::parse_subscription(const std::string &params_json,
                                      Subscription &subscription) {
    clear_subscription(subscription);
    JsonCursor json(params_json);
    if (!json.consume('{')) return false;

    bool saw_data_ids = false;
    uint32_t sample_ms = 0;
    uint32_t report_ms = 0;

    json.skip_ws();
    while (json.pos < json.end && *json.pos != '}') {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) return false;

        if (strcmp(key, "dataIds") == 0) {
            if (!json.consume('[')) return false;
            saw_data_ids = true;
            json.skip_ws();
            while (json.pos < json.end && *json.pos != ']') {
                char data_id[AC_STREAM_FRAME_SIGNAL_NAME_MAX] = {};
                if (!json.parse_string(data_id, sizeof(data_id))) {
                    return false;
                }
                if (!add_data_id(subscription, data_id)) return false;

                json.skip_ws();
                if (json.pos < json.end && *json.pos == ',') {
                    json.pos++;
                    json.skip_ws();
                    continue;
                }
                if (json.pos < json.end && *json.pos == ']') break;
                return false;
            }
            if (!json.consume(']')) return false;
        } else if (strcmp(key, "sampleIntervalMs") == 0) {
            if (!json.parse_uint(sample_ms)) return false;
        } else if (strcmp(key, "reportIntervalMs") == 0) {
            if (!json.parse_uint(report_ms)) return false;
        } else {
            if (!json.skip_value()) return false;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            json.skip_ws();
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') break;
        return false;
    }
    if (!json.consume('}')) return false;
    if (!saw_data_ids) return false;

    normalize_stream_intervals(sample_ms, report_ms);
    subscription.sample_ms = sample_ms;
    subscription.report_ms = report_ms;
    return true;
}

std::string StreamBroker::build_subscription_params(
    const Subscription &subscription) {
    return build_stream_params(subscription.data_ids_csv, subscription.sample_ms,
                               subscription.report_ms);
}

bool StreamBroker::add_data_id(Subscription &subscription,
                               const std::string &data_id) {
    if (data_id.empty()) return true;
    return data_id_csv_add(subscription.data_ids_csv,
                           subscription.data_id_count,
                           data_id.data(),
                           data_id.size(),
                           STREAM_DATA_ID_LIMITS);
}

bool StreamBroker::merge_data_ids(Subscription &subscription,
                                  const Subscription &input) {
    return data_id_csv_merge(subscription.data_ids_csv,
                             subscription.data_id_count,
                             input.data_ids_csv.c_str(),
                             STREAM_DATA_ID_LIMITS);
}

bool StreamBroker::parse_start_response(const std::string &payload,
                                        Subscription &accepted,
                                        uint32_t &stream_id) {
    clear_subscription(accepted);
    stream_id = 0;

    JsonCursor json(payload);
    if (!json.consume('{')) return false;

    bool saw_result = false;
    bool saw_data_ids = false;
    json.skip_ws();
    while (json.pos < json.end && *json.pos != '}') {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) return false;

        if (strcmp(key, "result") == 0) {
            if (!json.consume('{')) return false;
            saw_result = true;
            json.skip_ws();
            while (json.pos < json.end && *json.pos != '}') {
                char result_key[64] = {};
                if (!json.parse_string(result_key, sizeof(result_key))) {
                    return false;
                }
                if (!json.consume(':')) return false;

                if (strcmp(result_key, "streamId") == 0) {
                    if (!json.parse_uint(stream_id)) return false;
                } else if (strcmp(result_key, "dataIds") == 0) {
                    if (!json.consume('[')) return false;
                    saw_data_ids = true;
                    json.skip_ws();
                    while (json.pos < json.end && *json.pos != ']') {
                        if (!json.consume('{')) return false;
                        char data_id[AC_STREAM_FRAME_SIGNAL_NAME_MAX] = {};
                        bool valid = false;
                        bool saw_data_id = false;
                        json.skip_ws();
                        while (json.pos < json.end && *json.pos != '}') {
                            char item_key[64] = {};
                            if (!json.parse_string(item_key,
                                                   sizeof(item_key))) {
                                return false;
                            }
                            if (!json.consume(':')) return false;
                            if (strcmp(item_key, "dataId") == 0) {
                                if (!json.parse_string(data_id,
                                                       sizeof(data_id))) {
                                    return false;
                                }
                                saw_data_id = true;
                            } else if (strcmp(item_key, "valid") == 0) {
                                if (json.consume_literal("true")) {
                                    valid = true;
                                } else if (json.consume_literal("false")) {
                                    valid = false;
                                } else {
                                    return false;
                                }
                            } else {
                                if (!json.skip_value()) return false;
                            }

                            json.skip_ws();
                            if (json.pos < json.end && *json.pos == ',') {
                                json.pos++;
                                json.skip_ws();
                                continue;
                            }
                            if (json.pos < json.end && *json.pos == '}') {
                                break;
                            }
                            return false;
                        }
                        if (!json.consume('}')) return false;
                        if (saw_data_id && valid) {
                            if (!add_data_id(accepted, data_id)) return false;
                        }

                        json.skip_ws();
                        if (json.pos < json.end && *json.pos == ',') {
                            json.pos++;
                            json.skip_ws();
                            continue;
                        }
                        if (json.pos < json.end && *json.pos == ']') break;
                        return false;
                    }
                    if (!json.consume(']')) return false;
                } else {
                    if (!json.skip_value()) return false;
                }

                json.skip_ws();
                if (json.pos < json.end && *json.pos == ',') {
                    json.pos++;
                    json.skip_ws();
                    continue;
                }
                if (json.pos < json.end && *json.pos == '}') break;
                return false;
            }
            if (!json.consume('}')) return false;
        } else {
            if (!json.skip_value()) return false;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            json.skip_ws();
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') break;
        return false;
    }
    if (!json.consume('}')) return false;
    return saw_result && saw_data_ids;
}

bool StreamBroker::build_desired_subscription(
    Subscription &subscription) const {
    clear_subscription(subscription);
    bool have_interval = false;

    auto merge = [&](const Subscription &input) -> bool {
        if (input.data_id_count == 0) return true;
        if (!have_interval) {
            subscription.sample_ms = input.sample_ms;
            subscription.report_ms = input.report_ms;
            have_interval = true;
        } else {
            if (input.sample_ms < subscription.sample_ms) {
                subscription.sample_ms = input.sample_ms;
            }
            if (input.report_ms < subscription.report_ms) {
                subscription.report_ms = input.report_ms;
            }
        }
        return merge_data_ids(subscription, input);
    };

    if (external_active_ && !merge(external_subscription_)) return false;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        if (!consumers_[i].active) continue;
        if (!merge(consumers_[i].subscription)) return false;
    }

    return have_interval;
}

bool StreamBroker::build_desired_with_extra(
    const Subscription &extra,
    Subscription &subscription) const {
    clear_subscription(subscription);
    bool have_interval = false;

    auto merge = [&](const Subscription &input) -> bool {
        if (input.data_id_count == 0) return true;
        if (!have_interval) {
            subscription.sample_ms = input.sample_ms;
            subscription.report_ms = input.report_ms;
            have_interval = true;
        } else {
            if (input.sample_ms < subscription.sample_ms) {
                subscription.sample_ms = input.sample_ms;
            }
            if (input.report_ms < subscription.report_ms) {
                subscription.report_ms = input.report_ms;
            }
        }
        return merge_data_ids(subscription, input);
    };

    if (external_active_ && !merge(external_subscription_)) return false;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        if (!consumers_[i].active) continue;
        if (!merge(consumers_[i].subscription)) return false;
    }
    if (!merge(extra)) return false;
    if (!have_interval) return false;
    return true;
}

bool StreamBroker::build_desired_with_replacement(
    StreamConsumerHandle handle,
    const Subscription &replacement,
    Subscription &subscription) const {
    clear_subscription(subscription);
    bool have_interval = false;

    auto merge = [&](const Subscription &input) -> bool {
        if (input.data_id_count == 0) return true;
        if (!have_interval) {
            subscription.sample_ms = input.sample_ms;
            subscription.report_ms = input.report_ms;
            have_interval = true;
        } else {
            if (input.sample_ms < subscription.sample_ms) {
                subscription.sample_ms = input.sample_ms;
            }
            if (input.report_ms < subscription.report_ms) {
                subscription.report_ms = input.report_ms;
            }
        }
        return merge_data_ids(subscription, input);
    };

    if (external_active_ && !merge(external_subscription_)) return false;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        if (!consumers_[i].active) continue;
        if (static_cast<StreamConsumerHandle>(i) == handle) {
            if (!merge(replacement)) return false;
        } else if (!merge(consumers_[i].subscription)) {
            return false;
        }
    }
    if (!have_interval) subscription = replacement;
    return true;
}

void StreamBroker::apply_desired_subscription(
    const Subscription &subscription) {
    const std::string new_params = build_subscription_params(subscription);
    desired_subscription_ = subscription;
    if (params_json_ != new_params) {
        params_json_ = new_params;
        actual_active_ = false;
    }
}

void StreamBroker::clear_subscription(Subscription &subscription) {
    subscription.sample_ms = 0;
    subscription.report_ms = 0;
    subscription.data_id_count = 0;
    subscription.data_ids_csv.clear();
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
