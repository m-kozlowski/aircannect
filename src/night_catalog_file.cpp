#include "night_catalog_file.h"

#include <cmath>
#include <limits>
#include <new>
#include <string.h>

#include "crc32.h"
#include "little_endian.h"
#include "storage_path.h"

namespace aircannect {
namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

constexpr uint8_t FILE_MAGIC[8] = {
    'A', 'C', 'N', 'C', 'A', 'T', '0', '6',
};

constexpr size_t RECORD_BYTES = 112;
constexpr size_t RANGE_BYTES = 16;
constexpr size_t FILE_BYTES = 80;
constexpr size_t COVERAGE_BYTES = 24;

constexpr uint8_t SOURCE_FLAGS = NIGHT_CATALOG_SOURCE_EDF |
                                 NIGHT_CATALOG_SOURCE_STR |
                                 NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK;
constexpr uint16_t METRIC_FLAGS =
    (1u << static_cast<uint8_t>(NightCatalogMetric::Count)) - 1u;

struct CatalogLayout {
    uint32_t records = 0;
    uint32_t sessions = 0;
    uint32_t masks = 0;
    uint32_t files = 0;
    uint32_t coverage = 0;
    uint32_t path_bytes = 0;
    size_t body_bytes = 0;
};

bool add_size(size_t &total, size_t count, size_t item_size) {
    if (count > std::numeric_limits<size_t>::max() / item_size) return false;

    const size_t bytes = count * item_size;
    if (total > std::numeric_limits<size_t>::max() - bytes) return false;
    total += bytes;
    return true;
}

bool add_u32(uint32_t &total, size_t count) {
    if (count > UINT32_MAX || total > UINT32_MAX - count) return false;
    total += static_cast<uint32_t>(count);
    return true;
}

bool ranges_valid(const NightCatalogTimeRange *ranges, size_t count) {
    if (count > 0 && !ranges) return false;

    for (size_t i = 0; i < count; ++i) {
        if (!ranges[i].valid()) return false;
        if (i > 0 && ranges[i].start_ms < ranges[i - 1].start_ms) {
            return false;
        }
    }
    return true;
}

bool metrics_valid(const NightCatalogMetrics &metrics) {
    if ((metrics.valid_mask & ~METRIC_FLAGS) != 0 ||
        (metrics.str_mask & ~metrics.valid_mask) != 0 ||
        (metrics.summary_mask & ~metrics.valid_mask) != 0 ||
        (metrics.str_mask & metrics.summary_mask) != 0 ||
        (metrics.str_mask | metrics.summary_mask) != metrics.valid_mask) {
        return false;
    }

    return std::isfinite(metrics.ahi) &&
           std::isfinite(metrics.obstructive_apnea_index) &&
           std::isfinite(metrics.central_apnea_index) &&
           std::isfinite(metrics.unknown_apnea_index) &&
           std::isfinite(metrics.hypopnea_index) &&
           std::isfinite(metrics.arousal_index) &&
           std::isfinite(metrics.mask_pressure_50_cm_h2o) &&
           std::isfinite(metrics.leak_50_l_min);
}

bool file_span_valid(const NightCatalogSourceFile &file) {
    return file.identity != 0 && file.record_size != 0 &&
           file.data_offset <= file.file_size &&
           file.data_size <= file.file_size - file.data_offset;
}

bool inspect_catalog(const NightCatalog &catalog, CatalogLayout &layout) {
    if (catalog.size() > UINT32_MAX) return false;
    layout.records = static_cast<uint32_t>(catalog.size());

    uint32_t expected_session = 0;
    uint32_t expected_mask = 0;
    uint32_t expected_file = 0;
    uint32_t expected_coverage = 0;
    uint32_t expected_path = 0;
    SleepDayId previous_day;

    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *record = catalog.record(i);
        if (!record || !record->sleep_day.valid() ||
            !record->source_revision.valid() ||
            record->day_end_ms <= record->day_start_ms ||
            record->source_flags == 0 ||
            (record->source_flags & ~SOURCE_FLAGS) != 0 ||
            (((record->source_flags &
               NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) != 0) !=
             (record->summary_identity != 0)) ||
            !metrics_valid(record->metrics)) {
            return false;
        }
        if (i > 0 && !(record->sleep_day < previous_day)) return false;
        previous_day = record->sleep_day;

        size_t session_count = 0;
        const NightCatalogTimeRange *sessions =
            catalog.sessions(*record, session_count);
        if (record->session_offset != expected_session ||
            session_count != record->session_count ||
            !ranges_valid(sessions, session_count) ||
            !add_u32(expected_session, session_count)) {
            return false;
        }

        size_t mask_count = 0;
        const NightCatalogTimeRange *masks =
            catalog.mask_windows(*record, mask_count);
        if (record->mask_window_offset != expected_mask ||
            mask_count != record->mask_window_count ||
            !ranges_valid(masks, mask_count) ||
            !add_u32(expected_mask, mask_count)) {
            return false;
        }

        size_t file_count = 0;
        const NightCatalogSourceFile *files =
            catalog.files(*record, file_count);
        if (record->file_offset != expected_file ||
            file_count != record->file_count ||
            (file_count > 0 && !files)) {
            return false;
        }

        bool has_edf = false;
        bool has_str = false;
        for (size_t file_index = 0; file_index < file_count; ++file_index) {
            const NightCatalogSourceFile &file = files[file_index];
            if (static_cast<uint8_t>(file.kind) >
                    static_cast<uint8_t>(NightCatalogFileKind::Str) ||
                file.path_offset != expected_path || file.path_length == 0 ||
                (file.session_index != NIGHT_CATALOG_NO_SESSION &&
                 file.session_index >= session_count) ||
                ((file.kind == NightCatalogFileKind::Str) !=
                 (file.session_index == NIGHT_CATALOG_NO_SESSION)) ||
                file.coverage_offset != expected_coverage ||
                file.coverage_count == 0 || !file_span_valid(file)) {
                return false;
            }

            has_str = has_str || file.kind == NightCatalogFileKind::Str;
            has_edf = has_edf || file.kind != NightCatalogFileKind::Str;

            const char *path = catalog.path(file);
            if (!path || path[file.path_length] != '\0' ||
                memchr(path, '\0', file.path_length) != nullptr ||
                !storage_user_path_valid(path) ||
                !add_u32(expected_path,
                         static_cast<size_t>(file.path_length) + 1)) {
                return false;
            }

            size_t coverage_count = 0;
            const NightCatalogSourceCoverage *coverage =
                catalog.coverage(file, coverage_count);
            if (coverage_count != file.coverage_count || !coverage) {
                return false;
            }
            for (size_t coverage_index = 0;
                 coverage_index < coverage_count;
                 ++coverage_index) {
                if (coverage[coverage_index].range.start_ms <= 0 ||
                    coverage[coverage_index].range.end_ms <
                        coverage[coverage_index].range.start_ms) {
                    return false;
                }
            }
            if (!add_u32(expected_coverage, coverage_count)) return false;
        }
        if (has_edf != ((record->source_flags &
                         NIGHT_CATALOG_SOURCE_EDF) != 0) ||
            has_str != ((record->source_flags &
                         NIGHT_CATALOG_SOURCE_STR) != 0)) {
            return false;
        }
        if (!add_u32(expected_file, file_count)) return false;
    }

