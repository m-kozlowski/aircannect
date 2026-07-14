#include "report_night_index_store.h"

#include <Arduino.h>
#include <algorithm>
#include <string.h>

#include "crc32.h"
#include "little_endian.h"
#include "memory_manager.h"
#include "report_diagnostics.h"
#include "report_index_scratch.h"
#include "report_manager_limits.h"
#include "report_summary_record_codec.h"
#include "storage_manager.h"

namespace aircannect {
namespace ReportNightIndexStore {
namespace {

constexpr const char *INDEX_DIR = "/aircannect/report/v4/index/v3";
constexpr const char *INDEX_PATH =
    "/aircannect/report/v4/index/v3/night-index.bin";
constexpr const char *INDEX_TMP_PATH =
    "/aircannect/report/v4/index/v3/night-index.bin.tmp";
constexpr uint32_t INDEX_MAGIC = 0x494e4341u;  // "ACNI", little-endian.
constexpr uint16_t INDEX_SCHEMA = 3;
constexpr size_t INDEX_HEADER_SIZE = 32;
constexpr size_t INDEX_RECORD_SIZE = 4096;
constexpr size_t RECORD_SUMMARY_OFFSET = 32;
constexpr size_t RECORD_RANGES_OFFSET =
    RECORD_SUMMARY_OFFSET + AC_REPORT_SUMMARY_RECORD_CODEC_SIZE;
constexpr size_t RECORD_DATA_RANGES_OFFSET =
    RECORD_RANGES_OFFSET + AC_REPORT_NIGHT_SESSION_MAX * 16;
constexpr size_t RECORD_EDF_SIGNATURES_OFFSET =
    RECORD_DATA_RANGES_OFFSET + AC_REPORT_NIGHT_SESSION_MAX * 16;
static_assert(RECORD_EDF_SIGNATURES_OFFSET +
                      AC_REPORT_EDF_SESSION_MAX * sizeof(uint64_t) <=
                  INDEX_RECORD_SIZE,
              "durable report night index record is too small");

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

bool read_all(File &file, uint8_t *data, size_t len) {
    if (!len) return true;
    return data && file.read(data, len) == static_cast<int>(len);
}

bool write_all(File &file, const uint8_t *data, size_t len) {
    if (!len) return true;
    return data && file.write(data, len) == len;
}

bool ensure_layout() {
    return Storage::ensure_dir("/aircannect") &&
           Storage::ensure_dir("/aircannect/report") &&
           Storage::ensure_dir("/aircannect/report/v4") &&
           Storage::ensure_dir("/aircannect/report/v4/index") &&
           Storage::ensure_dir(INDEX_DIR);
}

void encode_header(uint8_t *header,
                   uint32_t record_count,
                   uint32_t payload_len,
                   uint32_t payload_crc) {
    memset(header, 0, INDEX_HEADER_SIZE);
    put_le32(header + 0, INDEX_MAGIC);
    put_le16(header + 4, INDEX_SCHEMA);
    put_le16(header + 6, INDEX_HEADER_SIZE);
    put_le16(header + 8, INDEX_RECORD_SIZE);
    put_le32(header + 12, record_count);
    put_le32(header + 16, payload_len);
    put_le32(header + 20, payload_crc);
}

bool decode_header(const uint8_t *header,
                   uint32_t &record_count,
                   uint32_t &payload_len,
                   uint32_t &payload_crc) {
    if (get_le32(header + 0) != INDEX_MAGIC) return false;
    if (get_le16(header + 4) != INDEX_SCHEMA) return false;
    if (get_le16(header + 6) != INDEX_HEADER_SIZE) return false;
    if (get_le16(header + 8) != INDEX_RECORD_SIZE) return false;
    record_count = get_le32(header + 12);
    payload_len = get_le32(header + 16);
    payload_crc = get_le32(header + 20);
    return record_count <= AC_REPORT_SUMMARY_RECORD_MAX &&
           payload_len == record_count * INDEX_RECORD_SIZE;
}

void encode_range(uint8_t *out, const ReportSessionRange &range) {
    put_le64(out + 0, static_cast<uint64_t>(range.start_ms));
    put_le64(out + 8, static_cast<uint64_t>(range.end_ms));
}

ReportSessionRange decode_range(const uint8_t *in) {
    ReportSessionRange range;
    range.start_ms = static_cast<int64_t>(get_le64(in + 0));
    range.end_ms = static_cast<int64_t>(get_le64(in + 8));
    return range;
}

bool encode_record(uint8_t *raw, const ReportIndexedNight &night) {
    memset(raw, 0, INDEX_RECORD_SIZE);
    uint32_t flags = 0;
    if (night.summary.valid) flags |= 1u << 0;
    if (night.has_summary) flags |= 1u << 1;
    if (night.has_edf) flags |= 1u << 2;
    if (night.edf_catalog_pending) flags |= 1u << 3;
    put_le32(raw + 0, flags);
    put_le64(raw + 8, night.source_signature);
    const uint16_t range_count = static_cast<uint16_t>(
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX)));
    const uint16_t data_range_count = static_cast<uint16_t>(
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_NIGHT_SESSION_MAX)));
    const uint16_t edf_signature_count = static_cast<uint16_t>(
        std::min(night.edf_source_signature_count,
                 static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX)));
    put_le16(raw + 16, range_count);
    put_le16(raw + 18, data_range_count);
    put_le16(raw + 20, edf_signature_count);
    if (!report_summary_record_encode(raw + RECORD_SUMMARY_OFFSET,
                                      AC_REPORT_SUMMARY_RECORD_CODEC_SIZE,
                                      night.summary)) {
        return false;
    }
    for (uint16_t i = 0; i < range_count; ++i) {
        encode_range(raw + RECORD_RANGES_OFFSET + i * 16, night.ranges[i]);
    }
    for (uint16_t i = 0; i < data_range_count; ++i) {
        encode_range(raw + RECORD_DATA_RANGES_OFFSET + i * 16,
                     night.data_ranges[i]);
    }
    for (uint16_t i = 0; i < edf_signature_count; ++i) {
        put_le64(raw + RECORD_EDF_SIGNATURES_OFFSET +
                     static_cast<size_t>(i) * sizeof(uint64_t),
                 night.edf_source_signatures[i]);
    }
    return true;
}

