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

struct SignalFileLayout {
    uint32_t samples_per_block = 0;
    uint32_t raw_bytes = 0;
    uint32_t one_second_offset = 0;
    uint32_t one_second_bytes = 0;
    uint32_t one_second_cells = 0;
    uint32_t ten_second_offset = 0;
    uint32_t ten_second_bytes = 0;
    uint32_t ten_second_cells = 0;
    size_t total_bytes = 0;
};

bool layout(const ReportSignalStoreTrack &track, SignalFileLayout &result) {
    if (!report_signal_store_track_valid(track)) return false;

    result = {};
    result.samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / track.sample_interval_ms);
    size_t raw_size = 0;
    if (!CheckedSize::multiply(result.samples_per_block,
                               sizeof(int16_t),
                               raw_size) ||
        raw_size > UINT32_MAX) {
        return false;
    }
    result.raw_bytes = static_cast<uint32_t>(raw_size);

    size_t offset = ReportSignalStoreFileCodec::HeaderBytes;
    if (!CheckedSize::add_array(offset,
                                track.present_block_count,
                                raw_size)) {
        return false;
    }

    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_1S) != 0) {
        if (offset > UINT32_MAX) return false;
        result.one_second_offset = static_cast<uint32_t>(offset);
        result.one_second_cells = static_cast<uint32_t>(
            REPORT_SIGNAL_STORE_BLOCK_MS / 1000);
        const size_t block_bytes =
            static_cast<size_t>(result.one_second_cells) *
            sizeof(int16_t) * 2;
        if (block_bytes > UINT32_MAX ||
            !CheckedSize::add_array(offset,
                                    track.present_block_count,
                                    block_bytes)) {
            return false;
        }
        result.one_second_bytes = static_cast<uint32_t>(block_bytes);
    }

    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_10S) != 0) {
        if (offset > UINT32_MAX) return false;
        result.ten_second_offset = static_cast<uint32_t>(offset);
        result.ten_second_cells = static_cast<uint32_t>(
            REPORT_SIGNAL_STORE_BLOCK_MS / 10000);
        const size_t block_bytes =
            static_cast<size_t>(result.ten_second_cells) *
            sizeof(int16_t) * 2;
        if (block_bytes > UINT32_MAX ||
            !CheckedSize::add_array(offset,
                                    track.present_block_count,
                                    block_bytes)) {
            return false;
        }
        result.ten_second_bytes = static_cast<uint32_t>(block_bytes);
    }

    result.total_bytes = offset;
    return result.total_bytes <= UINT32_MAX;
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

}  // namespace

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreFileCodec::encode(
    const ReportSignalStoreFileData &data) {
    SignalFileLayout file_layout;
    if (!layout(data.track, file_layout)) return {};

    if (!data.raw_block_slots ||
        data.raw_block_slot_count != data.track.block_slot_count) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(file_layout.total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, file_layout.total_bytes);
    memcpy(bytes, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(bytes + 8, Version);
    put_le16(bytes + 10, HeaderBytes);
    put_le32(bytes + 12, static_cast<uint32_t>(file_layout.total_bytes));
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
    put_le32(bytes + 52, file_layout.samples_per_block);
    put_i64(bytes + 56, data.track.first_block_start_ms);
    put_le32(bytes + 64, data.track.grid_phase_ms);
    put_le16(bytes + 68, data.track.present_block_count);
    put_le32(bytes + 72, HeaderBytes);
    put_le32(bytes + 76, file_layout.raw_bytes);
    put_le32(bytes + 80, file_layout.one_second_offset);
    put_le32(bytes + 84, file_layout.one_second_bytes);
    put_le32(bytes + 88, file_layout.one_second_cells ? 1000 : 0);
    put_le32(bytes + 92, file_layout.ten_second_offset);
    put_le32(bytes + 96, file_layout.ten_second_bytes);
    put_le32(bytes + 100, file_layout.ten_second_cells ? 10000 : 0);
    put_i64(bytes + 112, data.track.first_valid_sample_ms);
    put_i64(bytes + 120, data.track.last_valid_sample_ms);
    put_le64(bytes + 128, data.track.valid_sample_count);
    put_le64(bytes + 136, data.track.expected_sample_count);
    memcpy(bytes + PRESENT_BITMAP_OFFSET,
           data.track.present_blocks,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);

    size_t packed = 0;
    for (size_t slot = 0; slot < data.track.block_slot_count; ++slot) {
        const int16_t *raw = data.raw_block_slots[slot];
        if (!bit(data.track.present_blocks, slot)) {
            if (raw) return {};
            continue;
        }
        if (!raw) return {};

        uint8_t *raw_block =
            bytes + HeaderBytes + packed * file_layout.raw_bytes;
        for (uint32_t i = 0; i < file_layout.samples_per_block; ++i) {
            put_i16(raw_block + static_cast<size_t>(i) * 2, raw[i]);
        }

        const int64_t block_start = data.track.first_block_start_ms +
            static_cast<int64_t>(slot) * REPORT_SIGNAL_STORE_BLOCK_MS;
        if (file_layout.one_second_cells != 0) {
            build_envelopes(data.track,
                            block_start,
                            raw,
                            file_layout.samples_per_block,
                            bytes + file_layout.one_second_offset +
                                packed * file_layout.one_second_bytes,
                            1000,
                            file_layout.one_second_cells);
        }
        if (file_layout.ten_second_cells != 0) {
            build_envelopes(data.track,
                            block_start,
                            raw,
                            file_layout.samples_per_block,
                            bytes + file_layout.ten_second_offset +
                                packed * file_layout.ten_second_bytes,
                            10000,
                            file_layout.ten_second_cells);
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
        get_le32(bytes + 72) != HeaderBytes) {
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

    SignalFileLayout file_layout;
    if (!layout(track, file_layout) ||
        file_layout.total_bytes != length ||
        get_le32(bytes + 52) != file_layout.samples_per_block ||
        get_le32(bytes + 76) != file_layout.raw_bytes ||
        get_le32(bytes + 80) != file_layout.one_second_offset ||
        get_le32(bytes + 84) != file_layout.one_second_bytes ||
        get_le32(bytes + 88) !=
            (file_layout.one_second_cells ? 1000u : 0u) ||
        get_le32(bytes + 92) != file_layout.ten_second_offset ||
        get_le32(bytes + 96) != file_layout.ten_second_bytes ||
        get_le32(bytes + 100) !=
            (file_layout.ten_second_cells ? 10000u : 0u)) {
        return false;
    }

    view.track = track;
    view.bytes = bytes;
    view.length = length;
    view.raw_plane_offset = HeaderBytes;
    view.raw_plane_bytes = file_layout.raw_bytes;
    view.one_second_plane_offset = file_layout.one_second_offset;
    view.one_second_plane_bytes = file_layout.one_second_bytes;
    view.one_second_cell_count = file_layout.one_second_cells;
    view.ten_second_plane_offset = file_layout.ten_second_offset;
    view.ten_second_plane_bytes = file_layout.ten_second_bytes;
    view.ten_second_cell_count = file_layout.ten_second_cells;
    view.samples_per_block = file_layout.samples_per_block;
    return true;
}

bool ReportSignalStoreFileCodec::file_size(
    const ReportSignalStoreTrack &track,
    size_t &size) {
    SignalFileLayout file_layout;
    if (!layout(track, file_layout)) {
        size = 0;
        return false;
    }

    size = file_layout.total_bytes;
    return true;
}

bool ReportSignalStoreFileCodec::plane_range(
    const ReportSignalStoreFileView &view,
    int64_t block_start_ms,
    ReportSignalStoreLevel level,
    ReportSignalStorePlaneRange &range) {
    if (!view.bytes || view.length < HeaderBytes ||
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
    if (block_count == 0 || block_count > track.block_slot_count) {
        return false;
    }

    size_t first_slot = 0;
    if (!slot_for_block(track, first_block_start_ms, first_slot) ||
        block_count > track.block_slot_count - first_slot) {
        return false;
    }

    SignalFileLayout file_layout;
    if (!layout(track, file_layout)) return false;

    size_t plane_offset = 0;
    size_t plane_block_bytes = 0;
    uint32_t cells_per_block = 0;
    switch (level) {
        case ReportSignalStoreLevel::Raw:
            plane_offset = HeaderBytes;
            plane_block_bytes = file_layout.raw_bytes;
            range.interval_ms = track.sample_interval_ms;
            cells_per_block = file_layout.samples_per_block;
            break;
        case ReportSignalStoreLevel::OneSecond:
            if (file_layout.one_second_cells == 0) return false;
            plane_offset = file_layout.one_second_offset;
            plane_block_bytes = file_layout.one_second_bytes;
            range.interval_ms = 1000;
            cells_per_block = file_layout.one_second_cells;
            range.envelope = true;
            break;
        case ReportSignalStoreLevel::TenSeconds:
            if (file_layout.ten_second_cells == 0) return false;
            plane_offset = file_layout.ten_second_offset;
            plane_block_bytes = file_layout.ten_second_bytes;
            range.interval_ms = 10000;
            cells_per_block = file_layout.ten_second_cells;
            range.envelope = true;
            break;
    }

    const size_t ordinal = count_bits(track.present_blocks, first_slot);
    size_t present_in_range = 0;
    for (size_t slot = first_slot;
         slot < first_slot + block_count;
         ++slot) {
        if (bit(track.present_blocks, slot)) ++present_in_range;
    }

    size_t block_offset = 0;
    if (!CheckedSize::multiply(ordinal,
                               plane_block_bytes,
                               block_offset) ||
        !CheckedSize::add_to(block_offset, plane_offset) ||
        !CheckedSize::multiply(present_in_range,
                               plane_block_bytes,
                               range.length) ||
        block_offset > file_layout.total_bytes ||
        range.length > file_layout.total_bytes - block_offset ||
        present_in_range > UINT32_MAX / cells_per_block) {
        range = {};
        return false;
    }
    range.offset = block_offset;
    range.cell_count = static_cast<uint32_t>(present_in_range) *
        cells_per_block;
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
