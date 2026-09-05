#include "report_source_progress.h"

#include <cmath>
#include <cstring>

#include "checked_size.h"
#include "little_endian.h"

namespace aircannect {
namespace {

constexpr uint32_t PROGRESS_MAGIC = 0x31505352u;
constexpr uint16_t PROGRESS_VERSION = 1;
constexpr size_t PROGRESS_HEADER_BYTES = 16;
constexpr size_t PROGRESS_ENTRY_BYTES = 112;
constexpr uint8_t PROGRESS_PRIMARY = 1u << 0;

uint32_t float_bits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32-bit");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(uint32_t bits) {
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

bool valid_storage(uint8_t value) {
    return value == static_cast<uint8_t>(ReportSourceProgressStorage::Edf) ||
           value == static_cast<uint8_t>(ReportSourceProgressStorage::Fallback);
}

bool checked_entry_bytes(size_t path_length, size_t &out) {
    return CheckedSize::add(PROGRESS_ENTRY_BYTES, path_length, out) &&
           out <= UINT32_MAX;
}

void put_i64(uint8_t *out, int64_t value) {
    LittleEndian::put_le64(out, static_cast<uint64_t>(value));
}

int64_t get_i64(const uint8_t *in) {
    return static_cast<int64_t>(LittleEndian::get_le64(in));
}

void put_float(uint8_t *out, float value) {
    LittleEndian::put_le32(out, float_bits(value));
}

bool valid_entry(const ReportSourceProgressEntry &entry) {
    if (!entry.path || entry.path_length == 0 ||
        entry.full_end_ms <= entry.mapping_start_ms ||
        entry.cursor_ms < entry.mapping_start_ms ||
        entry.cursor_ms > entry.full_end_ms ||
        entry.series.sample_interval_ms == 0 ||
        !valid_storage(static_cast<uint8_t>(entry.storage))) {
        return false;
    }

    if (entry.storage == ReportSourceProgressStorage::Edf) {
        if (entry.file_record_size == 0 ||
            entry.file_record_duration_ms == 0 ||
            entry.samples_per_record == 0 ||
            entry.scale.digital_max <= entry.scale.digital_min ||
            !std::isfinite(entry.scale.physical_min) ||
            !std::isfinite(entry.scale.physical_max) ||
            !std::isfinite(entry.scale.scale) ||
            !std::isfinite(entry.scale.offset) ||
            entry.scale.scale <= 0.0f) {
            return false;
        }
    } else if (entry.fallback_payload_schema == 0 ||
               entry.fallback_coverage_start_ms >= entry.full_end_ms) {
        return false;
    }
    return true;
}

void encode_entry(uint8_t *out, const ReportSourceProgressEntry &entry) {
    const size_t path_offset = PROGRESS_ENTRY_BYTES;
    const uint8_t flags = entry.series.primary ? PROGRESS_PRIMARY : 0;

    LittleEndian::put_le32(out, static_cast<uint32_t>(path_offset +
                                                       entry.path_length));
    LittleEndian::put_le16(out + 4, entry.path_length);
    out[6] = static_cast<uint8_t>(entry.storage);
    out[7] = flags;
    out[8] = static_cast<uint8_t>(entry.series.signal);
    out[9] = static_cast<uint8_t>(entry.series.source);
    out[10] = 0;
    out[11] = 0;
    LittleEndian::put_le32(out + 12, entry.series.sample_interval_ms);
    LittleEndian::put_le32(out + 16, entry.samples_per_record);
    LittleEndian::put_le32(out + 20, entry.byte_offset_in_record);
    LittleEndian::put_le16(
        out + 24, static_cast<uint16_t>(entry.scale.digital_min));
    LittleEndian::put_le16(
        out + 26, static_cast<uint16_t>(entry.scale.digital_max));
    put_float(out + 28, entry.scale.physical_min);
    put_float(out + 32, entry.scale.physical_max);
    put_float(out + 36, entry.scale.scale);
    put_float(out + 40, entry.scale.offset);
    put_i64(out + 44, entry.session_start_ms);
    put_i64(out + 52, entry.mapping_start_ms);
    put_i64(out + 60, entry.full_end_ms);
    put_i64(out + 68, entry.cursor_ms);
    put_i64(out + 76, entry.file_start_ms);
    LittleEndian::put_le32(out + 84, entry.file_header_size);
    LittleEndian::put_le32(out + 88, entry.file_record_size);
    LittleEndian::put_le32(out + 92, entry.file_record_duration_ms);
    LittleEndian::put_le32(out + 96, entry.fallback_payload_schema);
    put_i64(out + 100, entry.fallback_coverage_start_ms);
    LittleEndian::put_le32(out + 108, 0);
    memcpy(out + path_offset, entry.path, entry.path_length);
}

bool decode_entry(const uint8_t *data,
                  size_t length,
                  size_t offset,
                  ReportSourceProgressEntry &out,
                  size_t &next_offset) {
    if (!data || offset > length || length - offset < PROGRESS_ENTRY_BYTES) {
        return false;
    }

    const uint8_t *in = data + offset;
    const size_t entry_bytes = LittleEndian::get_le32(in);
    const size_t path_length = LittleEndian::get_le16(in + 4);
    size_t expected_bytes = 0;
    if (!checked_entry_bytes(path_length, expected_bytes) ||
        entry_bytes != expected_bytes || entry_bytes > length - offset) {
        return false;
    }

    out = {};
    out.path = reinterpret_cast<const char *>(in + PROGRESS_ENTRY_BYTES);
    out.path_length = static_cast<uint16_t>(path_length);
    if (!valid_storage(in[6]) ||
        in[8] >= static_cast<uint8_t>(ReportSignalId::Count) ||
        in[9] > static_cast<uint8_t>(ReportSourceId::OximetryOneSecond) ||
        (in[10] != 0) || (in[11] != 0) ||
        (in[7] & ~PROGRESS_PRIMARY) != 0 ||
        in[108] != 0 || in[109] != 0 || in[110] != 0 || in[111] != 0) {
        return false;
    }

    out.storage = static_cast<ReportSourceProgressStorage>(in[6]);
    out.series.signal = static_cast<ReportSignalId>(in[8]);
    out.series.source = static_cast<ReportSourceId>(in[9]);
    out.series.primary = (in[7] & PROGRESS_PRIMARY) != 0;
    out.series.sample_interval_ms = LittleEndian::get_le32(in + 12);
    out.samples_per_record = LittleEndian::get_le32(in + 16);
    out.byte_offset_in_record = LittleEndian::get_le32(in + 20);
    out.scale.digital_min = static_cast<int16_t>(
        LittleEndian::get_le16(in + 24));
    out.scale.digital_max = static_cast<int16_t>(
        LittleEndian::get_le16(in + 26));
    out.scale.physical_min = bits_float(
        LittleEndian::get_le32(in + 28));
    out.scale.physical_max = bits_float(
        LittleEndian::get_le32(in + 32));
    out.scale.scale = bits_float(LittleEndian::get_le32(in + 36));
    out.scale.offset = bits_float(LittleEndian::get_le32(in + 40));
    out.session_start_ms = get_i64(in + 44);
    out.mapping_start_ms = get_i64(in + 52);
    out.full_end_ms = get_i64(in + 60);
    out.cursor_ms = get_i64(in + 68);
    out.file_start_ms = get_i64(in + 76);
    out.file_header_size = LittleEndian::get_le32(in + 84);
    out.file_record_size = LittleEndian::get_le32(in + 88);
    out.file_record_duration_ms = LittleEndian::get_le32(in + 92);
    out.fallback_payload_schema = LittleEndian::get_le32(in + 96);
    out.fallback_coverage_start_ms = get_i64(in + 100);
    next_offset = offset + entry_bytes;
    return valid_entry(out);
}

}  // namespace

bool ReportSourceProgressReader::open(const uint8_t *data, size_t length) {
    data_ = nullptr;
    length_ = 0;
    entries_offset_ = 0;
    count_ = 0;
    if (!data || length < PROGRESS_HEADER_BYTES ||
        LittleEndian::get_le32(data) != PROGRESS_MAGIC ||
        LittleEndian::get_le16(data + 4) != PROGRESS_VERSION ||
        LittleEndian::get_le16(data + 6) != PROGRESS_HEADER_BYTES ||
        LittleEndian::get_le32(data + 12) != length) {
        return false;
    }

    const uint32_t count = LittleEndian::get_le32(data + 8);
    size_t minimum_bytes = 0;
    if (!CheckedSize::multiply(count, PROGRESS_ENTRY_BYTES, minimum_bytes) ||
        minimum_bytes > length - PROGRESS_HEADER_BYTES) {
        return false;
    }

    data_ = data;
    length_ = length;
    entries_offset_ = PROGRESS_HEADER_BYTES;
    count_ = count;
    size_t offset = entries_offset_;
    for (size_t i = 0; i < count_; ++i) {
        ReportSourceProgressEntry ignored;
        size_t next = 0;
        if (!decode_entry(data_, length_, offset, ignored, next)) {
            data_ = nullptr;
            length_ = 0;
            entries_offset_ = 0;
            count_ = 0;
            return false;
        }
        offset = next;
    }
    return offset == length_;
}

bool ReportSourceProgressReader::entry(
    size_t index, ReportSourceProgressEntry &out) const {
    if (!data_ || index >= count_) return false;

    size_t offset = entries_offset_;
    for (size_t i = 0; i < index; ++i) {
        const size_t entry_bytes = LittleEndian::get_le32(data_ + offset);
        if (entry_bytes > length_ - offset) return false;
        offset += entry_bytes;
    }
    size_t ignored_next = 0;
    return decode_entry(data_, length_, offset, out, ignored_next);
}

std::shared_ptr<const LargeByteBuffer> encode_report_source_progress(
    const ReportSourceProgressEntry *entries,
    size_t count) {
    if ((count > 0 && !entries) || count > UINT32_MAX) return {};

    size_t total = PROGRESS_HEADER_BYTES;
    for (size_t i = 0; i < count; ++i) {
        const ReportSourceProgressEntry &entry = entries[i];
        if (!valid_entry(entry)) return {};

        size_t entry_bytes = 0;
        if (!checked_entry_bytes(entry.path_length, entry_bytes) ||
            !CheckedSize::add_to(total, entry_bytes)) {
            return {};
        }
    }
    if (total > UINT32_MAX) return {};

    std::unique_ptr<LargeByteBuffer> buffer =
        LargeByteBuffer::allocate(total);
    if (!buffer) return {};

    uint8_t *out = buffer->data();
    LittleEndian::put_le32(out, PROGRESS_MAGIC);
    LittleEndian::put_le16(out + 4, PROGRESS_VERSION);
    LittleEndian::put_le16(out + 6, PROGRESS_HEADER_BYTES);
    LittleEndian::put_le32(out + 8, static_cast<uint32_t>(count));
    LittleEndian::put_le32(out + 12, static_cast<uint32_t>(total));

    size_t offset = PROGRESS_HEADER_BYTES;
    for (size_t i = 0; i < count; ++i) {
        encode_entry(out + offset, entries[i]);
        offset += PROGRESS_ENTRY_BYTES + entries[i].path_length;
    }
    return LargeByteBuffer::freeze(std::move(buffer));
}

}  // namespace aircannect
