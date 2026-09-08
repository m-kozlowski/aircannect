#pragma once

#include "report_signal_store.h"

namespace aircannect {

// A transport tile contains whole, present 15-minute storage blocks.
// Slow raw signals and overview LOD cover the whole track; detail tiles remain
// UTC-aligned. Neither policy changes the underlying raw layout.
struct ReportSignalTile {
    static constexpr size_t IdentityBytes =
        28 + REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES;
    static constexpr size_t HeaderBytes = IdentityBytes + 4;
    static constexpr size_t MinRawBytes = 1024;
    static constexpr size_t DetailRawBytes = 45000;
    static constexpr size_t MaxBlocks = REPORT_SIGNAL_STORE_MAX_BLOCKS;
    static constexpr size_t MaxRawBytes =
        MaxBlocks * REPORT_SIGNAL_STORE_BLOCK_MS / 1000 * 2;

    int64_t start_ms = 0;
    int64_t end_ms = 0;
    size_t first_slot = 0;
    size_t block_count = 0;
    ReportSignalStorePlaneRange range;
    uint8_t header[HeaderBytes] = {};

    static bool whole_track(uint32_t interval_ms, bool envelope);
    static size_t blocks(uint32_t interval_ms, bool envelope);
    static bool describe(const ReportSignalStoreTrack &track,
                         ReportSignalStoreLevel level, size_t slot,
                         ReportSignalTile &tile);
    bool path(const ReportSignalStoreTrack &track,
              ReportSignalStoreLevel level, char *out, size_t capacity) const;
    bool matches(const uint8_t *data, size_t length, uint64_t file_size) const;
};

// Bounded compressor used by the report producer. Buffers live only through
// publication; no response cache or HTTP request owns this operation.
class ReportSignalTileEncoder {
public:
    ~ReportSignalTileEncoder();
    void reset();

    bool start(std::shared_ptr<const LargeByteBuffer> source,
               const ReportSignalTile &tile);
    bool poll();

    bool active() const { return state_ != nullptr; }
    bool succeeded() const { return succeeded_; }
    std::shared_ptr<const LargeByteBuffer> take_result();

private:
    void *state_ = nullptr;
    size_t consumed_ = 0;
    bool succeeded_ = true;

    std::shared_ptr<const LargeByteBuffer> source_;
    std::unique_ptr<LargeByteBuffer> output_;
    std::shared_ptr<const LargeByteBuffer> result_;
};

}  // namespace aircannect
