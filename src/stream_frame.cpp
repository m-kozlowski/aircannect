#include "stream_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_cursor.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

void set_valid(StreamFrameData &frame, size_t index, bool valid) {
    if (index >= AC_STREAM_FRAME_VALUES_MAX) return;
    const size_t byte = index / 8;
    const uint8_t mask = static_cast<uint8_t>(1u << (index % 8));
    if (valid) frame.valid_bits[byte] |= mask;
    else frame.valid_bits[byte] &= static_cast<uint8_t>(~mask);
}

bool parse_value_array(JsonCursor &json,
                       StreamFrameData &frame,
                       StreamSignalSpan &span) {
    if (!json.consume('[')) {
        json.fail("expected signal array");
        return false;
    }

    json.skip_ws();
    if (json.pos < json.end && *json.pos == ']') {
        json.pos++;
        return true;
    }

    while (json.pos < json.end) {
        const bool can_store = frame.value_count < AC_STREAM_FRAME_VALUES_MAX;
        bool valid = false;
        float value = 0.0f;

        if (json.consume_literal("null")) {
            valid = false;
        } else {
            if (!json.parse_float(value)) return false;
            valid = true;
        }

        if (can_store) {
            const size_t index = frame.value_count++;
            frame.values[index] = value;
            set_valid(frame, index, valid);
            span.sample_count++;
            if (valid) span.valid_count++;
        } else {
            frame.values_truncated = true;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == ']') {
            json.pos++;
            return true;
        }
        json.fail("expected value array separator");
        return false;
    }

    json.fail("unterminated value array");
    return false;
}

bool parse_signal_object(JsonCursor &json,
                         StreamFrameData &frame,
                         uint32_t fallback_interval_ms) {
    if (!json.consume('{')) {
        json.fail("expected signal object");
        return false;
    }

    json.skip_ws();
    if (json.pos < json.end && *json.pos == '}') {
        json.pos++;
        return true;
    }

    bool first_member = true;
    while (json.pos < json.end) {
        char name[AC_STREAM_FRAME_SIGNAL_NAME_MAX] = {};
        if (!json.parse_string(name, sizeof(name))) return false;
        if (!json.consume(':')) {
            json.fail("expected signal colon");
            return false;
        }

        if (first_member && frame.signal_count < AC_STREAM_FRAME_SIGNAL_MAX) {
            StreamSignalSpan &span = frame.signals[frame.signal_count++];
            copy_text(span.name, sizeof(span.name), name);
            span.id = stream_signal_id_from_name(name);
            span.value_offset = frame.value_count;
            span.sample_interval_ms =
                stream_signal_sample_interval_ms(name, fallback_interval_ms);
            if (!parse_value_array(json, frame, span)) return false;
        } else {
            if (first_member) frame.values_truncated = true;
            if (!json.skip_value()) return false;
        }
        first_member = false;

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') {
            json.pos++;
            return true;
        }
        json.fail("expected signal object separator");
        return false;
    }

    json.fail("unterminated signal object");
    return false;
}

bool parse_data_array(JsonCursor &json, StreamFrameData &frame) {
    if (!json.consume('[')) {
        json.fail("expected data array");
        return false;
    }

    json.skip_ws();
    if (json.pos < json.end && *json.pos == ']') {
        json.pos++;
        return true;
    }

    while (json.pos < json.end) {
        if (!parse_signal_object(json, frame, frame.interval_ms)) {
            return false;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == ']') {
            json.pos++;
            return true;
        }
        json.fail("expected data array separator");
        return false;
    }

    json.fail("unterminated data array");
    return false;
}

bool parse_params(JsonCursor &json,
                  StreamFrameData *frame,
                  StreamFrameMetadata *metadata) {
    if (!json.consume('{')) {
        json.fail("expected params object");
        return false;
    }

    json.skip_ws();
    if (json.pos < json.end && *json.pos == '}') {
        json.pos++;
        return true;
    }

    while (json.pos < json.end) {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) {
            json.fail("expected params colon");
            return false;
        }

        if (strcmp(key, "streamId") == 0) {
            uint32_t id = 0;
            if (!json.parse_uint(id)) return false;
            if (frame) frame->stream_id = id;
            if (metadata) metadata->stream_id = id;
        } else if (strcmp(key, "intervalMs") == 0) {
            uint32_t interval = 0;
            if (!json.parse_uint(interval)) return false;
            if (frame) frame->interval_ms = interval;
            if (metadata) metadata->interval_ms = interval;
        } else if (strcmp(key, "startTime") == 0) {
            char start_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
            if (!json.parse_string(start_time, sizeof(start_time))) {
                return false;
            }
            if (frame) {
                copy_text(frame->start_time, sizeof(frame->start_time),
                          start_time);
            }
            if (metadata) {
                copy_text(metadata->start_time, sizeof(metadata->start_time),
                          start_time);
            }
        } else if (strcmp(key, "data") == 0 && frame) {
            if (!parse_data_array(json, *frame)) return false;
        } else {
            if (!json.skip_value()) return false;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') {
            json.pos++;
            return true;
        }
        json.fail("expected params separator");
        return false;
    }

    json.fail("unterminated params object");
    return false;
}

