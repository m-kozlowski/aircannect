#include "report_artifacts.h"

#include <limits>
#include <stdio.h>
#include <string.h>

#include "crc32.h"

namespace aircannect {
namespace {

constexpr uint32_t RESULT_MAGIC = 0x36524341u;    // "ACR6"
constexpr uint32_t MANIFEST_MAGIC = 0x364d4341u;  // "ACM6"

constexpr size_t RESULT_BODY_CRC_OFFSET = 148;
constexpr size_t RESULT_HEADER_CRC_OFFSET = 152;
constexpr size_t MANIFEST_BODY_CRC_OFFSET = 60;
constexpr size_t MANIFEST_HEADER_CRC_OFFSET = 64;

void put_u16(uint8_t *out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

void put_u64(uint8_t *out, uint64_t value) {
    put_u32(out, static_cast<uint32_t>(value));
    put_u32(out + 4, static_cast<uint32_t>(value >> 32));
}

void put_i32(uint8_t *out, int32_t value) {
    put_u32(out, static_cast<uint32_t>(value));
}

void put_i64(uint8_t *out, int64_t value) {
    put_u64(out, static_cast<uint64_t>(value));
}

uint16_t get_u16(const uint8_t *data) {
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(data[1]) << 8;
}

uint32_t get_u32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) |
           static_cast<uint32_t>(data[1]) << 8 |
           static_cast<uint32_t>(data[2]) << 16 |
           static_cast<uint32_t>(data[3]) << 24;
}

uint64_t get_u64(const uint8_t *data) {
    return static_cast<uint64_t>(get_u32(data)) |
           static_cast<uint64_t>(get_u32(data + 4)) << 32;
}

int32_t get_i32(const uint8_t *data) {
    return static_cast<int32_t>(get_u32(data));
}

int64_t get_i64(const uint8_t *data) {
    return static_cast<int64_t>(get_u64(data));
}

bool multiply_size(size_t count, size_t width, size_t &bytes) {
    if (count > std::numeric_limits<size_t>::max() / width) return false;
    bytes = count * width;
    return true;
}

bool add_size(size_t lhs, size_t rhs, size_t &total) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) return false;
    total = lhs + rhs;
    return true;
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

bool valid_tile(const ReportRangeTileArtifact &tile) {
    return tile.end_ms > tile.start_ms && tile.size > 0 &&
           tile.size <= UINT32_MAX;
}

bool tile_follows(const ReportRangeTileArtifact &previous,
                  const ReportRangeTileArtifact &tile) {
    return tile.start_ms > previous.start_ms ||
           (tile.start_ms == previous.start_ms &&
            tile.end_ms > previous.end_ms);
}

void encode_metrics(uint8_t *out, const ReportArtifactMetrics &metrics) {
    put_u16(out, metrics.valid_mask);
    put_u16(out + 2, metrics.str_mask);
    put_u16(out + 4, metrics.summary_mask);
    put_i32(out + 8, metrics.ahi_milli);
    put_i32(out + 12, metrics.obstructive_apnea_index_milli);
    put_i32(out + 16, metrics.central_apnea_index_milli);
    put_i32(out + 20, metrics.unknown_apnea_index_milli);
    put_i32(out + 24, metrics.hypopnea_index_milli);
    put_i32(out + 28, metrics.arousal_index_milli);
    put_i32(out + 32, metrics.mask_pressure_50_milli);
    put_i32(out + 36, metrics.leak_50_milli);
}

void decode_metrics(const uint8_t *data, ReportArtifactMetrics &metrics) {
    metrics.valid_mask = get_u16(data);
    metrics.str_mask = get_u16(data + 2);
    metrics.summary_mask = get_u16(data + 4);
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
    put_u32(out, events.hypopnea);
    put_u32(out + 4, events.central_apnea);
    put_u32(out + 8, events.obstructive_apnea);
    put_u32(out + 12, events.unknown_apnea);
    put_u32(out + 16, events.arousal);
    put_u32(out + 20, events.csr);
}

