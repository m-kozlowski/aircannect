#include "report_signal_store.h"

#include <algorithm>
#include <limits.h>
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

void put_i32(uint8_t *out, int32_t value) {
    put_le32(out, static_cast<uint32_t>(value));
}

void put_i64(uint8_t *out, int64_t value) {
    put_le64(out, static_cast<uint64_t>(value));
}

int16_t get_i16(const uint8_t *in) {
    return static_cast<int16_t>(get_le16(in));
}

int32_t get_i32(const uint8_t *in) {
    return static_cast<int32_t>(get_le32(in));
}

int64_t get_i64(const uint8_t *in) {
    return static_cast<int64_t>(get_le64(in));
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

bool layout(const ReportSignalStoreTrack &track,
            uint32_t &samples_per_block,
            uint32_t &raw_bytes,
            uint32_t &one_second_offset,
            uint32_t &one_second_cells,
            uint32_t &ten_second_offset,
            uint32_t &ten_second_cells,
            uint32_t &block_stride,
            size_t &total_bytes) {
    if (!report_signal_store_track_valid(track)) return false;

    samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / track.sample_interval_ms);
    size_t raw_size = 0;
    if (!CheckedSize::multiply(samples_per_block,
                               sizeof(int16_t),
                               raw_size) ||
        raw_size > UINT32_MAX) {
        return false;
    }
    raw_bytes = static_cast<uint32_t>(raw_size);

    size_t stride = raw_size;
    one_second_offset = 0;
    one_second_cells = 0;
    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_1S) != 0) {
        one_second_offset = static_cast<uint32_t>(stride);
        one_second_cells = static_cast<uint32_t>(
            REPORT_SIGNAL_STORE_BLOCK_MS / 1000);
        if (!CheckedSize::add_array(stride,
                                    one_second_cells,
                                    sizeof(int16_t) * 2)) {
            return false;
        }
    }

    ten_second_offset = 0;
    ten_second_cells = 0;
    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_10S) != 0) {
        if (stride > UINT32_MAX) return false;
        ten_second_offset = static_cast<uint32_t>(stride);
        ten_second_cells = static_cast<uint32_t>(
            REPORT_SIGNAL_STORE_BLOCK_MS / 10000);
        if (!CheckedSize::add_array(stride,
                                    ten_second_cells,
                                    sizeof(int16_t) * 2)) {
            return false;
        }
    }
    if (stride == 0 || stride > UINT32_MAX) return false;
    block_stride = static_cast<uint32_t>(stride);

    size_t data_bytes = 0;
    return CheckedSize::multiply(track.present_block_count,
                                 stride,
                                 data_bytes) &&
           CheckedSize::add(ReportSignalStoreFileCodec::HeaderBytes,
                            data_bytes,
                            total_bytes) &&
           total_bytes <= UINT32_MAX;
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

void initialize_envelopes(uint8_t *out, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        put_i16(out + static_cast<size_t>(i) * 4,
                REPORT_SIGNAL_STORE_MISSING_S16);
        put_i16(out + static_cast<size_t>(i) * 4 + 2,
                REPORT_SIGNAL_STORE_MISSING_S16);
    }
}