bool parse_top(JsonCursor &json,
               StreamFrameData *frame,
               StreamFrameMetadata *metadata) {
    if (!json.consume('{')) {
        json.fail("expected top object");
        return false;
    }

    json.skip_ws();
    if (json.pos < json.end && *json.pos == '}') {
        json.fail("empty stream payload");
        return false;
    }

    while (json.pos < json.end) {
        char key[64] = {};
        if (!json.parse_string(key, sizeof(key))) return false;
        if (!json.consume(':')) {
            json.fail("expected top colon");
            return false;
        }

        if (strcmp(key, "params") == 0) {
            if (!parse_params(json, frame, metadata)) return false;
        } else {
            if (!json.skip_value()) return false;
        }

        json.skip_ws();
        if (json.pos < json.end && *json.pos == ',') {
            json.pos++;
            continue;
        }
        if (json.pos < json.end && *json.pos == '}') {
            json.pos++;
            return true;
        }
        json.fail("expected top separator");
        return false;
    }

    json.fail("unterminated top object");
    return false;
}

void *alloc_frame_bytes(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes);
#else
    return calloc(1, bytes);
#endif
}

void free_frame_bytes(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

}  // namespace

bool StreamFrameData::value_valid(size_t index) const {
    if (index >= value_count || index >= AC_STREAM_FRAME_VALUES_MAX) {
        return false;
    }
    const size_t byte = index / 8;
    const uint8_t mask = static_cast<uint8_t>(1u << (index % 8));
    return (valid_bits[byte] & mask) != 0;
}

const StreamSignalSpan *StreamFrameData::find_signal(
    StreamSignalId id) const {
    for (size_t i = 0; i < signal_count; ++i) {
        if (signals[i].id == id) return &signals[i];
    }
    return nullptr;
}

StreamFrameRef::StreamFrameRef(StreamFramePool *pool, StreamFrameData *data)
    : pool_(pool), data_(data) {}

StreamFrameRef::StreamFrameRef(const StreamFrameRef &other)
    : pool_(other.pool_), data_(other.data_) {
    if (pool_ && data_) pool_->retain(data_);
}

StreamFrameRef::StreamFrameRef(StreamFrameRef &&other) noexcept
    : pool_(other.pool_), data_(other.data_) {
    other.pool_ = nullptr;
    other.data_ = nullptr;
}

StreamFrameRef::~StreamFrameRef() {
    reset();
}

StreamFrameRef &StreamFrameRef::operator=(const StreamFrameRef &other) {
    if (this == &other) return *this;
    reset();
    pool_ = other.pool_;
    data_ = other.data_;
    if (pool_ && data_) pool_->retain(data_);
    return *this;
}

StreamFrameRef &StreamFrameRef::operator=(StreamFrameRef &&other) noexcept {
    if (this == &other) return *this;
    reset();
    pool_ = other.pool_;
    data_ = other.data_;
    other.pool_ = nullptr;
    other.data_ = nullptr;
    return *this;
}

void StreamFrameRef::reset() {
    if (pool_ && data_) pool_->release(data_);
    pool_ = nullptr;
    data_ = nullptr;
}

void StreamFramePool::free_storage() {
    if (!frames_) return;
    for (size_t i = 0; i < capacity_; ++i) {
        free_frame_bytes(frames_[i]);
    }
    free_frame_bytes(frames_);
    frames_ = nullptr;
    capacity_ = 0;
}

StreamFramePool::~StreamFramePool() {
    free_storage();
}

bool StreamFramePool::begin() {
    size_t capacity = AC_STREAM_FRAME_POOL_INTERNAL;
#ifdef ARDUINO
    if (Memory::psram_available()) capacity = AC_STREAM_FRAME_POOL_PSRAM;
#endif
    return begin(capacity);
}