void decode_events(const uint8_t *data, ReportArtifactEventCounts &events) {
    events.hypopnea = get_u32(data);
    events.central_apnea = get_u32(data + 4);
    events.obstructive_apnea = get_u32(data + 8);
    events.unknown_apnea = get_u32(data + 12);
    events.arousal = get_u32(data + 16);
    events.csr = get_u32(data + 20);
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
        "%s/%s-%016llx.%s",
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
    tile_out.size = get_u32(record + 16);
    tile_out.crc32 = get_u32(record + 20);
    return tile_out.end_ms > tile_out.start_ms && tile_out.size > 0;
}

bool ReportArtifactBundle::valid() const {
    return key_is_result(key) && result && result->size() > 0 && overview &&
           overview->size() > 0 && manifest && manifest->size() > 0;
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
    if (!multiply_size(data.session_count, SessionBytes, body_bytes) ||
        !add_size(HeaderBytes, body_bytes, total_bytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    put_u32(bytes, RESULT_MAGIC);
    put_u16(bytes + 4, Version);
    put_u16(bytes + 6, HeaderBytes);
    put_u32(bytes + 8, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 12, data.key.sleep_day.epoch_days());
    put_u64(bytes + 16, data.key.source_revision.value());
    put_i64(bytes + 24, data.day_start_ms);
    put_i64(bytes + 32, data.day_end_ms);
    put_i64(bytes + 40, data.therapy_start_ms);
    put_i64(bytes + 48, data.therapy_end_ms);
    put_u32(bytes + 56, data.duration_min);
    put_u32(bytes + 60, data.requested_signal_mask);
    put_u32(bytes + 64, data.available_signal_mask);
    put_u32(bytes + 68, data.missing_required_signal_mask);
    put_u32(bytes + 72, data.missing_optional_signal_mask);
    put_u16(bytes + 76, data.flags);
    put_u16(bytes + 78, static_cast<uint16_t>(data.session_count));
    bytes[80] = data.requested_event_mask;
    bytes[81] = data.missing_event_mask;
    bytes[82] = data.source_flags;
    encode_metrics(bytes + 84, data.metrics);
    encode_events(bytes + 124, data.events);

    uint8_t *body = bytes + HeaderBytes;
    for (size_t i = 0; i < data.session_count; ++i) {
        const NightCatalogTimeRange &session = data.sessions[i];
        put_i64(body + i * SessionBytes, session.start_ms);
        put_i64(body + i * SessionBytes + 8, session.end_ms);
    }

    put_u32(bytes + RESULT_BODY_CRC_OFFSET,
            crc32_ieee(body, body_bytes));
    put_u32(bytes + RESULT_HEADER_CRC_OFFSET,
            crc32_ieee(bytes, RESULT_HEADER_CRC_OFFSET));
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportResultArtifactCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportResultArtifactView &view) {
    view = {};
    if (!bytes || length < HeaderBytes || get_u32(bytes) != RESULT_MAGIC ||
        get_u16(bytes + 4) != Version || get_u16(bytes + 6) != HeaderBytes ||
        get_u32(bytes + 8) != length ||
        crc32_ieee(bytes, RESULT_HEADER_CRC_OFFSET) !=
            get_u32(bytes + RESULT_HEADER_CRC_OFFSET)) {
        return false;
    }

    const size_t session_count = get_u16(bytes + 78);
    size_t body_bytes = 0;
    size_t expected = 0;
    if (!multiply_size(session_count, SessionBytes, body_bytes) ||
        !add_size(HeaderBytes, body_bytes, expected) || expected != length ||
        crc32_ieee(bytes + HeaderBytes, body_bytes) !=
            get_u32(bytes + RESULT_BODY_CRC_OFFSET)) {
        return false;
    }

    SleepDayId sleep_day;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 12), sleep_day)) {
        return false;
    }

    ReportResultArtifactData &data = view.data;
    data.key = ReportArtifactKey::result(
        sleep_day, SourceRevision(get_u64(bytes + 16)));
    data.day_start_ms = get_i64(bytes + 24);
    data.day_end_ms = get_i64(bytes + 32);
    data.therapy_start_ms = get_i64(bytes + 40);
    data.therapy_end_ms = get_i64(bytes + 48);
    data.duration_min = get_u32(bytes + 56);
    data.requested_signal_mask = get_u32(bytes + 60);
    data.available_signal_mask = get_u32(bytes + 64);
    data.missing_required_signal_mask = get_u32(bytes + 68);
    data.missing_optional_signal_mask = get_u32(bytes + 72);
    data.flags = get_u16(bytes + 76);
    data.session_count = session_count;
    data.requested_event_mask = bytes[80];
    data.missing_event_mask = bytes[81];
    data.source_flags = bytes[82];
    decode_metrics(bytes + 84, data.metrics);
    decode_events(bytes + 124, data.events);
    view.session_bytes = bytes + HeaderBytes;

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
        tile_count > UINT16_MAX || (tile_count > 0 && !tiles)) {
        return {};
    }

    size_t body_bytes = 0;
    size_t total_bytes = 0;
    if (!multiply_size(tile_count, TileBytes, body_bytes) ||
        !add_size(HeaderBytes, body_bytes, total_bytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    put_u32(bytes, MANIFEST_MAGIC);
    put_u16(bytes + 4, Version);
    put_u16(bytes + 6, HeaderBytes);
    put_u32(bytes + 8, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 12, bundle.key.sleep_day.epoch_days());
    put_u64(bytes + 16, bundle.key.source_revision.value());
    put_u64(bytes + 24, bundle.result->size());
    put_u64(bytes + 32, bundle.overview->size());
    put_u32(bytes + 40, bundle.result_crc32);
    put_u32(bytes + 44, bundle.overview_crc32);
    put_u16(bytes + 48, static_cast<uint16_t>(tile_count));

    uint8_t *body = bytes + HeaderBytes;
    for (size_t i = 0; i < tile_count; ++i) {
        if (!valid_tile(tiles[i]) ||
            (i > 0 && !tile_follows(tiles[i - 1], tiles[i]))) {
            return {};
        }
        uint8_t *record = body + i * TileBytes;
        put_i64(record, tiles[i].start_ms);
        put_i64(record + 8, tiles[i].end_ms);
        put_u32(record + 16, static_cast<uint32_t>(tiles[i].size));
        put_u32(record + 20, tiles[i].crc32);
    }

    put_u32(bytes + MANIFEST_BODY_CRC_OFFSET,
            crc32_ieee(body, body_bytes));
    put_u32(bytes + MANIFEST_HEADER_CRC_OFFSET,
            crc32_ieee(bytes, MANIFEST_HEADER_CRC_OFFSET));
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportArtifactManifestCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportArtifactManifestView &view) {
    view = {};
    if (!bytes || length < HeaderBytes || get_u32(bytes) != MANIFEST_MAGIC ||
        get_u16(bytes + 4) != Version || get_u16(bytes + 6) != HeaderBytes ||
        get_u32(bytes + 8) != length ||
        crc32_ieee(bytes, MANIFEST_HEADER_CRC_OFFSET) !=
            get_u32(bytes + MANIFEST_HEADER_CRC_OFFSET)) {
        return false;
    }

    const size_t tile_count = get_u16(bytes + 48);
    size_t body_bytes = 0;
    size_t expected = 0;
    if (!multiply_size(tile_count, TileBytes, body_bytes) ||
        !add_size(HeaderBytes, body_bytes, expected) || expected != length ||
        crc32_ieee(bytes + HeaderBytes, body_bytes) !=
            get_u32(bytes + MANIFEST_BODY_CRC_OFFSET)) {
        return false;
    }

    SleepDayId sleep_day;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 12), sleep_day)) {
        return false;
    }

    view.key = ReportArtifactKey::result(
        sleep_day, SourceRevision(get_u64(bytes + 16)));
    view.result_size = get_u64(bytes + 24);
    view.overview_size = get_u64(bytes + 32);
    view.result_crc32 = get_u32(bytes + 40);
    view.overview_crc32 = get_u32(bytes + 44);
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
    return key.kind == ReportArtifactKind::Result &&
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
                                 "%s/%s.manifest",
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
        "%s/%s-%016llx-%lld-%lld.tile",
        REPORT_ARTIFACT_ROOT,
        day,
        static_cast<unsigned long long>(key.source_revision.value()),
        static_cast<long long>(key.range_start_ms),
        static_cast<long long>(key.range_end_ms));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace aircannect