bool decode_record(const uint8_t *raw, ReportIndexedNight &night) {
    night = {};
    const uint32_t flags = get_le32(raw + 0);
    night.has_summary = (flags & (1u << 1)) != 0;
    night.has_edf = (flags & (1u << 2)) != 0;
    night.edf_catalog_pending = (flags & (1u << 3)) != 0;
    night.source_signature = get_le64(raw + 8);
    const uint16_t range_count = static_cast<uint16_t>(
        std::min<size_t>(get_le16(raw + 16),
                         AC_REPORT_NIGHT_SESSION_MAX));
    const uint16_t data_range_count = static_cast<uint16_t>(
        std::min<size_t>(get_le16(raw + 18),
                         AC_REPORT_NIGHT_SESSION_MAX));
    const uint16_t edf_signature_count = static_cast<uint16_t>(
        std::min<size_t>(get_le16(raw + 20),
                         AC_REPORT_EDF_SESSION_MAX));
    if (!report_summary_record_decode(raw + RECORD_SUMMARY_OFFSET,
                                      AC_REPORT_SUMMARY_RECORD_CODEC_SIZE,
                                      night.summary)) {
        return false;
    }
    night.range_count = range_count;
    for (uint16_t i = 0; i < range_count; ++i) {
        night.ranges[i] =
            decode_range(raw + RECORD_RANGES_OFFSET + i * 16);
    }
    night.data_range_count = data_range_count;
    for (uint16_t i = 0; i < data_range_count; ++i) {
        night.data_ranges[i] =
            decode_range(raw + RECORD_DATA_RANGES_OFFSET + i * 16);
    }
    night.edf_source_signature_count = edf_signature_count;
    for (uint16_t i = 0; i < edf_signature_count; ++i) {
        night.edf_source_signatures[i] =
            get_le64(raw + RECORD_EDF_SIGNATURES_OFFSET +
                     static_cast<size_t>(i) * sizeof(uint64_t));
    }

    normalize_report_indexed_night(night);
    return night.summary.valid && night.summary.start_ms > 0;
}

uint8_t *allocate_record_scratch(const char *context) {
    uint8_t *raw = static_cast<uint8_t *>(
        Memory::alloc_large(INDEX_RECORD_SIZE, false));
    if (!raw) log_report_alloc_failed(context, INDEX_RECORD_SIZE);
    return raw;
}

struct DurableRecordReaderContext {
    File *file = nullptr;
    uint8_t *raw = nullptr;
    size_t next_index = 0;
};

