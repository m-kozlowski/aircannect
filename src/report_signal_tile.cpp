#include "report_signal_tile.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "little_endian.h"
#include "memory_manager.h"
#include "storage_path.h"

#ifdef ARDUINO
#include <uzlib/uzlib.h>
#else
#include <zlib.h>
#endif

namespace aircannect {

bool ReportSignalTile::whole_track(uint32_t interval_ms, bool envelope) {
    return interval_ms >= (envelope ? 10000u : 1000u);
}

size_t ReportSignalTile::blocks(uint32_t interval_ms, bool envelope) {
    if (!interval_ms) return 1;
    if (whole_track(interval_ms, envelope)) return MaxBlocks;

    const size_t bytes = REPORT_SIGNAL_STORE_BLOCK_MS / interval_ms *
        (envelope ? 4 : 2);
    size_t count = 1;
    while (count < MaxBlocks && bytes * count * 2 <= DetailRawBytes) count *= 2;
    return count;
}

bool ReportSignalTile::describe(const ReportSignalStoreTrack &track,
                                ReportSignalStoreLevel level, size_t slot,
                                ReportSignalTile &tile) {
    tile = {};
    if (slot >= track.block_slot_count || !track.sample_interval_ms) return false;

    const uint32_t interval = level == ReportSignalStoreLevel::Raw
        ? track.sample_interval_ms
        : level == ReportSignalStoreLevel::OneSecond ? 1000 : 10000;
    tile.start_ms = track.first_block_start_ms;
    tile.end_ms = track.first_block_start_ms +
        track.block_slot_count * REPORT_SIGNAL_STORE_BLOCK_MS;

    const bool envelope = level != ReportSignalStoreLevel::Raw;
    if (!whole_track(interval, envelope)) {
        const int64_t duration = blocks(interval, envelope) *
            REPORT_SIGNAL_STORE_BLOCK_MS;
        const int64_t position = track.first_block_start_ms +
            slot * REPORT_SIGNAL_STORE_BLOCK_MS;
        const int64_t aligned = position - position % duration;
        tile.start_ms = std::max(tile.start_ms, aligned);
        tile.end_ms = std::min(tile.end_ms, aligned + duration);
    }

    tile.first_slot = (tile.start_ms - track.first_block_start_ms) /
        REPORT_SIGNAL_STORE_BLOCK_MS;
    tile.block_count = (tile.end_ms - tile.start_ms) / REPORT_SIGNAL_STORE_BLOCK_MS;
    if (!ReportSignalStoreFileCodec::plane_range(
            track, tile.start_ms, tile.block_count, level, tile.range)) return false;

    for (size_t i = 0; i < tile.block_count; ++i) {
        const size_t s = tile.first_slot + i;
        if (track.present_blocks[s / 8] & (1u << (s % 8))) {
            tile.header[28 + i / 8] |= 1u << (i % 8);
        }
    }
    memcpy(tile.header, "ACTILE02", 8);
    LittleEndian::put_le64(tile.header + 8, tile.start_ms);
    LittleEndian::put_le64(tile.header + 16, std::min(tile.end_ms,
        track.last_valid_sample_ms + track.sample_interval_ms));
    LittleEndian::put_le32(tile.header + 24, static_cast<uint32_t>(tile.range.length));
    return true;
}

bool ReportSignalTile::path(const ReportSignalStoreTrack &track,
                            ReportSignalStoreLevel level,
                            char *out, size_t capacity) const {
    char signal[AC_STORAGE_PATH_MAX] = {};
    if (!report_signal_store_signal_path(track, level, signal, sizeof(signal))) {
        return false;
    }
    char *name = strrchr(signal, '/');
    if (!name) return false;
    *name++ = '\0';
    const int n = snprintf(out, capacity, "%s/http/%s/%lld.deflate",
        signal, name, static_cast<long long>(start_ms));
    return n > 0 && static_cast<size_t>(n) < capacity;
}

bool ReportSignalTile::matches(const uint8_t *data, size_t length,
                               uint64_t file_size) const {
    return data && length == HeaderBytes && file_size > HeaderBytes &&
        file_size == LittleEndian::get_le32(data + IdentityBytes) &&
        memcmp(data, header, IdentityBytes) == 0;
}

namespace {
constexpr size_t INPUT_BYTES = 2048;
#ifdef ARDUINO
struct EncoderState {
    uzlib_comp compressor = {};
    uzlib_hash_entry_t hash[4096] = {};
    size_t capacity = 0;
};

unsigned int write_byte(uzlib_comp *c, unsigned char byte) {
    const auto *state = reinterpret_cast<const EncoderState *>(c);
    if (static_cast<size_t>(c->outlen) >= state->capacity) {
        c->reserved[0] = 1;
        return 0;
    }
    c->outbuf[c->outlen] = byte;
    return 1;
}
#else
struct EncoderState { z_stream compressor = {}; };
#endif
}

ReportSignalTileEncoder::~ReportSignalTileEncoder() { reset(); }

bool ReportSignalTileEncoder::start(
    std::shared_ptr<const LargeByteBuffer> source, const ReportSignalTile &tile) {
    reset();
    if (!source || source->size() != tile.range.length ||
        source->size() < ReportSignalTile::MinRawBytes ||
        source->size() > ReportSignalTile::MaxRawBytes) {
        return false;
    }

    state_ = Memory::calloc_large(1, sizeof(EncoderState), false);
    const size_t capacity = ReportSignalTile::HeaderBytes + source->size() +
        (source->size() + 7) / 8 + 64;
    output_ = LargeByteBuffer::allocate(capacity);
    if (!state_ || !output_) {
        reset();
        succeeded_ = false;
        return false;
    }
    memcpy(output_->data(), tile.header, ReportSignalTile::HeaderBytes);
    auto &c = static_cast<EncoderState *>(state_)->compressor;
#ifdef ARDUINO
    c.outbuf = output_->data();
    static_cast<EncoderState *>(state_)->capacity = capacity;
    c.outsize = static_cast<int>(capacity);
    c.outlen = ReportSignalTile::HeaderBytes;
    c.outbuf[c.outlen++] = 0x78;
    c.outbuf[c.outlen++] = 0x01;
    c.hash_table = static_cast<EncoderState *>(state_)->hash;
    c.hash_bits = 12;
    c.dict_size = 32768;
    c.writeDestByte = write_byte;
    c.checksum = 1;
    zlib_start_block(&c);
#else
    if (deflateInit(&c, 1) != Z_OK) {
        reset();
        succeeded_ = false;
        return false;
    }
    c.next_out = output_->data() + ReportSignalTile::HeaderBytes;
    c.avail_out = capacity - ReportSignalTile::HeaderBytes;
#endif
    source_ = std::move(source);
    return true;
}

bool ReportSignalTileEncoder::poll() {
    if (!state_) return false;
    auto &c = static_cast<EncoderState *>(state_)->compressor;
    const size_t count = std::min(INPUT_BYTES, source_->size() - consumed_);
    const bool final = consumed_ + count == source_->size();
    size_t size = 0;
    bool failed = false;
#ifdef ARDUINO
    c.checksum = uzlib_adler32(source_->data() + consumed_, count, c.checksum);
    uzlib_compress(&c, source_->data() + consumed_, count);
    if (final) {
        zlib_finish_block(&c);
        for (int shift = 24; shift >= 0; shift -= 8) {
            write_byte(&c, c.checksum >> shift);
            ++c.outlen;
        }
    }
    failed = c.reserved[0] != 0;
    size = c.outlen;
#else
    c.next_in = const_cast<uint8_t *>(source_->data() + consumed_);
    c.avail_in = count;
    const int status = deflate(&c, final ? Z_FINISH : Z_NO_FLUSH);
    failed = final ? status != Z_STREAM_END : status != Z_OK;
    size = c.total_out + ReportSignalTile::HeaderBytes;
#endif
    consumed_ += count;
    if (!failed && !final) return true;

    succeeded_ = !failed;
    if (!failed && size + 256 < source_->size()) {
        LittleEndian::put_le32(output_->data() + ReportSignalTile::IdentityBytes, size);
        output_->truncate(size);
        result_ = LargeByteBuffer::freeze(std::move(output_));
    }
#ifndef ARDUINO
    deflateEnd(&c);
#endif
    Memory::free(state_);
    state_ = nullptr;
    output_.reset();
    source_.reset();
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportSignalTileEncoder::take_result() {
    return std::move(result_);
}

void ReportSignalTileEncoder::reset() {
#ifndef ARDUINO
    if (state_) deflateEnd(&static_cast<EncoderState *>(state_)->compressor);
#endif
    Memory::free(state_);
    state_ = nullptr;
    consumed_ = 0;
    succeeded_ = true;
    source_.reset();
    output_.reset();
    result_.reset();
}

}  // namespace aircannect