void add_envelope(uint8_t *out, uint32_t index, int16_t value) {
    uint8_t *cell = out + static_cast<size_t>(index) * 4;
    int16_t minimum = get_i16(cell);
    int16_t maximum = get_i16(cell + 2);
    if (minimum == REPORT_SIGNAL_STORE_MISSING_S16) {
        minimum = value;
        maximum = value;
    } else {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    put_i16(cell, minimum);
    put_i16(cell + 2, maximum);
}

void build_envelopes(const ReportSignalStoreTrack &track,
                     int64_t block_start_ms,
                     const int16_t *raw,
                     uint32_t raw_count,
                     uint8_t *out,
                     uint32_t interval_ms,
                     uint32_t cell_count) {
    initialize_envelopes(out, cell_count);
    const int64_t first_ms = first_sample_in_block(track, block_start_ms);

    for (uint32_t i = 0; i < raw_count; ++i) {
        const int16_t value = raw[i];
        if (value == REPORT_SIGNAL_STORE_MISSING_S16) continue;

        const int64_t timestamp =
            first_ms + static_cast<int64_t>(i) * track.sample_interval_ms;
        if (timestamp < block_start_ms ||
            timestamp >= block_start_ms + REPORT_SIGNAL_STORE_BLOCK_MS) {
            continue;
        }
        const uint32_t cell = static_cast<uint32_t>(
            (timestamp - block_start_ms) / interval_ms);
        if (cell < cell_count) add_envelope(out, cell, value);
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

bool packed_ordinal(const ReportSignalStoreTrack &track,
                    size_t slot,
                    size_t &ordinal) {
    if (slot >= track.block_slot_count ||
        !bit(track.present_blocks, slot)) {
        return false;
    }

    ordinal = count_bits(track.present_blocks, slot);
    return ordinal < track.present_block_count;
}

}  // namespace

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreFileCodec::encode(
    const ReportSignalStoreFileData &data) {
    uint32_t samples_per_block = 0;
    uint32_t raw_bytes = 0;
    uint32_t one_second_offset = 0;
    uint32_t one_second_cells = 0;
    uint32_t ten_second_offset = 0;
    uint32_t ten_second_cells = 0;
    uint32_t block_stride = 0;
    size_t total_bytes = 0;
    if (!layout(data.track,
                samples_per_block,
                raw_bytes,
                one_second_offset,
                one_second_cells,
                ten_second_offset,
                ten_second_cells,
                block_stride,
                total_bytes)) {
        return {};
    }

    size_t expected_values = 0;
    if (!CheckedSize::multiply(data.track.present_block_count,
                               samples_per_block,
                               expected_values) ||
        !data.raw_blocks || data.raw_value_count != expected_values) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    memcpy(bytes, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(bytes + 8, Version);
    put_le16(bytes + 10, HeaderBytes);
    put_le32(bytes + 12, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 16, data.track.sleep_day.epoch_days());
    put_le32(bytes + 20, data.track.generation);
    put_le64(bytes + 24, data.track.source_revision.value());
    bytes[32] = static_cast<uint8_t>(data.track.signal);
    bytes[33] = static_cast<uint8_t>(data.track.encoding);
    bytes[34] = static_cast<uint8_t>(data.track.unit);
    bytes[35] = data.track.lod_mask;
    put_le16(bytes + 36, data.track.track_index);
    put_le16(bytes + 38, data.track.block_slot_count);
    put_le32(bytes + 40, data.track.sample_interval_ms);
    put_le32(bytes + 44, data.track.value_scale_milli);
    put_le32(bytes + 48,
             static_cast<uint32_t>(REPORT_SIGNAL_STORE_BLOCK_MS));
    put_le32(bytes + 52, samples_per_block);
    put_i64(bytes + 56, data.track.first_block_start_ms);
    put_le32(bytes + 64, data.track.grid_phase_ms);
    put_le16(bytes + 68, data.track.present_block_count);
    put_le32(bytes + 72, HeaderBytes);
    put_le32(bytes + 76, block_stride);
    put_le32(bytes + 80, 0);
    put_le32(bytes + 84, raw_bytes);
    put_le32(bytes + 88, one_second_offset);
    put_le32(bytes + 92, one_second_cells);
    put_le32(bytes + 96, one_second_cells ? 1000 : 0);
    put_le32(bytes + 100, ten_second_offset);
    put_le32(bytes + 104, ten_second_cells);
    put_le32(bytes + 108, ten_second_cells ? 10000 : 0);
    put_i64(bytes + 112, data.track.first_valid_sample_ms);
    put_i64(bytes + 120, data.track.last_valid_sample_ms);
    put_le64(bytes + 128, data.track.valid_sample_count);
    put_le64(bytes + 136, data.track.expected_sample_count);
    memcpy(bytes + PRESENT_BITMAP_OFFSET,
           data.track.present_blocks,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);

    size_t packed = 0;
    for (size_t slot = 0; slot < data.track.block_slot_count; ++slot) {
        if (!bit(data.track.present_blocks, slot)) continue;

        uint8_t *block = bytes + HeaderBytes + packed * block_stride;
        const int16_t *raw =
            data.raw_blocks + packed * samples_per_block;
        for (uint32_t i = 0; i < samples_per_block; ++i) {
            put_i16(block + static_cast<size_t>(i) * 2, raw[i]);
        }

        const int64_t block_start = data.track.first_block_start_ms +
            static_cast<int64_t>(slot) * REPORT_SIGNAL_STORE_BLOCK_MS;
        if (one_second_cells != 0) {
            build_envelopes(data.track,
                            block_start,
                            raw,
                            samples_per_block,
                            block + one_second_offset,
                            1000,
                            one_second_cells);
        }
        if (ten_second_cells != 0) {
            build_envelopes(data.track,
                            block_start,
                            raw,
                            samples_per_block,
                            block + ten_second_offset,
                            10000,
                            ten_second_cells);
        }
        ++packed;
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
        get_le32(bytes + 72) != HeaderBytes ||
        get_le32(bytes + 80) != 0) {
        return false;
    }

    ReportSignalStoreTrack track;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 16),
                                     track.sleep_day)) {
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
    track.value_scale_milli = get_le32(bytes + 44);
    track.first_block_start_ms = get_i64(bytes + 56);
    track.grid_phase_ms = get_le32(bytes + 64);
    track.present_block_count = get_le16(bytes + 68);
    track.first_valid_sample_ms = get_i64(bytes + 112);
    track.last_valid_sample_ms = get_i64(bytes + 120);
    track.valid_sample_count = get_le64(bytes + 128);
    track.expected_sample_count = get_le64(bytes + 136);
    memcpy(track.present_blocks,
           bytes + PRESENT_BITMAP_OFFSET,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);
    if (!report_signal_store_track_valid(track)) return false;

    uint32_t samples_per_block = 0;
    uint32_t raw_bytes = 0;
    uint32_t one_second_offset = 0;
    uint32_t one_second_cells = 0;
    uint32_t ten_second_offset = 0;
    uint32_t ten_second_cells = 0;
    uint32_t block_stride = 0;
    size_t expected_length = 0;
    if (!layout(track,
                samples_per_block,
                raw_bytes,
                one_second_offset,
                one_second_cells,
                ten_second_offset,
                ten_second_cells,
                block_stride,
                expected_length) ||
        expected_length != length ||
        get_le32(bytes + 52) != samples_per_block ||
        get_le32(bytes + 76) != block_stride ||
        get_le32(bytes + 84) != raw_bytes ||
        get_le32(bytes + 88) != one_second_offset ||
        get_le32(bytes + 92) != one_second_cells ||
        get_le32(bytes + 96) != (one_second_cells ? 1000u : 0u) ||
        get_le32(bytes + 100) != ten_second_offset ||
        get_le32(bytes + 104) != ten_second_cells ||
        get_le32(bytes + 108) != (ten_second_cells ? 10000u : 0u)) {
        return false;
    }

    view.track = track;
    view.bytes = bytes;
    view.length = length;
    view.data_offset = HeaderBytes;
    view.block_stride = block_stride;
    view.raw_plane_offset = 0;
    view.raw_plane_bytes = raw_bytes;
    view.one_second_plane_offset = one_second_offset;
    view.one_second_cell_count = one_second_cells;
    view.ten_second_plane_offset = ten_second_offset;
    view.ten_second_cell_count = ten_second_cells;
    view.samples_per_block = samples_per_block;
    return true;
}

bool ReportSignalStoreFileCodec::plane_range(
    const ReportSignalStoreFileView &view,
    int64_t block_start_ms,
    ReportSignalStoreLevel level,
    ReportSignalStorePlaneRange &range) {
    range = {};
    if (!view.bytes || view.length < HeaderBytes || view.block_stride == 0) {
        return false;
    }

    size_t slot = 0;
    size_t ordinal = 0;
    if (!slot_for_block(view.track, block_start_ms, slot) ||
        !packed_ordinal(view.track, slot, ordinal)) {
        return false;
    }

    size_t plane_offset = 0;
    switch (level) {
        case ReportSignalStoreLevel::Raw:
            plane_offset = view.raw_plane_offset;
            range.length = view.raw_plane_bytes;
            range.interval_ms = view.track.sample_interval_ms;
            range.cell_count = view.samples_per_block;
            break;
        case ReportSignalStoreLevel::OneSecond:
            if (view.one_second_cell_count == 0) return false;
            plane_offset = view.one_second_plane_offset;
            range.length =
                static_cast<size_t>(view.one_second_cell_count) * 4;
            range.interval_ms = 1000;
            range.cell_count = view.one_second_cell_count;
            range.envelope = true;
            break;
        case ReportSignalStoreLevel::TenSeconds:
            if (view.ten_second_cell_count == 0) return false;
            plane_offset = view.ten_second_plane_offset;
            range.length =
                static_cast<size_t>(view.ten_second_cell_count) * 4;
            range.interval_ms = 10000;
            range.cell_count = view.ten_second_cell_count;
            range.envelope = true;
            break;
    }

    size_t block_offset = 0;
    if (!CheckedSize::multiply(ordinal,
                               view.block_stride,
                               block_offset) ||
        !CheckedSize::add_to(block_offset, view.data_offset) ||
        !CheckedSize::add_to(block_offset, plane_offset) ||
        block_offset > view.length ||
        range.length > view.length - block_offset) {
        range = {};
        return false;
    }
    range.offset = block_offset;
    return true;
}

bool ReportSignalStoreFileCodec::sample(
    const ReportSignalStoreFileView &view,
    int64_t timestamp_ms,
    bool &present,
    int32_t &value_milli) {
    present = false;
    value_milli = 0;
    if (!view.bytes || timestamp_ms < view.track.first_block_start_ms ||
        view.track.sample_interval_ms == 0) {
        return false;
    }

    const int64_t block_start = timestamp_ms -
        timestamp_ms % REPORT_SIGNAL_STORE_BLOCK_MS;
    ReportSignalStorePlaneRange plane;
    if (!plane_range(view,
                     block_start,
                     ReportSignalStoreLevel::Raw,
                     plane)) {
        return false;
    }

    const int64_t first_ms = first_sample_in_block(view.track, block_start);
    if (timestamp_ms < first_ms) return false;
    const int64_t delta = timestamp_ms - first_ms;
    if ((delta % view.track.sample_interval_ms) != 0) return false;

    const uint64_t index =
        static_cast<uint64_t>(delta / view.track.sample_interval_ms);
    if (index >= view.samples_per_block) return false;

    const int16_t encoded = get_i16(
        view.bytes + plane.offset + static_cast<size_t>(index) * 2);
    if (encoded == REPORT_SIGNAL_STORE_MISSING_S16) return true;

    present = true;
    value_milli = static_cast<int32_t>(encoded) *
        static_cast<int32_t>(view.track.value_scale_milli);
    return true;
}

}  // namespace aircannect
