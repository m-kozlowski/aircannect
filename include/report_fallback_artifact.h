#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "night_catalog.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

static constexpr const char *REPORT_FALLBACK_ARTIFACT_ROOT =
    "/aircannect/report/v6/fallback";

struct ReportFallbackSectionInput {
    ReportFallbackSectionKind kind = ReportFallbackSectionKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Count;
    uint8_t event_mask = 0;
    uint32_t payload_schema = 0;
    uint32_t record_count = 0;
    uint32_t sample_interval_ms = 0;
    NightCatalogTimeRange coverage;
    const uint8_t *payload = nullptr;
    size_t payload_size = 0;
};

struct ReportFallbackSection {
    ReportFallbackSectionKind kind = ReportFallbackSectionKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Count;
    uint8_t event_mask = 0;
    uint32_t payload_schema = 0;
    uint32_t record_count = 0;
    uint32_t sample_interval_ms = 0;
    NightCatalogTimeRange coverage;
    uint64_t data_offset = 0;
    uint32_t data_size = 0;
    uint32_t data_crc32 = 0;
};

struct ReportFallbackArtifactInfo {
    SleepDayId sleep_day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    uint64_t content_identity = 0;
    uint32_t session_count = 0;
    uint32_t section_count = 0;
    size_t metadata_bytes = 0;
    size_t payload_bytes = 0;
    size_t total_bytes = 0;
};

struct ReportFallbackArtifactView {
    ReportFallbackArtifactInfo info;
    const uint8_t *session_bytes = nullptr;
    const uint8_t *section_bytes = nullptr;

    bool session(size_t index, NightCatalogTimeRange &out) const;
    bool section(size_t index, ReportFallbackSection &out) const;
};

class ReportFallbackArtifactCodec {
public:
    static constexpr uint16_t Version = 3;
    static constexpr size_t HeaderBytes = 72;
    static constexpr size_t SessionBytes = 16;
    static constexpr size_t SectionBytes = 48;
    static constexpr size_t MaxSessions = 256;
    static constexpr size_t MaxSections = 128;
    static constexpr size_t MaxMetadataBytes =
        HeaderBytes + MaxSessions * SessionBytes +
        MaxSections * SectionBytes;
    static constexpr size_t MaxFileBytes = 4 * 1024 * 1024;

    static bool inspect_header(const uint8_t *header,
                               size_t header_length,
                               ReportFallbackArtifactInfo &info);
    static bool decode_metadata(const uint8_t *metadata,
                                size_t metadata_length,
                                ReportFallbackArtifactView &view);
    static std::shared_ptr<const LargeByteBuffer> encode(
        SleepDayId sleep_day,
        int64_t day_start_ms,
        int64_t day_end_ms,
        const NightCatalogTimeRange *sessions,
        size_t session_count,
        const ReportFallbackSectionInput *sections,
        size_t section_count);
};

bool report_fallback_artifact_path(SleepDayId sleep_day,
                                   char *out,
                                   size_t out_size);

}  // namespace aircannect
