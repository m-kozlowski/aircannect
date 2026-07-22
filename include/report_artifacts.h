#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "night_catalog.h"
#include "report_artifact_key.h"

namespace aircannect {

static constexpr const char *REPORT_ARTIFACT_ROOT =
    "/aircannect/report/v6";

enum ReportResultArtifactFlag : uint16_t {
    REPORT_RESULT_COMPLETE = 1u << 0,
    REPORT_RESULT_EVENTS_AVAILABLE = 1u << 1,
};

struct ReportArtifactMetrics {
    uint16_t valid_mask = 0;
    uint16_t str_mask = 0;
    uint16_t summary_mask = 0;
    int32_t ahi_milli = 0;
    int32_t obstructive_apnea_index_milli = 0;
    int32_t central_apnea_index_milli = 0;
    int32_t unknown_apnea_index_milli = 0;
    int32_t hypopnea_index_milli = 0;
    int32_t arousal_index_milli = 0;
    int32_t mask_pressure_50_milli = 0;
    int32_t leak_50_milli = 0;
};

struct ReportArtifactEventCounts {
    uint32_t hypopnea = 0;
    uint32_t central_apnea = 0;
    uint32_t obstructive_apnea = 0;
    uint32_t unknown_apnea = 0;
    uint32_t arousal = 0;
    uint32_t csr = 0;
};

struct ReportResultArtifactData {
    ReportArtifactKey key;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    int64_t therapy_start_ms = 0;
    int64_t therapy_end_ms = 0;
    uint32_t duration_min = 0;
    uint32_t requested_signal_mask = 0;
    uint32_t available_signal_mask = 0;
    uint32_t missing_required_signal_mask = 0;
    uint32_t missing_optional_signal_mask = 0;
    uint16_t flags = 0;
    uint8_t requested_event_mask = 0;
    uint8_t missing_event_mask = 0;
    uint8_t source_flags = 0;
    ReportArtifactMetrics metrics;
    ReportArtifactEventCounts events;
    const NightCatalogTimeRange *sessions = nullptr;
    size_t session_count = 0;
};

struct ReportResultArtifactView {
    ReportResultArtifactData data;
    const uint8_t *session_bytes = nullptr;

    bool session(size_t index, NightCatalogTimeRange &range) const;
};

struct ReportRangeTileArtifact {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint64_t size = 0;
    uint32_t crc32 = 0;
};

struct ReportArtifactManifestView {
    ReportArtifactKey key;
    uint64_t result_size = 0;
    uint64_t overview_size = 0;
    uint32_t result_crc32 = 0;
    uint32_t overview_crc32 = 0;
    const uint8_t *tile_bytes = nullptr;
    size_t tile_count = 0;

    bool tile(size_t index, ReportRangeTileArtifact &tile) const;
};

struct ReportArtifactDescriptor {
    ReportArtifactKey key;
    uint64_t size = 0;
    uint32_t crc32 = 0;

    bool valid() const;
    bool path(char *out, size_t out_size) const;
};

struct ReportArtifactBundle {
    ReportArtifactKey key;
    std::shared_ptr<const LargeByteBuffer> result;
    std::shared_ptr<const LargeByteBuffer> overview;
    std::shared_ptr<const LargeByteBuffer> range_tile;
    std::shared_ptr<const LargeByteBuffer> manifest;
    uint32_t result_crc32 = 0;
    uint32_t overview_crc32 = 0;
    uint32_t range_tile_crc32 = 0;

    bool valid() const;
};

struct ReportArtifactAvailability {
    ReportArtifactKey request;
    ReportArtifactDescriptor result;
    ReportArtifactDescriptor overview;
    ReportArtifactDescriptor range_tile;

    bool pair_ready() const;
    bool requested_ready() const;
    bool descriptor(const ReportArtifactKey &key,
                    ReportArtifactDescriptor &out) const;
    bool load(const ReportArtifactManifestView &manifest,
              const ReportArtifactKey &requested);
    bool merge(const ReportArtifactBundle &bundle);
};

class ReportResultArtifactCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t HeaderBytes = 160;
    static constexpr size_t SessionBytes = 16;

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportResultArtifactData &data);
    static bool decode(const uint8_t *bytes,
                       size_t length,
                       ReportResultArtifactView &view);
};

class ReportArtifactManifestCodec {
public:
    static constexpr uint16_t Version = 1;
    static constexpr size_t HeaderBytes = 72;
    static constexpr size_t TileBytes = 24;
    static constexpr size_t MaxTiles = 128;
    static constexpr size_t MaxBytes = HeaderBytes + MaxTiles * TileBytes;

    static std::shared_ptr<const LargeByteBuffer> encode(
        const ReportArtifactBundle &bundle,
        const ReportRangeTileArtifact *tiles = nullptr,
        size_t tile_count = 0);
    static std::shared_ptr<const LargeByteBuffer> add_tile(
        const ReportArtifactManifestView &manifest,
        const ReportRangeTileArtifact &tile);
    static bool decode(const uint8_t *bytes,
                       size_t length,
                       ReportArtifactManifestView &view);
};

bool report_artifact_result_path(const ReportArtifactKey &key,
                                 char *out,
                                 size_t out_size);
bool report_artifact_overview_path(const ReportArtifactKey &key,
                                   char *out,
                                   size_t out_size);
bool report_artifact_manifest_path(SleepDayId sleep_day,
                                   char *out,
                                   size_t out_size);
bool report_artifact_tile_path(const ReportArtifactKey &key,
                               char *out,
                               size_t out_size);

}  // namespace aircannect
