#include "report_artifacts.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "checked_size.h"
#include "crc32.h"
#include "large_scratch_array.h"
#include "little_endian.h"
#include "report_range_tile.h"

namespace aircannect {

bool report_range_tile_artifact_valid(const ReportRangeTileArtifact &tile) {
    return tile.start_ms > 0 &&
           tile.start_ms % REPORT_RANGE_TILE_MS == 0 &&
           tile.start_ms <= INT64_MAX - REPORT_RANGE_TILE_MS &&
           tile.end_ms == tile.start_ms + REPORT_RANGE_TILE_MS &&
           tile.size > 0 && tile.size <= UINT32_MAX;
}

namespace {

constexpr uint32_t RESULT_MAGIC = 0x36524341u;    // "ACR6"
constexpr uint32_t MANIFEST_MAGIC = 0x364d4341u;  // "ACM6"

constexpr size_t RESULT_V1_BODY_CRC_OFFSET = 148;
constexpr size_t RESULT_V1_HEADER_CRC_OFFSET = 152;
constexpr size_t RESULT_BODY_CRC_OFFSET = 204;
constexpr size_t RESULT_HEADER_CRC_OFFSET = 208;
constexpr size_t MANIFEST_BODY_CRC_OFFSET = 60;
constexpr size_t MANIFEST_HEADER_CRC_OFFSET = 64;

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

void put_i32(uint8_t *out, int32_t value) {
    put_le32(out, static_cast<uint32_t>(value));
}

void put_i64(uint8_t *out, int64_t value) {
    put_le64(out, static_cast<uint64_t>(value));
}

int32_t get_i32(const uint8_t *data) {
    return static_cast<int32_t>(get_le32(data));
}

int64_t get_i64(const uint8_t *data) {
    return static_cast<int64_t>(get_le64(data));
}

bool key_is_result(const ReportArtifactKey &key) {
    return key.valid() && key.kind == ReportArtifactKind::Result;
}

bool valid_result_session_ranges(const ReportResultArtifactData &data) {
    if (data.session_count == 0) {
        return !data.sessions && data.therapy_start_ms == 0 &&
               data.therapy_end_ms == 0;
    }
    if (!data.sessions || data.therapy_end_ms <= data.therapy_start_ms) {
        return false;
    }

    int64_t previous_end_ms = 0;
    for (size_t i = 0; i < data.session_count; ++i) {
        const NightCatalogTimeRange &session = data.sessions[i];
        if (!session.valid() || session.start_ms < data.day_start_ms ||
            session.end_ms > data.day_end_ms ||
            (i > 0 && session.start_ms < previous_end_ms)) {
            return false;
        }
        previous_end_ms = session.end_ms;
    }

    return data.therapy_start_ms == data.sessions[0].start_ms &&
           data.therapy_end_ms == data.sessions[data.session_count - 1].end_ms;
}

bool tile_follows(const ReportRangeTileArtifact &previous,
                  const ReportRangeTileArtifact &tile) {
    return tile.start_ms > previous.start_ms ||
           (tile.start_ms == previous.start_ms &&
            tile.end_ms > previous.end_ms);
}

struct ManifestHeaderFields {
    ReportArtifactKey key;
    uint64_t result_size = 0;
    uint64_t overview_size = 0;
    uint32_t result_crc32 = 0;
    uint32_t overview_crc32 = 0;
    uint32_t overview_prefix_crc32 = 0;
};

template <typename TileReader>
std::shared_ptr<const LargeByteBuffer> encode_manifest(
    const ManifestHeaderFields &fields,
    size_t tile_count,
    TileReader read_tile) {
    if (!key_is_result(fields.key) || fields.result_size == 0 ||
        fields.overview_size == 0 ||
        tile_count > ReportArtifactManifestCodec::MaxTiles) {
        return {};
    }

    size_t body_bytes = 0;
    size_t total_bytes = 0;
    if (!CheckedSize::multiply(
            tile_count, ReportArtifactManifestCodec::TileBytes, body_bytes) ||
        !CheckedSize::add(
            ReportArtifactManifestCodec::HeaderBytes,
            body_bytes,
            total_bytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    put_le32(bytes, MANIFEST_MAGIC);
    put_le16(bytes + 4, ReportArtifactManifestCodec::Version);
    put_le16(bytes + 6, ReportArtifactManifestCodec::HeaderBytes);
    put_le32(bytes + 8, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 12, fields.key.sleep_day.epoch_days());
    put_le64(bytes + 16, fields.key.source_revision.value());
    put_le64(bytes + 24, fields.result_size);
    put_le64(bytes + 32, fields.overview_size);
    put_le32(bytes + 40, fields.result_crc32);
    put_le32(bytes + 44, fields.overview_crc32);
    put_le32(bytes + 48, fields.overview_prefix_crc32);
    put_le16(bytes + 52, static_cast<uint16_t>(tile_count));

    uint8_t *body = bytes + ReportArtifactManifestCodec::HeaderBytes;
    ReportRangeTileArtifact previous;
    for (size_t i = 0; i < tile_count; ++i) {
        ReportRangeTileArtifact tile;
        if (!read_tile(i, tile) || !report_range_tile_artifact_valid(tile) ||
            (i > 0 && !tile_follows(previous, tile))) {
            return {};
        }

        uint8_t *record =
            body + i * ReportArtifactManifestCodec::TileBytes;
        put_i64(record, tile.start_ms);
        put_i64(record + 8, tile.end_ms);
        put_le32(record + 16, static_cast<uint32_t>(tile.size));
        put_le32(record + 20, tile.crc32);
        put_le32(record + 24, tile.prefix_crc32);
        previous = tile;
    }

    put_le32(bytes + MANIFEST_BODY_CRC_OFFSET,
             crc32_ieee(body, body_bytes));
    put_le32(bytes + MANIFEST_HEADER_CRC_OFFSET,
             crc32_ieee(bytes, MANIFEST_HEADER_CRC_OFFSET));
    return LargeByteBuffer::freeze(std::move(output));
}

void encode_metrics(uint8_t *out, const ReportArtifactMetrics &metrics) {
    put_le32(out, metrics.valid_mask);
    put_le32(out + 4, metrics.str_mask);
    put_le32(out + 8, metrics.summary_mask);
    put_i32(out + 12, metrics.leak_mean_milli);
    put_i32(out + 16, metrics.ahi_milli);
    put_i32(out + 20, metrics.obstructive_apnea_index_milli);
    put_i32(out + 24, metrics.central_apnea_index_milli);
    put_i32(out + 28, metrics.unknown_apnea_index_milli);
    put_i32(out + 32, metrics.hypopnea_index_milli);
    put_i32(out + 36, metrics.arousal_index_milli);
    put_i32(out + 40, metrics.mask_pressure_50_milli);
    put_i32(out + 44, metrics.leak_50_milli);
    put_le32(out + 48, metrics.duration_minutes);
    put_i32(out + 52, metrics.mask_pressure_95_milli);
    put_i32(out + 56, metrics.leak_95_milli);
    put_i32(out + 60, metrics.minute_ventilation_50_milli);
    put_i32(out + 64, metrics.minute_ventilation_95_milli);
    put_i32(out + 68, metrics.respiratory_rate_50_milli);
    put_i32(out + 72, metrics.respiratory_rate_95_milli);
    put_i32(out + 76, metrics.tidal_volume_50_milli);
    put_i32(out + 80, metrics.tidal_volume_95_milli);
    put_i32(out + 84, metrics.spo2_median_milli);
    put_le32(out + 88, metrics.spo2_threshold_minutes);
    put_le32(out + 92, metrics.csr_minutes);
}

void decode_metrics(const uint8_t *data, ReportArtifactMetrics &metrics) {
    metrics.valid_mask = get_le32(data);
    metrics.str_mask = get_le32(data + 4);
    metrics.summary_mask = get_le32(data + 8);
    metrics.leak_mean_milli = get_i32(data + 12);
    metrics.ahi_milli = get_i32(data + 16);
    metrics.obstructive_apnea_index_milli = get_i32(data + 20);
    metrics.central_apnea_index_milli = get_i32(data + 24);
    metrics.unknown_apnea_index_milli = get_i32(data + 28);
    metrics.hypopnea_index_milli = get_i32(data + 32);
    metrics.arousal_index_milli = get_i32(data + 36);
    metrics.mask_pressure_50_milli = get_i32(data + 40);
    metrics.leak_50_milli = get_i32(data + 44);
    metrics.duration_minutes = get_le32(data + 48);
    metrics.mask_pressure_95_milli = get_i32(data + 52);
    metrics.leak_95_milli = get_i32(data + 56);
    metrics.minute_ventilation_50_milli = get_i32(data + 60);
    metrics.minute_ventilation_95_milli = get_i32(data + 64);
    metrics.respiratory_rate_50_milli = get_i32(data + 68);
    metrics.respiratory_rate_95_milli = get_i32(data + 72);
    metrics.tidal_volume_50_milli = get_i32(data + 76);
    metrics.tidal_volume_95_milli = get_i32(data + 80);
    metrics.spo2_median_milli = get_i32(data + 84);
    metrics.spo2_threshold_minutes = get_le32(data + 88);
    metrics.csr_minutes = get_le32(data + 92);
}

void decode_metrics_v1(const uint8_t *data,
                       ReportArtifactMetrics &metrics) {
    metrics.valid_mask = get_le16(data);
    metrics.str_mask = get_le16(data + 2);
    metrics.summary_mask = get_le16(data + 4);
    metrics.ahi_milli = get_i32(data + 8);
    metrics.obstructive_apnea_index_milli = get_i32(data + 12);
    metrics.central_apnea_index_milli = get_i32(data + 16);
    metrics.unknown_apnea_index_milli = get_i32(data + 20);
    metrics.hypopnea_index_milli = get_i32(data + 24);
    metrics.arousal_index_milli = get_i32(data + 28);
    metrics.mask_pressure_50_milli = get_i32(data + 32);
    metrics.leak_50_milli = get_i32(data + 36);
}

void encode_events(uint8_t *out, const ReportArtifactEventCounts &events) {
    put_le32(out, events.hypopnea);
    put_le32(out + 4, events.central_apnea);
    put_le32(out + 8, events.obstructive_apnea);
    put_le32(out + 12, events.unknown_apnea);
    put_le32(out + 16, events.arousal);
    put_le32(out + 20, events.csr);
}

void decode_events(const uint8_t *data, ReportArtifactEventCounts &events) {
    events.hypopnea = get_le32(data);
    events.central_apnea = get_le32(data + 4);
    events.obstructive_apnea = get_le32(data + 8);
    events.unknown_apnea = get_le32(data + 12);
    events.arousal = get_le32(data + 16);
    events.csr = get_le32(data + 20);
}

bool artifact_path(const ReportArtifactKey &key,
                   const char *suffix,
                   char *out,
                   size_t out_size) {
    if (!key.sleep_day.valid() || !key.source_revision.valid() || !suffix ||
        !out || out_size == 0) {
        return false;
    }

    char day[9] = {};
    if (!key.sleep_day.format_yyyymmdd(day, sizeof(day))) return false;

    const int written = snprintf(
        out,
        out_size,
        "%s/%s/%016llx.%s",
        REPORT_ARTIFACT_ROOT,
        day,
        static_cast<unsigned long long>(key.source_revision.value()),
        suffix);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace

bool ReportResultArtifactView::session(
    size_t index,
    NightCatalogTimeRange &range) const {
    if (!session_bytes || index >= data.session_count) return false;

    const uint8_t *record = session_bytes + index *
        ReportResultArtifactCodec::SessionBytes;
    range.start_ms = get_i64(record);
    range.end_ms = get_i64(record + 8);
    return range.valid();
}

bool ReportArtifactManifestView::tile(
    size_t index,
    ReportRangeTileArtifact &tile_out) const {
    if (!tile_bytes || index >= tile_count) return false;

    const uint8_t *record = tile_bytes + index *
        ReportArtifactManifestCodec::TileBytes;
    tile_out.start_ms = get_i64(record);
    tile_out.end_ms = get_i64(record + 8);
    tile_out.size = get_le32(record + 16);
    tile_out.crc32 = get_le32(record + 20);
    tile_out.prefix_crc32 = get_le32(record + 24);
    return tile_out.end_ms > tile_out.start_ms && tile_out.size > 0;
}

bool ReportArtifactBundle::valid() const {
    if (!key.valid()) return false;
    if (key.kind == ReportArtifactKind::RangeTile) {
        if (range_tile_count == 0 ||
            range_tile_count > REPORT_RANGE_TILE_BATCH_MAX ||
            range_tiles[0].key != key) {
            return false;
        }
        for (size_t i = 0; i < range_tile_count; ++i) {
            if (!range_tiles[i].valid() ||
                range_tiles[i].key.sleep_day != key.sleep_day ||
                range_tiles[i].key.source_revision != key.source_revision ||
                (i > 0 && range_tiles[i].key.range_start_ms !=
                              range_tiles[i - 1].key.range_end_ms)) {
                return false;
            }
        }
        return true;
    }
    return range_tile_count == 0 && key_is_result(key) && result &&
           result->size() > 0 && overview && overview->size() > 0 &&
           manifest && manifest->size() > 0;
}

bool ReportRangeTilePayload::valid() const {
    return key.kind == ReportArtifactKind::RangeTile && key.valid() && bytes &&
           bytes->size() > 0 && bytes->size() <= UINT32_MAX;
}

const ReportRangeTilePayload *ReportArtifactBundle::range_tile(
    size_t index) const {
    return index < range_tile_count && range_tiles[index].valid()
        ? &range_tiles[index]
        : nullptr;
}

bool ReportArtifactDescriptor::valid() const {
    return key.valid() && size > 0 && size <= UINT32_MAX;
}

bool ReportArtifactDescriptor::path(char *out, size_t out_size) const {
    if (!valid()) return false;

    switch (key.kind) {
        case ReportArtifactKind::Result:
            return report_artifact_result_path(key, out, out_size);
        case ReportArtifactKind::Overview:
            return report_artifact_overview_path(key, out, out_size);
        case ReportArtifactKind::RangeTile:
            return report_artifact_tile_path(key, out, out_size);
    }
    return false;
}

bool ReportArtifactAvailability::pair_ready() const {
    return result.valid() && overview.valid() &&
           result.key.kind == ReportArtifactKind::Result &&
           overview.key.kind == ReportArtifactKind::Overview &&
           result.key.sleep_day == request.sleep_day &&
           result.key.source_revision == request.source_revision &&
           overview.key.sleep_day == request.sleep_day &&
           overview.key.source_revision == request.source_revision;
}

bool ReportArtifactAvailability::requested_ready() const {
    if (!request.valid() || !pair_ready()) return false;
    if (request.kind != ReportArtifactKind::RangeTile) return true;

    if (requested_range_tile_count == 0 ||
        requested_range_tile_count > REPORT_RANGE_TILE_BATCH_MAX) {
        return false;
    }

    for (size_t requested_index = 0;
         requested_index < requested_range_tile_count;
         ++requested_index) {
        const int64_t offset = static_cast<int64_t>(requested_index) *
            REPORT_RANGE_TILE_MS;
        const ReportArtifactKey key = ReportArtifactKey::range_tile(
            request.sleep_day,
            request.source_revision,
            request.range_start_ms + offset,
            request.range_end_ms + offset);
        bool found = false;
        for (size_t i = 0; i < range_tile_count; ++i) {
            if (range_tiles[i].valid() && range_tiles[i].key == key) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

const ReportArtifactDescriptor *ReportArtifactAvailability::range_tile(
    size_t index) const {
    return index < range_tile_count && range_tiles[index].valid()
        ? &range_tiles[index]
        : nullptr;
}

bool ReportArtifactAvailability::descriptor(
    const ReportArtifactKey &key,
    ReportArtifactDescriptor &out) const {
    out = {};
    if (!key.valid() || !pair_ready() ||
        key.sleep_day != request.sleep_day ||
        key.source_revision != request.source_revision) {
        return false;
    }

    switch (key.kind) {
        case ReportArtifactKind::Result:
            out = result;
            return true;
        case ReportArtifactKind::Overview:
            out = overview;
            return true;
        case ReportArtifactKind::RangeTile:
            for (size_t i = 0; i < range_tile_count; ++i) {
                if (range_tiles[i].valid() && range_tiles[i].key == key) {
                    out = range_tiles[i];
                    return true;
                }
            }
            return false;
    }
    return false;
}

bool ReportArtifactAvailability::load(
    const ReportArtifactManifestView &manifest,
    const ReportArtifactKey &requested,
    uint64_t manifest_modified,
    uint8_t requested_tile_count) {
    *this = {};
    if (!requested.valid() || !key_is_result(manifest.key) ||
        manifest.key.sleep_day != requested.sleep_day ||
        manifest.key.source_revision != requested.source_revision ||
        !report_artifact_batch_count_valid(
            requested.kind, requested_tile_count)) {
        return false;
    }

    request = requested;
    requested_range_tile_count = requested_tile_count;
    result.key = manifest.key;
    result.size = manifest.result_size;
    result.crc32 = manifest.result_crc32;
    result.manifest_modified = manifest_modified;
    overview.key = ReportArtifactKey::overview(
        manifest.key.sleep_day, manifest.key.source_revision);
    overview.size = manifest.overview_size;
    overview.crc32 = manifest.overview_crc32;
    overview.prefix_crc32 = manifest.overview_prefix_crc32;
    overview.manifest_modified = manifest_modified;
    if (!pair_ready()) {
        *this = {};
        return false;
    }

    if (requested.kind != ReportArtifactKind::RangeTile) return true;
    for (size_t i = 0; i < manifest.tile_count; ++i) {
        ReportRangeTileArtifact tile;
        if (!manifest.tile(i, tile)) {
            *this = {};
            return false;
        }
        if (tile.start_ms < requested.range_start_ms ||
            tile.end_ms <= requested.range_start_ms) {
            continue;
        }

        const int64_t offset = tile.start_ms - requested.range_start_ms;
        if (offset < 0 || offset % REPORT_RANGE_TILE_MS != 0) continue;

        const size_t requested_index = static_cast<size_t>(
            offset / REPORT_RANGE_TILE_MS);
        if (requested_index >= requested_tile_count ||
            tile.end_ms != requested.range_end_ms + offset) {
            continue;
        }

        ReportArtifactDescriptor &descriptor =
            range_tiles[range_tile_count++];
        descriptor.key = ReportArtifactKey::range_tile(
            requested.sleep_day,
            requested.source_revision,
            tile.start_ms,
            tile.end_ms);
        descriptor.size = tile.size;
        descriptor.crc32 = tile.crc32;
        descriptor.prefix_crc32 = tile.prefix_crc32;
        descriptor.manifest_modified = manifest_modified;
    }
    return true;
}

bool ReportArtifactAvailability::merge(
    const ReportArtifactBundle &bundle,
    uint64_t manifest_modified) {
    if (!request.valid() || !bundle.valid() ||
        bundle.key.sleep_day != request.sleep_day ||
        bundle.key.source_revision != request.source_revision) {
        return false;
    }

    if (bundle.key.kind == ReportArtifactKind::Result) {
        result.key = bundle.key;
        result.size = bundle.result->size();
        result.crc32 = bundle.result_crc32;
        result.manifest_modified = manifest_modified;
        overview.key = ReportArtifactKey::overview(
            bundle.key.sleep_day, bundle.key.source_revision);
        overview.size = bundle.overview->size();
        overview.crc32 = bundle.overview_crc32;
        overview.prefix_crc32 = bundle.overview_prefix_crc32;
        overview.manifest_modified = manifest_modified;
        return pair_ready();
    }
    if (bundle.key.kind != ReportArtifactKind::RangeTile) {
        return false;
    }

    range_tile_count = 0;
    for (size_t i = 0; i < bundle.range_tile_count; ++i) {
        const ReportRangeTilePayload *tile = bundle.range_tile(i);
        if (!tile || range_tile_count >= REPORT_RANGE_TILE_BATCH_MAX) {
            return false;
        }

        ReportArtifactDescriptor &descriptor =
            range_tiles[range_tile_count++];
        descriptor.key = tile->key;
        descriptor.size = tile->bytes->size();
        descriptor.crc32 = tile->crc32;
        descriptor.prefix_crc32 = tile->prefix_crc32;
        descriptor.manifest_modified = manifest_modified;
    }
    result.manifest_modified = manifest_modified;
    overview.manifest_modified = manifest_modified;
    return requested_ready();
}

std::shared_ptr<const LargeByteBuffer> ReportResultArtifactCodec::encode(
    const ReportResultArtifactData &data) {
    if (!key_is_result(data.key) || data.day_end_ms <= data.day_start_ms ||
        data.session_count > UINT16_MAX ||
        !valid_result_session_ranges(data)) {
        return {};
    }

    size_t body_bytes = 0;
    size_t total_bytes = 0;
    if (!CheckedSize::multiply(data.session_count, SessionBytes, body_bytes) ||
        !CheckedSize::add(HeaderBytes, body_bytes, total_bytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    put_le32(bytes, RESULT_MAGIC);
    put_le16(bytes + 4, Version);
    put_le16(bytes + 6, HeaderBytes);
    put_le32(bytes + 8, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 12, data.key.sleep_day.epoch_days());
    put_le64(bytes + 16, data.key.source_revision.value());
    put_i64(bytes + 24, data.day_start_ms);
    put_i64(bytes + 32, data.day_end_ms);
    put_i64(bytes + 40, data.therapy_start_ms);
    put_i64(bytes + 48, data.therapy_end_ms);
    put_le32(bytes + 56, data.duration_min);
    put_le32(bytes + 60, data.requested_signal_mask);
    put_le32(bytes + 64, data.available_signal_mask);
    put_le32(bytes + 68, data.missing_required_signal_mask);
    put_le32(bytes + 72, data.missing_optional_signal_mask);
    put_le16(bytes + 76, data.flags);
    put_le16(bytes + 78, static_cast<uint16_t>(data.session_count));
    bytes[80] = data.requested_event_mask;
    bytes[81] = data.missing_event_mask;
    bytes[82] = data.source_flags;
    encode_metrics(bytes + 84, data.metrics);
    encode_events(bytes + 180, data.events);

    uint8_t *body = bytes + HeaderBytes;
    for (size_t i = 0; i < data.session_count; ++i) {
        const NightCatalogTimeRange &session = data.sessions[i];
        put_i64(body + i * SessionBytes, session.start_ms);
        put_i64(body + i * SessionBytes + 8, session.end_ms);
    }

    put_le32(bytes + RESULT_BODY_CRC_OFFSET,
            crc32_ieee(body, body_bytes));
    put_le32(bytes + RESULT_HEADER_CRC_OFFSET,
            crc32_ieee(bytes, RESULT_HEADER_CRC_OFFSET));
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportResultArtifactCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportResultArtifactView &view) {
    view = {};
    if (!bytes || length < LegacyHeaderBytes ||
        get_le32(bytes) != RESULT_MAGIC || get_le32(bytes + 8) != length) {
        return false;
    }

    const uint16_t version = get_le16(bytes + 4);
    const bool legacy = version == LegacyVersion;
    const size_t header_bytes = legacy ? LegacyHeaderBytes : HeaderBytes;
    const size_t body_crc_offset = legacy
        ? RESULT_V1_BODY_CRC_OFFSET : RESULT_BODY_CRC_OFFSET;
    const size_t header_crc_offset = legacy
        ? RESULT_V1_HEADER_CRC_OFFSET : RESULT_HEADER_CRC_OFFSET;
    const size_t events_offset = legacy ? 124 : 180;
    if ((!legacy && version != Version) ||
        get_le16(bytes + 6) != header_bytes || length < header_bytes ||
        crc32_ieee(bytes, header_crc_offset) !=
            get_le32(bytes + header_crc_offset)) {
        return false;
    }

    const size_t session_count = get_le16(bytes + 78);
    size_t body_bytes = 0;
    size_t expected = 0;
    if (!CheckedSize::multiply(session_count, SessionBytes, body_bytes) ||
        !CheckedSize::add(header_bytes, body_bytes, expected) ||
        expected != length ||
        crc32_ieee(bytes + header_bytes, body_bytes) !=
            get_le32(bytes + body_crc_offset)) {
        return false;
    }

    SleepDayId sleep_day;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 12), sleep_day)) {
        return false;
    }

    ReportResultArtifactData &data = view.data;
    data.key = ReportArtifactKey::result(
        sleep_day, SourceRevision(get_le64(bytes + 16)));
    data.day_start_ms = get_i64(bytes + 24);
    data.day_end_ms = get_i64(bytes + 32);
    data.therapy_start_ms = get_i64(bytes + 40);
    data.therapy_end_ms = get_i64(bytes + 48);
    data.duration_min = get_le32(bytes + 56);
    data.requested_signal_mask = get_le32(bytes + 60);
    data.available_signal_mask = get_le32(bytes + 64);
    data.missing_required_signal_mask = get_le32(bytes + 68);
    data.missing_optional_signal_mask = get_le32(bytes + 72);
    data.flags = get_le16(bytes + 76);
    data.session_count = session_count;
    data.requested_event_mask = bytes[80];
    data.missing_event_mask = bytes[81];
    data.source_flags = bytes[82];
    if (legacy) decode_metrics_v1(bytes + 84, data.metrics);
    else decode_metrics(bytes + 84, data.metrics);
    decode_events(bytes + events_offset, data.events);
    view.session_bytes = bytes + header_bytes;

    if (!data.key.valid() || data.day_end_ms <= data.day_start_ms) {
        view = {};
        return false;
    }

    NightCatalogTimeRange previous;
    for (size_t i = 0; i < session_count; ++i) {
        NightCatalogTimeRange session;
        if (!view.session(i, session) || session.start_ms < data.day_start_ms ||
            session.end_ms > data.day_end_ms ||
            (i > 0 && session.start_ms < previous.end_ms) ||
            (i == 0 && session.start_ms != data.therapy_start_ms) ||
            (i + 1 == session_count &&
             session.end_ms != data.therapy_end_ms)) {
            view = {};
            return false;
        }
        previous = session;
    }
    if (session_count == 0 &&
        (data.therapy_start_ms != 0 || data.therapy_end_ms != 0)) {
        view = {};
        return false;
    }
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactManifestCodec::encode(
    const ReportArtifactBundle &bundle,
    const ReportRangeTileArtifact *tiles,
    size_t tile_count) {
    if (!key_is_result(bundle.key) || !bundle.result || !bundle.overview ||
        bundle.result->size() == 0 || bundle.overview->size() == 0 ||
        tile_count > MaxTiles || (tile_count > 0 && !tiles)) {
        return {};
    }

    ManifestHeaderFields fields;
    fields.key = bundle.key;
    fields.result_size = bundle.result->size();
    fields.overview_size = bundle.overview->size();
    fields.result_crc32 = bundle.result_crc32;
    fields.overview_crc32 = bundle.overview_crc32;
    fields.overview_prefix_crc32 = bundle.overview_prefix_crc32;

    return encode_manifest(fields, tile_count,
                           [tiles](size_t index,
                                   ReportRangeTileArtifact &tile) {
                               tile = tiles[index];
                               return true;
                           });
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactManifestCodec::add_tile(
    const ReportArtifactManifestView &manifest,
    const ReportRangeTileArtifact &tile) {
    return add_tiles(manifest, &tile, 1);
}

std::shared_ptr<const LargeByteBuffer> ReportArtifactManifestCodec::add_tiles(
    const ReportArtifactManifestView &manifest,
    const ReportRangeTileArtifact *tiles,
    size_t tile_count) {
    if (!key_is_result(manifest.key) || manifest.result_size == 0 ||
        manifest.overview_size == 0 || tile_count == 0 || !tiles ||
        tile_count > REPORT_RANGE_TILE_BATCH_MAX ||
        manifest.tile_count > MaxTiles ||
        manifest.tile_count > SIZE_MAX - tile_count) {
        return {};
    }

    LargeScratchArray<ReportRangeTileArtifact> merged;
    if (!merged.allocate(manifest.tile_count + tile_count)) return {};

    for (size_t i = 0; i < manifest.tile_count; ++i) {
        ReportRangeTileArtifact existing;
        if (!manifest.tile(i, existing)) return {};
        ReportRangeTileArtifact *slot = merged.append();
        if (!slot) return {};
        *slot = existing;
    }

    for (size_t i = 0; i < tile_count; ++i) {
        if (!report_range_tile_artifact_valid(tiles[i])) return {};

        bool replaced = false;
        for (size_t current = 0; current < merged.size(); ++current) {
            if (merged.data()[current].start_ms == tiles[i].start_ms &&
                merged.data()[current].end_ms == tiles[i].end_ms) {
                merged.data()[current] = tiles[i];
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            ReportRangeTileArtifact *slot = merged.append();
            if (!slot) return {};
            *slot = tiles[i];
        }
    }
    if (merged.size() > MaxTiles) return {};

    std::sort(
        merged.data(),
        merged.data() + merged.size(),
        [](const ReportRangeTileArtifact &lhs,
           const ReportRangeTileArtifact &rhs) {
            return lhs.start_ms < rhs.start_ms ||
                (lhs.start_ms == rhs.start_ms &&
                 lhs.end_ms < rhs.end_ms);
        });
    for (size_t i = 1; i < merged.size(); ++i) {
        if (!tile_follows(merged.data()[i - 1], merged.data()[i])) return {};
    }

    ManifestHeaderFields fields;
    fields.key = manifest.key;
    fields.result_size = manifest.result_size;
    fields.overview_size = manifest.overview_size;
    fields.result_crc32 = manifest.result_crc32;
    fields.overview_crc32 = manifest.overview_crc32;
    fields.overview_prefix_crc32 = manifest.overview_prefix_crc32;

    return encode_manifest(
        fields,
        merged.size(),
        [&merged](size_t output_index, ReportRangeTileArtifact &value) {
            value = merged.data()[output_index];
            return true;
        });
}

bool ReportArtifactManifestCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportArtifactManifestView &view) {
    view = {};
    if (!bytes || length < HeaderBytes || get_le32(bytes) != MANIFEST_MAGIC ||
        get_le16(bytes + 4) != Version || get_le16(bytes + 6) != HeaderBytes ||
        get_le32(bytes + 8) != length ||
        crc32_ieee(bytes, MANIFEST_HEADER_CRC_OFFSET) !=
            get_le32(bytes + MANIFEST_HEADER_CRC_OFFSET)) {
        return false;
    }

    const size_t tile_count = get_le16(bytes + 52);
    size_t body_bytes = 0;
    size_t expected = 0;
    if (!CheckedSize::multiply(tile_count, TileBytes, body_bytes) ||
        tile_count > MaxTiles ||
        !CheckedSize::add(HeaderBytes, body_bytes, expected) || expected != length ||
        crc32_ieee(bytes + HeaderBytes, body_bytes) !=
            get_le32(bytes + MANIFEST_BODY_CRC_OFFSET)) {
        return false;
    }

    SleepDayId sleep_day;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 12), sleep_day)) {
        return false;
    }

    view.key = ReportArtifactKey::result(
        sleep_day, SourceRevision(get_le64(bytes + 16)));
    view.result_size = get_le64(bytes + 24);
    view.overview_size = get_le64(bytes + 32);
    view.result_crc32 = get_le32(bytes + 40);
    view.overview_crc32 = get_le32(bytes + 44);
    view.overview_prefix_crc32 = get_le32(bytes + 48);
    view.tile_count = tile_count;
    view.tile_bytes = bytes + HeaderBytes;
    if (!view.key.valid() || view.result_size == 0 ||
        view.overview_size == 0) {
        view = {};
        return false;
    }

    ReportRangeTileArtifact previous;
    for (size_t i = 0; i < tile_count; ++i) {
        ReportRangeTileArtifact tile;
        if (!view.tile(i, tile) ||
            (i > 0 && !tile_follows(previous, tile))) {
            view = {};
            return false;
        }
        previous = tile;
    }
    return true;
}

bool report_artifact_result_path(
    const ReportArtifactKey &key,
    char *out,
    size_t out_size) {
    return key.kind == ReportArtifactKind::Result &&
           artifact_path(key, "result", out, out_size);
}

bool report_artifact_overview_path(
    const ReportArtifactKey &key,
    char *out,
    size_t out_size) {
    return (key.kind == ReportArtifactKind::Result ||
            key.kind == ReportArtifactKind::Overview) &&
           artifact_path(key, "overview", out, out_size);
}

bool report_artifact_manifest_path(
    SleepDayId sleep_day,
    char *out,
    size_t out_size) {
    if (!sleep_day.valid() || !out || out_size == 0) return false;

    char day[9] = {};
    if (!sleep_day.format_yyyymmdd(day, sizeof(day))) return false;
    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s/manifest",
                                 REPORT_ARTIFACT_ROOT,
                                 day);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool report_artifact_tile_path(
    const ReportArtifactKey &key,
    char *out,
    size_t out_size) {
    if (key.kind != ReportArtifactKind::RangeTile ||
        !key.valid() || !out || out_size == 0) {
        return false;
    }

    char day[9] = {};
    if (!key.sleep_day.format_yyyymmdd(day, sizeof(day))) return false;
    const int written = snprintf(
        out,
        out_size,
        "%s/%s/%016llx-%lld-%lld.tile",
        REPORT_ARTIFACT_ROOT,
        day,
        static_cast<unsigned long long>(key.source_revision.value()),
        static_cast<long long>(key.range_start_ms),
        static_cast<long long>(key.range_end_ms));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace aircannect