bool StreamFramePool::begin(size_t capacity) {
    if (frames_) return true;
    if (capacity == 0) return false;

    frames_ = static_cast<StreamFrameData **>(
        alloc_frame_bytes(sizeof(StreamFrameData *) * capacity));
    if (!frames_) {
        allocation_failures_++;
        return false;
    }

    capacity_ = capacity;
    for (size_t i = 0; i < capacity_; ++i) {
        frames_[i] = static_cast<StreamFrameData *>(
            alloc_frame_bytes(sizeof(StreamFrameData)));
        if (!frames_[i]) {
            allocation_failures_++;
            for (size_t j = 0; j < i; ++j) {
                free_frame_bytes(frames_[j]);
            }
            free_frame_bytes(frames_);
            frames_ = nullptr;
            capacity_ = 0;
            return false;
        }
    }

    return true;
}

void StreamFramePool::reset() {
    if (!frames_) return;
    for (size_t i = 0; i < capacity_; ++i) {
        stream_frame_reset(*frames_[i]);
    }
}

bool StreamFramePool::release_storage() {
    if (!frames_) return true;
    if (in_use_count() != 0) return false;
    free_storage();
    return true;
}

StreamFrameRef StreamFramePool::allocate(uint32_t now_ms) {
    if (!begin()) {
        allocation_failures_++;
        return {};
    }

    for (size_t i = 0; i < capacity_; ++i) {
        StreamFrameData *frame = frames_[i];
        if (!frame || frame->in_use) continue;
        stream_frame_reset(*frame);
        frame->in_use = true;
        frame->refcount = 1;
        frame->sequence = ++sequence_;
        frame->parsed_at_ms = now_ms;
        return StreamFrameRef(this, frame);
    }

    allocation_failures_++;
    return {};
}

size_t StreamFramePool::free_count() const {
    if (!frames_) return 0;
    size_t free = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if (frames_[i] && !frames_[i]->in_use) free++;
    }
    return free;
}

size_t StreamFramePool::in_use_count() const {
    if (!frames_) return 0;
    size_t used = 0;
    for (size_t i = 0; i < capacity_; ++i) {
        if (frames_[i] && frames_[i]->in_use) used++;
    }
    return used;
}

void StreamFramePool::retain(StreamFrameData *data) {
    if (!data || !data->in_use) return;
    if (data->refcount < 255) data->refcount++;
}

void StreamFramePool::release(StreamFrameData *data) {
    if (!data || !data->in_use || data->refcount == 0) return;
    data->refcount--;
    if (data->refcount == 0) {
        stream_frame_reset(*data);
    }
}

void stream_frame_reset(StreamFrameData &frame) {
    memset(&frame, 0, sizeof(frame));
}

StreamSignalId stream_signal_id_from_name(const char *name) {
    if (!name) return StreamSignalId::Unknown;
    if (strcmp(name, "PatientFlow") == 0 ||
        strcmp(name, "PatientFlow-100hz") == 0) {
        return StreamSignalId::PatientFlow;
    }
    if (strcmp(name, "MaskPressure") == 0 ||
        strcmp(name, "MaskPressure-100hz") == 0) {
        return StreamSignalId::MaskPressure;
    }
    if (strcmp(name, "MaskPressure-TwoSecond") == 0) {
        return StreamSignalId::MaskPressureTwoSecond;
    }
    if (strcmp(name, "InspiratoryPressure-50hz") == 0) {
        return StreamSignalId::InspiratoryPressure;
    }
    if (strcmp(name, "ExpiratoryPressure-50hz") == 0) {
        return StreamSignalId::ExpiratoryPressure;
    }
    if (strcmp(name, "InspiratoryPressure-TwoSecond") == 0) {
        return StreamSignalId::InspiratoryPressureTwoSecond;
    }
    if (strcmp(name, "ExpiratoryPressure-TwoSecond") == 0) {
        return StreamSignalId::ExpiratoryPressureTwoSecond;
    }
    if (strcmp(name, "Leak") == 0 ||
        strcmp(name, "Leak-50hz") == 0) return StreamSignalId::Leak;
    if (strcmp(name, "RespiratoryRate") == 0 ||
        strcmp(name, "RespiratoryRate-50hz") == 0) {
        return StreamSignalId::RespiratoryRate;
    }
    if (strcmp(name, "TidalVolume") == 0 ||
        strcmp(name, "TidalVolume-50hz") == 0) {
        return StreamSignalId::TidalVolume;
    }
    if (strcmp(name, "MinuteVentilation") == 0 ||
        strcmp(name, "MinuteVentilation-50hz") == 0) {
        return StreamSignalId::MinuteVentilation;
    }
    if (strcmp(name, "TargetMinuteVentilation") == 0) {
        return StreamSignalId::TargetMinuteVentilation;
    }
    if (strcmp(name, "IeRatio") == 0) return StreamSignalId::IeRatio;
    if (strcmp(name, "SnoreIndex") == 0 ||
        strcmp(name, "SnoreIndex-50hz") == 0) {
        return StreamSignalId::SnoreIndex;
    }
    if (strcmp(name, "FlowLimitation") == 0 ||
        strcmp(name, "FlowLimitation-50hz") == 0) {
        return StreamSignalId::FlowLimitation;
    }
    if (strcmp(name, "InspiratoryDuration") == 0) {
        return StreamSignalId::InspiratoryDuration;
    }
    if (strcmp(name, "HeartRate") == 0) return StreamSignalId::HeartRate;
    if (strcmp(name, "SpO2") == 0) return StreamSignalId::SpO2;
    return StreamSignalId::Unknown;
}

