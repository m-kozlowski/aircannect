#include "report_signal_store.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "checked_size.h"
#include "little_endian.h"

namespace aircannect {
namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

constexpr uint8_t FILE_MAGIC[8] = {
    'A', 'C', 'R', 'S', 'I', 'G', '0', '1',
};
constexpr size_t PRESENT_BITMAP_OFFSET = 144;

void put_i16(uint8_t *out, int16_t value) {
    put_le16(out, static_cast<uint16_t>(value));
}

int16_t get_i16(const uint8_t *in) {
    return static_cast<int16_t>(get_le16(in));
}

bool bit(const uint8_t *bitmap, size_t index) {
    return (bitmap[index / 8] & (1u << (index % 8))) != 0;
}

size_t count_bits(const uint8_t *bitmap, size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (bit(bitmap, i)) ++total;
    }
    return total;
}

struct SignalFileLayout {
    uint32_t samples_per_block = 0;
    uint32_t block_bytes = 0;
    uint32_t interval_ms = 0;
    uint32_t cell_count = 0;
    size_t total_bytes = 0;
};

bool layout(const ReportSignalStoreTrack &track,
            ReportSignalStoreLevel level,
            SignalFileLayout &result) {
    result = {};
    if (!report_signal_store_track_valid(track)) return false;

    result.samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / track.sample_interval_ms);

    switch (level) {
        case ReportSignalStoreLevel::Raw:
            result.interval_ms = track.sample_interval_ms;
            break;
        case ReportSignalStoreLevel::OneSecond:
            if (!(track.lod_mask & REPORT_SIGNAL_STORE_LOD_1S)) return false;
            result.interval_ms = 1000;
            break;
        case ReportSignalStoreLevel::TenSeconds:
            if (!(track.lod_mask & REPORT_SIGNAL_STORE_LOD_10S)) return false;
            result.interval_ms = 10000;
            break;
        default:
            return false;
    }

    result.cell_count = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / result.interval_ms);
    const size_t cell_bytes = level == ReportSignalStoreLevel::Raw ? 2 : 4;
    size_t block_bytes = 0;

    if (!CheckedSize::multiply(result.cell_count, cell_bytes, block_bytes) ||
        block_bytes > ReportSignalStoreFileCodec::MaxBlockBytes) {
        return false;
    }

    result.block_bytes = static_cast<uint32_t>(block_bytes);
    result.total_bytes = ReportSignalStoreFileCodec::HeaderBytes;
    return CheckedSize::add_array(result.total_bytes,
                                   track.present_block_count,
                                   block_bytes) &&
           result.total_bytes <= UINT32_MAX;
}

int64_t first_sample_in_block(const ReportSignalStoreTrack &track,
                              int64_t block_start_ms) {
    const int64_t interval = track.sample_interval_ms;
    int64_t remainder = block_start_ms % interval;
    if (remainder < 0) remainder += interval;

    int64_t delta = static_cast<int64_t>(track.grid_phase_ms) - remainder;
    if (delta < 0) delta += interval;
    return block_start_ms + delta;
}

void build_envelopes(const ReportSignalStoreTrack &track,
                     int64_t block_start_ms,
                     const int16_t *raw,
                     const SignalFileLayout &file_layout,
                     uint8_t *out,
                     const uint8_t *one_second) {
    const int64_t phase = first_sample_in_block(track, block_start_ms) -
        block_start_ms;
    uint32_t cursor = 0;
    for (uint32_t cell = 0; cell < file_layout.cell_count; ++cell) {
        int16_t minimum = track.missing_value;
        int16_t maximum = track.missing_value;
        const int64_t end_ms =
            static_cast<int64_t>(cell + 1) * file_layout.interval_ms;
        const uint32_t end = one_second ? (cell + 1) * 10
            : static_cast<uint32_t>(std::min<int64_t>(
                file_layout.samples_per_block,
                std::max<int64_t>(0, (end_ms - phase +
                    track.sample_interval_ms - 1) / track.sample_interval_ms)));

        for (; cursor < end; ++cursor) {
            const int16_t low = one_second
                ? get_i16(one_second + static_cast<size_t>(cursor) * 4)
                : raw[cursor];
            const int16_t high = one_second
                ? get_i16(one_second + static_cast<size_t>(cursor) * 4 + 2)
                : low;
            if (low == track.missing_value || high == track.missing_value) continue;

            minimum = minimum == track.missing_value ? low : std::min(minimum, low);
            maximum = maximum == track.missing_value ? high : std::max(maximum, high);
        }

        put_i16(out + static_cast<size_t>(cell) * 4, minimum);
        put_i16(out + static_cast<size_t>(cell) * 4 + 2, maximum);
    }
}