bool read_durable_record(void *opaque,
                         size_t index,
                         ReportIndexedNight &out) {
    auto *context = static_cast<DurableRecordReaderContext *>(opaque);
    if (!context || !context->file || !context->raw) return false;

    if (index != context->next_index) {
        const size_t offset = INDEX_HEADER_SIZE + index * INDEX_RECORD_SIZE;
        if (!context->file->seek(offset)) return false;
    }

    if (!read_all(*context->file, context->raw, INDEX_RECORD_SIZE)) {
        return false;
    }

    context->next_index = index + 1;
    return decode_record(context->raw, out);
}

}  // namespace

bool load(ReportNightIndexSnapshotRef &out, uint32_t &content_crc) {
    out.reset();
    content_crc = crc32_ieee(nullptr, 0);

    Storage::Guard guard;
    if (!Storage::mounted() || !Storage::exists(INDEX_PATH)) return false;

    File file = Storage::open(INDEX_PATH, "r");
    if (!file) return false;

    uint8_t header[INDEX_HEADER_SIZE];
    uint32_t record_count = 0;
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
    bool ok = read_all(file, header, sizeof(header)) &&
              decode_header(header, record_count, payload_len, payload_crc);

    uint8_t *raw = ok && record_count > 0
        ? allocate_record_scratch("durable_night_index_load_record")
        : nullptr;
    if (ok && record_count > 0 && !raw) ok = false;

    ScopedIndexedNight verified("durable_night_index_load_night");
    if (ok && record_count > 0 && !verified) ok = false;

    uint32_t crc_state = crc32_ieee_initial_state();
    for (uint32_t i = 0; ok && i < record_count; ++i) {
        ok = read_all(file, raw, INDEX_RECORD_SIZE);
        if (!ok) break;

        crc_state = crc32_ieee_update_state(crc_state,
                                            raw,
                                            INDEX_RECORD_SIZE);
        ok = decode_record(raw, verified.get());
    }

    const uint32_t actual_crc = crc32_ieee_finish_state(crc_state);
    if (ok && actual_crc != payload_crc) ok = false;

    if (ok) {
        if (record_count == 0) {
            out = ReportNightIndexSnapshot::create(nullptr, 0);
        } else {
            DurableRecordReaderContext context;
            context.file = &file;
            context.raw = raw;
            context.next_index = record_count;

            out = ReportNightIndexSnapshot::create_from_reader(
                record_count,
                read_durable_record,
                &context);
        }
        ok = out != nullptr;
    }

    file.close();
    Memory::free(raw);
    if (!ok) return false;

    content_crc = actual_crc;
    return out != nullptr;
}

bool save(const ReportNightIndexSnapshot &snapshot,
          uint32_t &content_crc) {
    content_crc = crc32_ieee(nullptr, 0);
    const size_t count = snapshot.count();
    if (count > AC_REPORT_SUMMARY_RECORD_MAX) {
        return false;
    }

    ScopedIndexedNight night("durable_night_index_save_night");
    uint8_t *raw = allocate_record_scratch("durable_night_index_save_record");
    if (!night || !raw) {
        Memory::free(raw);
        return false;
    }

    Storage::Guard guard;
    if (!Storage::mounted() || !ensure_layout()) {
        Memory::free(raw);
        return false;
    }
    Storage::remove(INDEX_TMP_PATH);
    File file = Storage::open(INDEX_TMP_PATH, "w");
    if (!file) {
        Memory::free(raw);
        return false;
    }

    uint8_t header[INDEX_HEADER_SIZE] = {};
    bool ok = write_all(file, header, sizeof(header));
    uint32_t crc_state = crc32_ieee_initial_state();
    for (size_t i = 0; ok && i < count; ++i) {
        ok = snapshot.materialize(i, night.get()) &&
             encode_record(raw, night.get());
        if (!ok) break;

        crc_state = crc32_ieee_update_state(crc_state,
                                            raw,
                                            INDEX_RECORD_SIZE);
        ok = write_all(file, raw, INDEX_RECORD_SIZE);
    }

    content_crc = crc32_ieee_finish_state(crc_state);
    if (ok) {
        encode_header(header,
                      static_cast<uint32_t>(count),
                      static_cast<uint32_t>(count * INDEX_RECORD_SIZE),
                      content_crc);
        ok = file.seek(0) && write_all(file, header, sizeof(header));
    }
    file.close();
    Memory::free(raw);

    if (!ok) {
        Storage::remove(INDEX_TMP_PATH);
        return false;
    }
    Storage::remove(INDEX_PATH);
    if (!Storage::rename(INDEX_TMP_PATH, INDEX_PATH)) {
        Storage::remove(INDEX_TMP_PATH);
        return false;
    }
    return true;
}

}  // namespace ReportNightIndexStore
}  // namespace aircannect