    layout.sessions = expected_session;
    layout.masks = expected_mask;
    layout.files = expected_file;
    layout.coverage = expected_coverage;
    layout.path_bytes = expected_path;

    size_t body_bytes = 0;
    if (!add_size(body_bytes, layout.records, RECORD_BYTES) ||
        !add_size(body_bytes, layout.sessions, RANGE_BYTES) ||
        !add_size(body_bytes, layout.masks, RANGE_BYTES) ||
        !add_size(body_bytes, layout.files, FILE_BYTES) ||
        !add_size(body_bytes, layout.coverage, COVERAGE_BYTES) ||
        !add_size(body_bytes, layout.path_bytes, 1) ||
        body_bytes >
            NightCatalogFileCodec::MaximumFileBytes -
                NightCatalogFileCodec::HeaderBytes) {
        return false;
    }

    layout.body_bytes = body_bytes;
    return true;
}

uint32_t float_bits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(uint32_t bits) {
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void encode_range(uint8_t *out, const NightCatalogTimeRange &range) {
    put_le64(out, static_cast<uint64_t>(range.start_ms));
    put_le64(out + 8, static_cast<uint64_t>(range.end_ms));
}

NightCatalogTimeRange decode_range(const uint8_t *in) {
    return {
        static_cast<int64_t>(get_le64(in)),
        static_cast<int64_t>(get_le64(in + 8)),
    };
}

void encode_metrics(uint8_t *out, const NightCatalogMetrics &metrics) {
    put_le16(out, metrics.valid_mask);
    put_le16(out + 2, metrics.str_mask);
    put_le16(out + 4, metrics.summary_mask);
    put_le16(out + 6, 0);
    put_le32(out + 8, float_bits(metrics.ahi));
    put_le32(out + 12, float_bits(metrics.obstructive_apnea_index));
    put_le32(out + 16, float_bits(metrics.central_apnea_index));
    put_le32(out + 20, float_bits(metrics.unknown_apnea_index));
    put_le32(out + 24, float_bits(metrics.hypopnea_index));
    put_le32(out + 28, float_bits(metrics.arousal_index));
    put_le32(out + 32, float_bits(metrics.mask_pressure_50_cm_h2o));
    put_le32(out + 36, float_bits(metrics.leak_50_l_min));
    put_le32(out + 40, metrics.duration_min);
}

void decode_metrics(const uint8_t *in, NightCatalogMetrics &metrics) {
    metrics.valid_mask = get_le16(in);
    metrics.str_mask = get_le16(in + 2);
    metrics.summary_mask = get_le16(in + 4);
    metrics.ahi = bits_float(get_le32(in + 8));
    metrics.obstructive_apnea_index = bits_float(get_le32(in + 12));
    metrics.central_apnea_index = bits_float(get_le32(in + 16));
    metrics.unknown_apnea_index = bits_float(get_le32(in + 20));
    metrics.hypopnea_index = bits_float(get_le32(in + 24));
    metrics.arousal_index = bits_float(get_le32(in + 28));
    metrics.mask_pressure_50_cm_h2o = bits_float(get_le32(in + 32));
    metrics.leak_50_l_min = bits_float(get_le32(in + 36));
    metrics.duration_min = get_le32(in + 40);
}

void encode_record(uint8_t *out, const NightCatalogRecord &record) {
    put_le32(out, static_cast<uint32_t>(record.sleep_day.epoch_days()));
    out[4] = record.source_flags;
    put_le64(out + 8, record.source_revision.value());
    put_le64(out + 16, static_cast<uint64_t>(record.day_start_ms));
    put_le64(out + 24, static_cast<uint64_t>(record.day_end_ms));
    put_le32(out + 32, record.session_offset);
    put_le32(out + 36, record.session_count);
    put_le32(out + 40, record.mask_window_offset);
    put_le32(out + 44, record.mask_window_count);
    put_le32(out + 48, record.file_offset);
    put_le32(out + 52, record.file_count);
    put_le64(out + 56, record.summary_identity);
    encode_metrics(out + 64, record.metrics);
}

bool decode_record(const uint8_t *in, NightCatalogRecord &record) {
    SleepDayId day;
    if (!SleepDayId::from_epoch_days(
            static_cast<int32_t>(get_le32(in)), day)) {
        return false;
    }

    const uint32_t session_count = get_le32(in + 36);
    const uint32_t mask_count = get_le32(in + 44);
    const uint32_t file_count = get_le32(in + 52);
    if (session_count > UINT16_MAX || mask_count > UINT16_MAX ||
        file_count > UINT16_MAX) {
        return false;
    }

    record.sleep_day = day;
    record.source_flags = in[4];
    record.source_revision = SourceRevision(get_le64(in + 8));
    record.day_start_ms = static_cast<int64_t>(get_le64(in + 16));
    record.day_end_ms = static_cast<int64_t>(get_le64(in + 24));
    record.session_offset = get_le32(in + 32);
    record.session_count = static_cast<uint16_t>(session_count);
    record.mask_window_offset = get_le32(in + 40);
    record.mask_window_count = static_cast<uint16_t>(mask_count);
    record.file_offset = get_le32(in + 48);
    record.file_count = static_cast<uint16_t>(file_count);
    record.summary_identity = get_le64(in + 56);
    decode_metrics(in + 64, record.metrics);
    return true;
}

void encode_file(uint8_t *out, const NightCatalogSourceFile &file) {
    out[0] = static_cast<uint8_t>(file.kind);
    put_le32(out + 4, file.path_offset);
    put_le32(out + 8, file.path_length);
    put_le32(out + 12,
             file.session_index == NIGHT_CATALOG_NO_SESSION
                 ? UINT32_MAX
                 : file.session_index);
    put_le32(out + 16, file.coverage_offset);
    put_le32(out + 20, file.coverage_count);
    put_le64(out + 24, file.file_size);
    put_le64(out + 32, static_cast<uint64_t>(file.last_write_ms));
    put_le64(out + 40, file.data_offset);
    put_le64(out + 48, file.data_size);
    put_le64(out + 56, file.identity);
    put_le32(out + 64, file.header_size);
    put_le32(out + 68, file.record_size);
    put_le32(out + 72, file.record_duration_ms);
    put_le32(out + 76, file.complete_records);
}

bool decode_file(const uint8_t *in, NightCatalogSourceFile &file) {
    const uint32_t kind = in[0];
    const uint32_t path_length = get_le32(in + 8);
    const uint32_t session_index = get_le32(in + 12);
    const uint32_t coverage_count = get_le32(in + 20);
    if (kind > static_cast<uint8_t>(NightCatalogFileKind::Str) ||
        path_length == 0 || path_length > UINT16_MAX ||
        (session_index != UINT32_MAX && session_index > UINT16_MAX) ||
        coverage_count == 0 || coverage_count > UINT16_MAX) {
        return false;
    }

    file.kind = static_cast<NightCatalogFileKind>(kind);
    file.path_offset = get_le32(in + 4);
    file.path_length = static_cast<uint16_t>(path_length);
    file.session_index = session_index == UINT32_MAX
        ? NIGHT_CATALOG_NO_SESSION
        : static_cast<uint16_t>(session_index);
    file.coverage_offset = get_le32(in + 16);
    file.coverage_count = static_cast<uint16_t>(coverage_count);
    file.file_size = get_le64(in + 24);
    file.last_write_ms = static_cast<int64_t>(get_le64(in + 32));
    file.data_offset = get_le64(in + 40);
    file.data_size = get_le64(in + 48);
    file.identity = get_le64(in + 56);
    file.header_size = get_le32(in + 64);
    file.record_size = get_le32(in + 68);
    file.record_duration_ms = get_le32(in + 72);
    file.complete_records = get_le32(in + 76);
    return true;
}

void encode_coverage(uint8_t *out,
                     const NightCatalogSourceCoverage &coverage) {
    encode_range(out, coverage.range);
    put_le32(out + 16, coverage.primary_signal_mask);
    put_le32(out + 20, coverage.fallback_signal_mask);
}

void decode_coverage(const uint8_t *in,
                     NightCatalogSourceCoverage &coverage) {
    coverage.range = decode_range(in);
    coverage.primary_signal_mask = get_le32(in + 16);
    coverage.fallback_signal_mask = get_le32(in + 20);
}

bool parse_header(const uint8_t *header,
                  size_t header_length,
                  NightCatalogFileInfo &info,
                  uint32_t &body_crc) {
    info = {};
    body_crc = 0;
    if (!header || header_length != NightCatalogFileCodec::HeaderBytes ||
        memcmp(header, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0 ||
        get_le16(header + 8) != NightCatalogFileCodec::Version ||
        get_le16(header + 10) != NightCatalogFileCodec::HeaderBytes ||
        get_le16(header + 12) != RECORD_BYTES ||
        get_le16(header + 14) != RANGE_BYTES ||
        get_le16(header + 16) != FILE_BYTES ||
        get_le16(header + 18) != COVERAGE_BYTES ||
        get_le32(header + 20) != 0 ||
        crc32_ieee(header, 60) != get_le32(header + 60)) {
        return false;
    }

    info.record_count = get_le32(header + 24);
    info.session_count = get_le32(header + 28);
    info.mask_window_count = get_le32(header + 32);
    info.file_count = get_le32(header + 36);
    info.coverage_count = get_le32(header + 40);
    info.path_bytes = get_le32(header + 44);

    size_t body_bytes = 0;
    if (!add_size(body_bytes, info.record_count, RECORD_BYTES) ||
        !add_size(body_bytes, info.session_count, RANGE_BYTES) ||
        !add_size(body_bytes, info.mask_window_count, RANGE_BYTES) ||
        !add_size(body_bytes, info.file_count, FILE_BYTES) ||
        !add_size(body_bytes, info.coverage_count, COVERAGE_BYTES) ||
        !add_size(body_bytes, info.path_bytes, 1) ||
        get_le64(header + 48) != body_bytes ||
        body_bytes > NightCatalogFileCodec::MaximumFileBytes -
                         NightCatalogFileCodec::HeaderBytes) {
        return false;
    }

    info.body_bytes = body_bytes;
    info.total_bytes = NightCatalogFileCodec::HeaderBytes + body_bytes;
    body_crc = get_le32(header + 56);
    return true;
}

}  // namespace

bool NightCatalogFileCodec::inspect(const uint8_t *header,
                                    size_t header_length,
                                    NightCatalogFileInfo &info) {
    uint32_t ignored_crc = 0;
    return parse_header(header, header_length, info, ignored_crc);
}

std::shared_ptr<const LargeByteBuffer> NightCatalogFileCodec::encode(
    const NightCatalog &catalog) {
    CatalogLayout layout;
    if (!inspect_catalog(catalog, layout)) return {};

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(HeaderBytes + layout.body_bytes);
    if (!output) return {};
    memset(output->data(), 0, output->size());

    uint8_t *header = output->data();
    uint8_t *records = header + HeaderBytes;
    uint8_t *sessions = records + layout.records * RECORD_BYTES;
    uint8_t *masks = sessions + layout.sessions * RANGE_BYTES;
    uint8_t *files = masks + layout.masks * RANGE_BYTES;
    uint8_t *coverage = files + layout.files * FILE_BYTES;
    uint8_t *paths = coverage + layout.coverage * COVERAGE_BYTES;

    size_t next_session = 0;
    size_t next_mask = 0;
    size_t next_file = 0;
    size_t next_coverage = 0;
    size_t next_path = 0;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord &record = *catalog.record(i);
        encode_record(records + i * RECORD_BYTES, record);

        size_t session_count = 0;
        const NightCatalogTimeRange *record_sessions =
            catalog.sessions(record, session_count);
        for (size_t j = 0; j < session_count; ++j) {
            encode_range(sessions + next_session++ * RANGE_BYTES,
                         record_sessions[j]);
        }

        size_t mask_count = 0;
        const NightCatalogTimeRange *record_masks =
            catalog.mask_windows(record, mask_count);
        for (size_t j = 0; j < mask_count; ++j) {
            encode_range(masks + next_mask++ * RANGE_BYTES,
                         record_masks[j]);
        }

        size_t file_count = 0;
        const NightCatalogSourceFile *record_files =
            catalog.files(record, file_count);
        for (size_t j = 0; j < file_count; ++j) {
            const NightCatalogSourceFile &file = record_files[j];
            encode_file(files + next_file++ * FILE_BYTES, file);

            size_t coverage_count = 0;
            const NightCatalogSourceCoverage *file_coverage =
                catalog.coverage(file, coverage_count);
            for (size_t k = 0; k < coverage_count; ++k) {
                encode_coverage(
                    coverage + next_coverage++ * COVERAGE_BYTES,
                    file_coverage[k]);
            }

            const char *path = catalog.path(file);
            memcpy(paths + next_path, path, file.path_length + 1);
            next_path += file.path_length + 1;
        }
    }

    if (next_session != layout.sessions || next_mask != layout.masks ||
        next_file != layout.files || next_coverage != layout.coverage ||
        next_path != layout.path_bytes) {
        return {};
    }

    memcpy(header, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(header + 8, Version);
    put_le16(header + 10, HeaderBytes);
    put_le16(header + 12, RECORD_BYTES);
    put_le16(header + 14, RANGE_BYTES);
    put_le16(header + 16, FILE_BYTES);
    put_le16(header + 18, COVERAGE_BYTES);
    put_le32(header + 24, layout.records);
    put_le32(header + 28, layout.sessions);
    put_le32(header + 32, layout.masks);
    put_le32(header + 36, layout.files);
    put_le32(header + 40, layout.coverage);
    put_le32(header + 44, layout.path_bytes);
    put_le64(header + 48, layout.body_bytes);
    put_le32(header + 56,
             crc32_ieee(header + HeaderBytes, layout.body_bytes));
    put_le32(header + 60, crc32_ieee(header, 60));
    return LargeByteBuffer::freeze(std::move(output));
}

std::shared_ptr<const NightCatalog> NightCatalogFileCodec::decode(
    const uint8_t *header,
    size_t header_length,
    const uint8_t *body,
    size_t body_length) {
    NightCatalogFileInfo info;
    uint32_t expected_body_crc = 0;
    if (!parse_header(header,
                      header_length,
                      info,
                      expected_body_crc) ||
        body_length != info.body_bytes ||
        (body_length > 0 && !body) ||
        crc32_ieee(body, body_length) != expected_body_crc) {
        return {};
    }

    std::shared_ptr<NightCatalog> catalog(new (std::nothrow) NightCatalog());
    if (!catalog || !catalog->allocate(info.record_count,
                                       info.session_count,
                                       info.mask_window_count,
                                       info.file_count,
                                       info.coverage_count,
                                       info.path_bytes)) {
        return {};
    }

    const uint8_t *records = body ? body : header + header_length;
    const uint8_t *sessions = records + info.record_count * RECORD_BYTES;
    const uint8_t *masks = sessions + info.session_count * RANGE_BYTES;
    const uint8_t *files = masks + info.mask_window_count * RANGE_BYTES;
    const uint8_t *coverage = files + info.file_count * FILE_BYTES;
    const uint8_t *paths = coverage + info.coverage_count * COVERAGE_BYTES;

    for (size_t i = 0; i < info.record_count; ++i) {
        if (!decode_record(records + i * RECORD_BYTES,
                           catalog->records_[i])) {
            return {};
        }
    }
    for (size_t i = 0; i < info.session_count; ++i) {
        catalog->sessions_[i] = decode_range(sessions + i * RANGE_BYTES);
    }
    for (size_t i = 0; i < info.mask_window_count; ++i) {
        catalog->mask_windows_[i] = decode_range(masks + i * RANGE_BYTES);
    }
    for (size_t i = 0; i < info.file_count; ++i) {
        if (!decode_file(files + i * FILE_BYTES, catalog->files_[i])) {
            return {};
        }
    }
    for (size_t i = 0; i < info.coverage_count; ++i) {
        decode_coverage(coverage + i * COVERAGE_BYTES,
                        catalog->coverage_[i]);
    }
    if (info.path_bytes > 0) {
        memcpy(catalog->paths_, paths, info.path_bytes);
    }

    CatalogLayout decoded_layout;
    if (!inspect_catalog(*catalog, decoded_layout) ||
        decoded_layout.records != info.record_count ||
        decoded_layout.sessions != info.session_count ||
        decoded_layout.masks != info.mask_window_count ||
        decoded_layout.files != info.file_count ||
        decoded_layout.coverage != info.coverage_count ||
        decoded_layout.path_bytes != info.path_bytes) {
        return {};
    }
    return catalog;
}

}  // namespace aircannect