const char *stream_signal_id_name(StreamSignalId id) {
    switch (id) {
        case StreamSignalId::PatientFlow: return "PatientFlow";
        case StreamSignalId::MaskPressure: return "MaskPressure";
        case StreamSignalId::MaskPressureTwoSecond:
            return "MaskPressure-TwoSecond";
        case StreamSignalId::InspiratoryPressure:
            return "InspiratoryPressure";
        case StreamSignalId::ExpiratoryPressure:
            return "ExpiratoryPressure";
        case StreamSignalId::InspiratoryPressureTwoSecond:
            return "InspiratoryPressure-TwoSecond";
        case StreamSignalId::ExpiratoryPressureTwoSecond:
            return "ExpiratoryPressure-TwoSecond";
        case StreamSignalId::Leak: return "Leak";
        case StreamSignalId::RespiratoryRate:
            return "RespiratoryRate";
        case StreamSignalId::TidalVolume: return "TidalVolume";
        case StreamSignalId::MinuteVentilation:
            return "MinuteVentilation";
        case StreamSignalId::TargetMinuteVentilation:
            return "TargetMinuteVentilation";
        case StreamSignalId::IeRatio: return "IeRatio";
        case StreamSignalId::SnoreIndex: return "SnoreIndex";
        case StreamSignalId::FlowLimitation:
            return "FlowLimitation";
        case StreamSignalId::InspiratoryDuration:
            return "InspiratoryDuration";
        case StreamSignalId::HeartRate: return "HeartRate";
        case StreamSignalId::SpO2: return "SpO2";
        case StreamSignalId::Unknown:
        default: return "Unknown";
    }
}

uint32_t stream_signal_sample_interval_ms(const char *name,
                                          uint32_t fallback_interval_ms) {
    if (!name) return fallback_interval_ms;
    if (strstr(name, "-100hz")) return 10;
    if (strstr(name, "-50hz")) return 20;
    if (strstr(name, "-TwoSecond")) return 2000;
    return fallback_interval_ms;
}

bool stream_parse_metadata(const std::string &payload,
                           StreamFrameMetadata &metadata,
                           char *error,
                           size_t error_len) {
    metadata = {};
    JsonCursor json(payload, error, error_len);
    return parse_top(json, nullptr, &metadata);
}

bool stream_parse_frame(const std::string &payload,
                        uint32_t now_ms,
                        StreamFrameData &frame,
                        char *error,
                        size_t error_len) {
    const uint32_t sequence = frame.sequence;
    stream_frame_reset(frame);
    frame.sequence = sequence;
    frame.parsed_at_ms = now_ms;
    frame.in_use = true;
    frame.refcount = 1;

    frame.raw_json_len = payload.size();
    const size_t copy_len =
        payload.size() < AC_STREAM_FRAME_RAW_MAX - 1
            ? payload.size()
            : AC_STREAM_FRAME_RAW_MAX - 1;
    if (copy_len) memcpy(frame.raw_json, payload.data(), copy_len);
    frame.raw_json[copy_len] = 0;
    frame.raw_truncated = copy_len != payload.size();

    JsonCursor json(payload, error, error_len);
    if (!parse_top(json, &frame, nullptr)) return false;
    for (size_t i = 0; i < frame.signal_count; ++i) {
        if (frame.signals[i].sample_interval_ms == 0) {
            frame.signals[i].sample_interval_ms = frame.interval_ms;
        }
    }
    return frame.stream_id != 0 || frame.signal_count > 0;
}

}  // namespace aircannect
