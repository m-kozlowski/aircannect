#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "board.h"

namespace aircannect {

enum class StreamSignalId : uint8_t {
    Unknown,
    PatientFlow,
    MaskPressure,
    MaskPressureTwoSecond,
    InspiratoryPressure,
    ExpiratoryPressure,
    InspiratoryPressureTwoSecond,
    ExpiratoryPressureTwoSecond,
    Leak,
    RespiratoryRate,
    TidalVolume,
    MinuteVentilation,
    TargetMinuteVentilation,
    IeRatio,
    SnoreIndex,
    FlowLimitation,
    InspiratoryDuration,
    HeartRate,
    SpO2,
};

struct StreamFrameMetadata {
    uint32_t stream_id = 0;
    uint32_t interval_ms = 0;
    char start_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
};

struct StreamSignalSpan {
    StreamSignalId id = StreamSignalId::Unknown;
    char name[AC_STREAM_FRAME_SIGNAL_NAME_MAX] = {};
    uint16_t value_offset = 0;
    uint16_t sample_count = 0;
    uint16_t valid_count = 0;
    uint32_t sample_interval_ms = 0;
};

struct StreamFrameData {
    bool in_use = false;
    bool raw_truncated = false;
    bool values_truncated = false;
    uint8_t refcount = 0;
    uint32_t sequence = 0;
    uint32_t parsed_at_ms = 0;
    uint32_t stream_id = 0;
    uint32_t interval_ms = 0;
    char start_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    size_t raw_json_len = 0;
    char raw_json[AC_STREAM_FRAME_RAW_MAX] = {};
    uint8_t signal_count = 0;
    uint16_t value_count = 0;
    StreamSignalSpan signals[AC_STREAM_FRAME_SIGNAL_MAX] = {};
    float values[AC_STREAM_FRAME_VALUES_MAX] = {};
    uint8_t valid_bits[(AC_STREAM_FRAME_VALUES_MAX + 7) / 8] = {};

    bool value_valid(size_t index) const;
    const StreamSignalSpan *find_signal(StreamSignalId id) const;
};

class StreamFramePool;

class StreamFrameRef {
public:
    StreamFrameRef() = default;
    StreamFrameRef(const StreamFrameRef &other);
    StreamFrameRef(StreamFrameRef &&other) noexcept;
    ~StreamFrameRef();

    StreamFrameRef &operator=(const StreamFrameRef &other);
    StreamFrameRef &operator=(StreamFrameRef &&other) noexcept;

    explicit operator bool() const { return data_ != nullptr; }
    const StreamFrameData *operator->() const { return data_; }
    const StreamFrameData &operator*() const { return *data_; }
    const StreamFrameData *data() const { return data_; }
    StreamFrameData *mutable_data() { return data_; }
    void reset();

private:
    friend class StreamFramePool;
    StreamFrameRef(StreamFramePool *pool, StreamFrameData *data);

    StreamFramePool *pool_ = nullptr;
    StreamFrameData *data_ = nullptr;
};

class StreamFramePool {
public:
    StreamFramePool() = default;
    ~StreamFramePool();

    bool begin();
    bool begin(size_t capacity);
    void reset();
    bool release_storage();

    StreamFrameRef allocate(uint32_t now_ms);

    size_t capacity() const { return capacity_; }
    size_t free_count() const;
    size_t in_use_count() const;
    uint32_t allocation_failures() const { return allocation_failures_; }

private:
    friend class StreamFrameRef;

    void retain(StreamFrameData *data);
    void release(StreamFrameData *data);
    void free_storage();

    StreamFrameData **frames_ = nullptr;
    size_t capacity_ = 0;
    uint32_t sequence_ = 0;
    uint32_t allocation_failures_ = 0;
};

void stream_frame_reset(StreamFrameData &frame);
StreamSignalId stream_signal_id_from_name(const char *name);
const char *stream_signal_id_name(StreamSignalId id);
uint32_t stream_signal_sample_interval_ms(const char *name,
                                          uint32_t fallback_interval_ms);
bool stream_parse_metadata(const std::string &payload,
                           StreamFrameMetadata &metadata,
                           char *error = nullptr,
                           size_t error_len = 0);
bool stream_parse_metadata(const char *payload,
                           size_t payload_len,
                           StreamFrameMetadata &metadata,
                           char *error = nullptr,
                           size_t error_len = 0);
bool stream_parse_frame(const std::string &payload,
                        uint32_t now_ms,
                        StreamFrameData &frame,
                        char *error = nullptr,
                        size_t error_len = 0);
bool stream_parse_frame(const char *payload,
                        size_t payload_len,
                        uint32_t now_ms,
                        StreamFrameData &frame,
                        char *error = nullptr,
                        size_t error_len = 0);

}  // namespace aircannect
