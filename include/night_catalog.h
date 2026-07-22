#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "report_artifact_key.h"

namespace aircannect {

static constexpr uint16_t NIGHT_CATALOG_NO_SESSION = UINT16_MAX;

enum class NightCatalogFileKind : uint8_t {
    Brp,
    Pld,
    Sa2,
    Eve,
    Csl,
    Str,
};

enum class NightCatalogMetric : uint8_t {
    Ahi,
    ObstructiveApneaIndex,
    CentralApneaIndex,
    UnknownApneaIndex,
    HypopneaIndex,
    ArousalIndex,
    MaskPressure50,
    Leak50,
    DurationMinutes,
    Count,
};

enum class NightCatalogMetricSource : uint8_t {
    None,
    Str,
    Summary,
};

enum NightCatalogSourceFlag : uint8_t {
    NIGHT_CATALOG_SOURCE_EDF = 1u << 0,
    NIGHT_CATALOG_SOURCE_STR = 1u << 1,
    NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK = 1u << 2,
};

struct NightCatalogTimeRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0;

    bool valid() const { return end_ms > start_ms; }
};

struct NightCatalogMetrics {
    uint16_t valid_mask = 0;
    uint16_t str_mask = 0;
    uint16_t summary_mask = 0;

    float ahi = 0.0f;
    float obstructive_apnea_index = 0.0f;
    float central_apnea_index = 0.0f;
    float unknown_apnea_index = 0.0f;
    float hypopnea_index = 0.0f;
    float arousal_index = 0.0f;
    float mask_pressure_50_cm_h2o = 0.0f;
    float leak_50_l_min = 0.0f;
    uint32_t duration_min = 0;

    bool has(NightCatalogMetric metric) const;
    NightCatalogMetricSource source(NightCatalogMetric metric) const;
};

struct NightCatalogSourceCoverage {
    NightCatalogTimeRange range;
    uint32_t primary_signal_mask = 0;
    uint32_t fallback_signal_mask = 0;
};

struct NightCatalogSourceFile {
    NightCatalogFileKind kind = NightCatalogFileKind::Brp;
    uint32_t path_offset = 0;
    uint16_t path_length = 0;
    uint16_t session_index = 0;
    uint32_t coverage_offset = 0;
    uint16_t coverage_count = 0;

    uint64_t file_size = 0;
    int64_t last_write_ms = 0;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;
    uint64_t identity = 0;
    int64_t record_start_ms = 0;

    uint32_t header_size = 0;
    uint32_t record_size = 0;
    uint32_t record_duration_ms = 0;
    uint32_t complete_records = 0;
};

struct NightCatalogRecord {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;

    uint32_t session_offset = 0;
    uint16_t session_count = 0;
    uint32_t mask_window_offset = 0;
    uint16_t mask_window_count = 0;
    uint32_t file_offset = 0;
    uint16_t file_count = 0;

    uint8_t source_flags = 0;
    uint64_t summary_identity = 0;
    NightCatalogMetrics metrics;
};

class NightCatalogBuilder;
class NightCatalogFileCodec;

class NightCatalog {
public:
    ~NightCatalog();

    NightCatalog(const NightCatalog &) = delete;
    NightCatalog &operator=(const NightCatalog &) = delete;

    size_t size() const { return record_count_; }
    size_t storage_bytes() const { return storage_bytes_; }

    const NightCatalogRecord *record(size_t index) const;
    const NightCatalogRecord *find(SleepDayId sleep_day) const;
    const NightCatalogTimeRange *sessions(const NightCatalogRecord &record,
                                          size_t &count) const;
    const NightCatalogTimeRange *mask_windows(
        const NightCatalogRecord &record,
        size_t &count) const;
    const NightCatalogSourceFile *files(const NightCatalogRecord &record,
                                        size_t &count) const;
    const NightCatalogSourceCoverage *coverage(
        const NightCatalogSourceFile &file,
        size_t &count) const;
    const char *path(const NightCatalogSourceFile &file) const;

private:
    NightCatalog() = default;

    bool allocate(size_t record_count,
                  size_t session_count,
                  size_t mask_window_count,
                  size_t file_count,
                  size_t coverage_count,
                  size_t path_bytes);

    uint8_t *storage_ = nullptr;
    size_t storage_bytes_ = 0;
    NightCatalogRecord *records_ = nullptr;
    NightCatalogTimeRange *sessions_ = nullptr;
    NightCatalogTimeRange *mask_windows_ = nullptr;
    NightCatalogSourceFile *files_ = nullptr;
    NightCatalogSourceCoverage *coverage_ = nullptr;
    char *paths_ = nullptr;
    size_t record_count_ = 0;
    size_t session_count_ = 0;
    size_t mask_window_count_ = 0;
    size_t file_count_ = 0;
    size_t coverage_count_ = 0;
    size_t path_bytes_ = 0;

    friend class NightCatalogBuilder;
    friend class NightCatalogFileCodec;
};

}  // namespace aircannect
