#pragma once

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

namespace aircannect {

// PLOT v6 is one immutable file with a fixed named section directory.
// Section names are the durable identities; planner enum values and catalog
// order never enter the wire or storage contract.
constexpr uint32_t PLOT_BIN_MAGIC = 0x42504341u;  // "ACPB"
constexpr uint16_t PLOT_BIN_VERSION = 6;
constexpr size_t PLOT_INDEX_HEADER_BYTES = 64;
constexpr size_t PLOT_INDEX_ENTRY_BYTES = 48;
constexpr size_t PLOT_INDEX_MAX_ENTRIES = 32;
constexpr size_t PLOT_SECTION_NAME_BYTES = 32;
constexpr size_t PLOT_INDEX_PREFIX_BYTES =
    PLOT_INDEX_HEADER_BYTES +
    PLOT_INDEX_ENTRY_BYTES * PLOT_INDEX_MAX_ENTRIES;
constexpr uint32_t PLOT_INDEX_PREFIX_CRC_OFFSET = 48;

constexpr uint8_t PLOT_SECTION_KIND_EVENTS = 0;
constexpr uint8_t PLOT_SECTION_KIND_SERIES = 1;
constexpr uint8_t PLOT_SERIES_MODE_COMPACT = 0;
constexpr uint8_t PLOT_SERIES_MODE_ENVELOPE_RUNS = 1;
constexpr int32_t PLOT_POINT_GAP_DELTA = INT32_MIN;
constexpr uint16_t PLOT_POINT_GAP_INDEX = UINT16_MAX;
constexpr uint32_t PLOT_POINT_MAX_TIME_INDEX =
    static_cast<uint32_t>(PLOT_POINT_GAP_INDEX - 1);
constexpr uint32_t PLOT_ENVELOPE_GAP_BUCKET = UINT32_MAX;
constexpr int64_t PLOT_UNKNOWN_INTERVAL_GAP_MS = 5 * 60 * 1000;

struct ReportPlotSectionDescriptor {
    uint8_t kind = 0;
    uint8_t encoding = 0;
    uint16_t flags = 0;
    char name[PLOT_SECTION_NAME_BYTES] = {};
    uint32_t offset = 0;
    uint32_t length = 0;
    uint32_t crc32 = 0;

    bool is_events() const { return kind == PLOT_SECTION_KIND_EVENTS; }
    bool is_series() const { return kind == PLOT_SECTION_KIND_SERIES; }
};

struct ReportPlotIndexView {
    uint16_t flags = 0;
    uint32_t prefix_size = 0;
    uint32_t total_size = 0;
    int64_t base_ms = 0;
    int64_t window_start_ms = 0;
    int64_t window_end_ms = 0;
    uint16_t section_count = 0;
    uint32_t prefix_crc32 = 0;
    const uint8_t *prefix = nullptr;

    bool section(size_t index, ReportPlotSectionDescriptor &out) const;
    bool find_events(ReportPlotSectionDescriptor &out) const;
    bool find_series(const char *name,
                     ReportPlotSectionDescriptor &out) const;
};

bool report_plot_encode_prefix(
    uint8_t *prefix,
    size_t prefix_size,
    int64_t base_ms,
    int64_t window_start_ms,
    int64_t window_end_ms,
    uint32_t total_size,
    const ReportPlotSectionDescriptor *sections,
    size_t section_count);
bool report_plot_decode_prefix(const uint8_t *data,
                               size_t length,
                               ReportPlotIndexView &view);
uint32_t report_plot_prefix_crc32(const uint8_t *data, size_t length);

}  // namespace aircannect