bool slot_for_block(const ReportSignalStoreTrack &track,
                    int64_t block_start_ms,
                    size_t &slot) {
    if (block_start_ms < track.first_block_start_ms ||
        (block_start_ms % REPORT_SIGNAL_STORE_BLOCK_MS) != 0) {
        return false;
    }

    const int64_t delta = block_start_ms - track.first_block_start_ms;
    const int64_t candidate = delta / REPORT_SIGNAL_STORE_BLOCK_MS;
    if (candidate < 0 || candidate >= track.block_slot_count) return false;

    slot = static_cast<size_t>(candidate);
    return true;
}

}  // namespace

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreFileCodec::encode_header(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level) {
    SignalFileLayout file_layout;
    if (!layout(track, level, file_layout)) return {};

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(HeaderBytes);
    if (!output) return {};

    uint32_t scale_bits = 0;
    uint32_t offset_bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
    memcpy(&scale_bits, &track.value_scale, sizeof(scale_bits));
    memcpy(&offset_bits, &track.value_offset, sizeof(offset_bits));

    uint8_t *bytes = output->data();
    memset(bytes, 0, HeaderBytes);
    memcpy(bytes, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(bytes + 8, Version);
    put_le16(bytes + 10, HeaderBytes);
    put_le32(bytes + 12, static_cast<uint32_t>(file_layout.total_bytes));
    put_le32(bytes + 16, static_cast<uint32_t>(track.sleep_day.epoch_days()));
    put_le32(bytes + 20, track.generation);
    put_le64(bytes + 24, track.source_revision.value());
    bytes[32] = static_cast<uint8_t>(track.signal);
    bytes[33] = static_cast<uint8_t>(track.encoding);
    bytes[34] = static_cast<uint8_t>(track.unit);
    bytes[35] = track.lod_mask;
    put_le16(bytes + 36, track.track_index);
    put_le16(bytes + 38, track.block_slot_count);
    put_le32(bytes + 40, track.sample_interval_ms);
    put_le32(bytes + 44, scale_bits);
    put_le32(bytes + 48, static_cast<uint32_t>(REPORT_SIGNAL_STORE_BLOCK_MS));
    put_le32(bytes + 52, file_layout.samples_per_block);
    put_le64(bytes + 56, static_cast<uint64_t>(track.first_block_start_ms));
    put_le32(bytes + 64, track.grid_phase_ms);
    put_le16(bytes + 68, track.present_block_count);
    bytes[70] = static_cast<uint8_t>(level);
    put_le32(bytes + 72, HeaderBytes);
    put_le32(bytes + 76, file_layout.block_bytes);
    put_le32(bytes + 80, file_layout.interval_ms);
    put_le32(bytes + 84, file_layout.cell_count);
    put_le64(bytes + 112, static_cast<uint64_t>(track.first_valid_sample_ms));
    put_le64(bytes + 120, static_cast<uint64_t>(track.last_valid_sample_ms));
    put_le64(bytes + 128, track.valid_sample_count);
    put_le64(bytes + 136, track.expected_sample_count);
    memcpy(bytes + PRESENT_BITMAP_OFFSET,
           track.present_blocks,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);
    put_le32(bytes + 160, offset_bits);
    put_i16(bytes + 164, track.missing_value);

    return LargeByteBuffer::freeze(std::move(output));
}

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreFileCodec::encode_block(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level,
    size_t slot,
    const int16_t *raw) {
    const int16_t *raw_blocks[] = {raw};
    return encode_blocks(track, level, slot, raw_blocks, 1);
}

std::shared_ptr<const LargeByteBuffer>
ReportSignalStoreFileCodec::encode_blocks(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level,
    size_t first_slot,
    const int16_t *const *raw_blocks,
    size_t block_count,
    bool include_header,
    const LargeByteBuffer *one_second) {
    SignalFileLayout file_layout;
    if (!raw_blocks || !layout(track, level, file_layout) ||
        block_count == 0 || first_slot >= track.block_slot_count ||
        block_count > track.block_slot_count - first_slot ||
        (include_header && count_bits(track.present_blocks, first_slot) != 0)) {
        return {};
    }

    size_t total_bytes = 0;
    if (!CheckedSize::multiply(block_count, file_layout.block_bytes,
                               total_bytes)) {
        return {};
    }
    const size_t prefix_bytes = include_header ? HeaderBytes : 0;
    if (!CheckedSize::add_array(total_bytes, prefix_bytes, 1)) return {};

    const uint8_t *one_second_body = nullptr;
    SignalFileLayout one_second_layout;
    if (one_second) {
        size_t body_bytes = 0;
        if (level != ReportSignalStoreLevel::TenSeconds ||
            !layout(track, ReportSignalStoreLevel::OneSecond, one_second_layout) ||
            !CheckedSize::multiply(block_count, one_second_layout.block_bytes,
                                   body_bytes)) return {};

        const size_t size = one_second->size();
        if (size != body_bytes &&
            (size < HeaderBytes || size - HeaderBytes != body_bytes)) return {};
        one_second_body = one_second->data() + (size - body_bytes);
    }

    for (size_t i = 0; i < block_count; ++i) {
        if (!raw_blocks[i] || !bit(track.present_blocks, first_slot + i)) {
            return {};
        }
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    if (include_header) {
        const auto header = encode_header(track, level);
        if (!header) return {};
        memcpy(output->data(), header->data(), HeaderBytes);
    }

    for (size_t block = 0; block < block_count; ++block) {
        uint8_t *block_output = output->data() +
            prefix_bytes + block * file_layout.block_bytes;
        if (level == ReportSignalStoreLevel::Raw) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            memcpy(block_output, raw_blocks[block], file_layout.block_bytes);
#else
            for (uint32_t i = 0; i < file_layout.samples_per_block; ++i) {
                put_i16(block_output + static_cast<size_t>(i) * 2,
                        raw_blocks[block][i]);
            }
#endif
        } else {
            const int64_t block_start = track.first_block_start_ms +
                static_cast<int64_t>(first_slot + block) *
                    REPORT_SIGNAL_STORE_BLOCK_MS;
            build_envelopes(track, block_start, raw_blocks[block],
                            file_layout, block_output,
                            one_second_body ? one_second_body +
                                block * one_second_layout.block_bytes : nullptr);
        }
    }

    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportSignalStoreFileCodec::inspect(
    const uint8_t *bytes,
    size_t length,
    ReportSignalStoreFileView &view) {
    view = {};
    if (!bytes || length < HeaderBytes ||
        memcmp(bytes, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0 ||
        get_le16(bytes + 8) != Version ||
        get_le16(bytes + 10) != HeaderBytes ||
        get_le32(bytes + 12) != length ||
        get_le32(bytes + 48) != REPORT_SIGNAL_STORE_BLOCK_MS ||
        get_le32(bytes + 72) != HeaderBytes) {
        return false;
    }

    ReportSignalStoreTrack track;
    if (!SleepDayId::from_epoch_days(
            static_cast<int32_t>(get_le32(bytes + 16)), track.sleep_day)) {
        return false;
    }

    track.generation = get_le32(bytes + 20);
    track.source_revision = SourceRevision(get_le64(bytes + 24));
    track.signal = static_cast<ReportSignalId>(bytes[32]);
    track.encoding = static_cast<ReportSignalStoreEncoding>(bytes[33]);
    track.unit = static_cast<ReportSignalStoreUnit>(bytes[34]);
    track.lod_mask = bytes[35];
    track.track_index = get_le16(bytes + 36);
    track.block_slot_count = get_le16(bytes + 38);
    track.sample_interval_ms = get_le32(bytes + 40);
    track.first_block_start_ms = static_cast<int64_t>(get_le64(bytes + 56));
    track.grid_phase_ms = get_le32(bytes + 64);
    track.present_block_count = get_le16(bytes + 68);
    track.first_valid_sample_ms = static_cast<int64_t>(get_le64(bytes + 112));
    track.last_valid_sample_ms = static_cast<int64_t>(get_le64(bytes + 120));
    track.valid_sample_count = get_le64(bytes + 128);
    track.expected_sample_count = get_le64(bytes + 136);
    memcpy(track.present_blocks,
           bytes + PRESENT_BITMAP_OFFSET,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);

    const uint32_t scale_bits = get_le32(bytes + 44);
    const uint32_t offset_bits = get_le32(bytes + 160);
    memcpy(&track.value_scale, &scale_bits, sizeof(scale_bits));
    memcpy(&track.value_offset, &offset_bits, sizeof(offset_bits));
    track.missing_value = get_i16(bytes + 164);

    const auto level = static_cast<ReportSignalStoreLevel>(bytes[70]);
    SignalFileLayout file_layout;
    if (!layout(track, level, file_layout) ||
        file_layout.total_bytes != length ||
        get_le32(bytes + 52) != file_layout.samples_per_block ||
        get_le32(bytes + 76) != file_layout.block_bytes ||
        get_le32(bytes + 80) != file_layout.interval_ms ||
        get_le32(bytes + 84) != file_layout.cell_count) {
        return false;
    }

    view.track = track;
    view.level = level;
    view.bytes = bytes;
    view.length = length;
    view.plane_block_bytes = file_layout.block_bytes;
    view.plane_interval_ms = file_layout.interval_ms;
    view.plane_cell_count = file_layout.cell_count;
    view.samples_per_block = file_layout.samples_per_block;
    return true;
}

bool ReportSignalStoreFileCodec::file_size(
    const ReportSignalStoreTrack &track,
    ReportSignalStoreLevel level,
    size_t &size) {
    size = 0;
    SignalFileLayout file_layout;
    if (!layout(track, level, file_layout)) return false;

    size = file_layout.total_bytes;
    return true;
}

bool ReportSignalStoreFileCodec::plane_range(
    const ReportSignalStoreFileView &view,
    int64_t block_start_ms,
    ReportSignalStoreLevel level,
    ReportSignalStorePlaneRange &range) {
    if (!view.bytes || view.length < HeaderBytes || level != view.level ||
        !plane_range(view.track, block_start_ms, 1, level, range) ||
        range.length == 0 || range.offset > view.length ||
        range.length > view.length - range.offset) {
        range = {};
        return false;
    }
    return true;
}

bool ReportSignalStoreFileCodec::plane_range(
    const ReportSignalStoreTrack &track,
    int64_t first_block_start_ms,
    size_t block_count,
    ReportSignalStoreLevel level,
    ReportSignalStorePlaneRange &range) {
    range = {};
    SignalFileLayout file_layout;
    if (!layout(track, level, file_layout) ||
        block_count == 0 || block_count > track.block_slot_count) {
        return false;
    }

    size_t first_slot = 0;
    if (!slot_for_block(track, first_block_start_ms, first_slot) ||
        block_count > track.block_slot_count - first_slot) {
        return false;
    }

    const size_t ordinal = count_bits(track.present_blocks, first_slot);
    const size_t present_in_range =
        count_bits(track.present_blocks, first_slot + block_count) - ordinal;
    size_t block_offset = HeaderBytes;

    if (!CheckedSize::add_array(block_offset, ordinal, file_layout.block_bytes) ||
        !CheckedSize::multiply(present_in_range,
                               file_layout.block_bytes,
                               range.length) ||
        block_offset > file_layout.total_bytes ||
        range.length > file_layout.total_bytes - block_offset ||
        present_in_range > UINT32_MAX / file_layout.cell_count) {
        range = {};
        return false;
    }

    range.offset = block_offset;
    range.interval_ms = file_layout.interval_ms;
    range.cell_count = static_cast<uint32_t>(present_in_range) *
        file_layout.cell_count;
    range.envelope = level != ReportSignalStoreLevel::Raw;
    return true;
}

bool ReportSignalStoreFileCodec::sample_raw(
    const ReportSignalStoreFileView &view,
    int64_t timestamp_ms,
    bool &present,
    int16_t &raw) {
    present = false;
    raw = view.track.missing_value;
    if (!view.bytes || timestamp_ms < view.track.first_block_start_ms ||
        view.track.sample_interval_ms == 0) {
        return false;
    }

    const int64_t block_start = timestamp_ms -
        timestamp_ms % REPORT_SIGNAL_STORE_BLOCK_MS;
    ReportSignalStorePlaneRange plane;

    if (!plane_range(view, block_start, ReportSignalStoreLevel::Raw, plane)) {
        return false;
    }

    const int64_t first_ms = first_sample_in_block(view.track, block_start);
    if (timestamp_ms < first_ms) return false;

    const int64_t delta = timestamp_ms - first_ms;
    if ((delta % view.track.sample_interval_ms) != 0) return false;

    const uint64_t index =
        static_cast<uint64_t>(delta / view.track.sample_interval_ms);
    if (index >= plane.cell_count) return false;

    raw = get_i16(view.bytes + plane.offset + static_cast<size_t>(index) * 2);
    present = raw != view.track.missing_value;
    return true;
}

bool ReportSignalStoreFileCodec::sample(
    const ReportSignalStoreFileView &view,
    int64_t timestamp_ms,
    bool &present,
    int32_t &value_milli) {
    value_milli = 0;
    int16_t raw = view.track.missing_value;
    if (!sample_raw(view, timestamp_ms, present, raw)) return false;
    if (!present) return true;

    const double value = round(
        (static_cast<double>(raw) * view.track.value_scale +
         view.track.value_offset) * 1000.0);

    if (!isfinite(value) || value < INT32_MIN || value > INT32_MAX) {
        present = false;
        return false;
    }

    value_milli = static_cast<int32_t>(value);
    return true;
}

}  // namespace aircannect
