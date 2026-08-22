#pragma once

#include <memory>
#include <stddef.h>

#include "large_byte_buffer.h"
#include "report_artifacts.h"

namespace aircannect {

class ReportPayloadDeflater {
public:
    ReportPayloadDeflater() = default;
    ~ReportPayloadDeflater();

    ReportPayloadDeflater(const ReportPayloadDeflater &) = delete;
    ReportPayloadDeflater &operator=(const ReportPayloadDeflater &) = delete;

    bool start(const ReportArtifactDescriptor &artifact,
               std::shared_ptr<const LargeByteBuffer> source,
               size_t psram_reserve);
    bool poll(size_t input_budget);
    void reset();

    bool active() const { return state_ == State::Compressing; }
    bool finished() const { return state_ == State::Finished; }
    const ReportArtifactDescriptor &artifact() const { return artifact_; }
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
    ReportArtifactDescriptor artifact_;
    std::shared_ptr<const LargeByteBuffer> source_;
    std::unique_ptr<LargeByteBuffer> output_;
    std::shared_ptr<const LargeByteBuffer> completed_;
    void *compressor_ = nullptr;
    size_t source_offset_ = 0;
    size_t output_offset_ = 0;
};

}  // namespace aircannect
