#pragma once

#include <memory>
#include <stddef.h>

#include "large_byte_buffer.h"
#include "report_artifact_payload.h"

namespace aircannect {

class ReportPayloadDeflater {
public:
    ReportPayloadDeflater() = default;
    ~ReportPayloadDeflater();

    ReportPayloadDeflater(const ReportPayloadDeflater &) = delete;
    ReportPayloadDeflater &operator=(const ReportPayloadDeflater &) = delete;

    bool start(const ReportArtifactPayloadDescriptor &payload,
               std::shared_ptr<const LargeByteBuffer> source,
               size_t psram_reserve);
    bool poll(size_t input_budget);
    void reset();

    bool active() const { return state_ == State::Compressing; }
    bool finished() const { return state_ == State::Finished; }
    const ReportArtifactPayloadDescriptor &payload() const {
        return payload_;
    }
    std::shared_ptr<const LargeByteBuffer> take_completed();

private:
    enum class State : uint8_t {
        Idle,
        Compressing,
        Finished,
    };

    void finish();
    void fail();
    void release_compressor();

    State state_ = State::Idle;
    ReportArtifactPayloadDescriptor payload_;
    std::shared_ptr<const LargeByteBuffer> source_;
    std::unique_ptr<LargeByteBuffer> output_;
    std::shared_ptr<const LargeByteBuffer> completed_;
    void *compressor_ = nullptr;
    size_t source_offset_ = 0;
    size_t output_offset_ = 0;
};

}  // namespace aircannect
