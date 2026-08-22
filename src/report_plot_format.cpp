#include "report_plot_format.h"

#include <string.h>

#include "crc32.h"

namespace aircannect {
namespace {

constexpr size_t HEADER_SECTION_COUNT_OFFSET = 40;
constexpr size_t ENTRY_KIND_OFFSET = 0;
constexpr size_t ENTRY_ENCODING_OFFSET = 1;
constexpr size_t ENTRY_FLAGS_OFFSET = 2;
constexpr size_t ENTRY_NAME_OFFSET = 4;
constexpr size_t ENTRY_OFFSET_OFFSET = 36;
constexpr size_t ENTRY_LENGTH_OFFSET = 40;
constexpr size_t ENTRY_CRC_OFFSET = 44;

void put_u16(uint8_t *data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value);
    data[1] = static_cast<uint8_t>(value >> 8);
    data[2] = static_cast<uint8_t>(value >> 16);
    data[3] = static_cast<uint8_t>(value >> 24);
}

void put_i64(uint8_t *data, int64_t value) {
    const uint64_t encoded = static_cast<uint64_t>(value);
    put_u32(data, static_cast<uint32_t>(encoded));
    put_u32(data + 4, static_cast<uint32_t>(encoded >> 32));
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

int64_t get_i64(const uint8_t *data) {
    return static_cast<int64_t>(static_cast<uint64_t>(get_u32(data)) |
                                static_cast<uint64_t>(get_u32(data + 4)) << 32);
}

uint32_t prefix_crc(const uint8_t *data) {
    const uint8_t zero[sizeof(uint32_t)] = {};
    uint32_t state = crc32_ieee_initial_state();
    state = crc32_ieee_update_state(
        state, data, PLOT_INDEX_PREFIX_CRC_OFFSET);
    state = crc32_ieee_update_state(state, zero, sizeof(zero));
    state = crc32_ieee_update_state(
        state,
        data + PLOT_INDEX_PREFIX_CRC_OFFSET + sizeof(uint32_t),
        PLOT_INDEX_PREFIX_BYTES -
            PLOT_INDEX_PREFIX_CRC_OFFSET - sizeof(uint32_t));
    return crc32_ieee_finish_state(state);
}

bool valid_name(const char *name) {
    if (!name || !name[0]) return false;

    for (size_t i = 0; i < PLOT_SECTION_NAME_BYTES; ++i) {
        const char ch = name[i];
        if (ch == '\0') return true;
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            return false;
        }
    }
    return false;
}

bool descriptor_valid(const ReportPlotSectionDescriptor &section,
                      uint32_t total_size) {
    if (!valid_name(section.name) || section.length == 0 ||
        section.offset < PLOT_INDEX_PREFIX_BYTES ||
        section.offset > total_size ||
        section.length > total_size - section.offset) {
        return false;
    }

    if (section.is_events()) {
        return strcmp(section.name, "events") == 0 &&
               section.encoding == 0;
    }
    return section.is_series() &&
           (section.encoding == PLOT_SERIES_MODE_COMPACT ||
            section.encoding == PLOT_SERIES_MODE_ENVELOPE_RUNS);
}

bool descriptors_are_valid(const ReportPlotSectionDescriptor *sections,
                           size_t count,
                           uint32_t total_size) {
    if (!sections || count == 0 || count > PLOT_INDEX_MAX_ENTRIES ||
        !sections[0].is_events()) {
        return false;
    }

    uint32_t previous_end = PLOT_INDEX_PREFIX_BYTES;
    for (size_t i = 0; i < count; ++i) {
        const ReportPlotSectionDescriptor &section = sections[i];
        if (!descriptor_valid(section, total_size) ||
            section.offset != previous_end ||
            (i > 0 && !section.is_series())) {
            return false;
        }
        previous_end = section.offset + section.length;

        for (size_t previous = 0; previous < i; ++previous) {
            if (strcmp(section.name, sections[previous].name) == 0) {
                return false;
            }
        }
    }
    return previous_end == total_size;
}

void decode_entry(const uint8_t *entry,
                  ReportPlotSectionDescriptor &out) {
    out = {};
    out.kind = entry[ENTRY_KIND_OFFSET];
    out.encoding = entry[ENTRY_ENCODING_OFFSET];
    out.flags = get_u16(entry + ENTRY_FLAGS_OFFSET);
    memcpy(out.name, entry + ENTRY_NAME_OFFSET, sizeof(out.name));
    out.offset = get_u32(entry + ENTRY_OFFSET_OFFSET);
    out.length = get_u32(entry + ENTRY_LENGTH_OFFSET);
    out.crc32 = get_u32(entry + ENTRY_CRC_OFFSET);
}

}  // namespace

bool ReportPlotIndexView::section(
    size_t index,
    ReportPlotSectionDescriptor &out) const {
    if (!prefix || index >= section_count ||
        index >= PLOT_INDEX_MAX_ENTRIES ||
        prefix_size != PLOT_INDEX_PREFIX_BYTES) {
        return false;
    }

    decode_entry(prefix + PLOT_INDEX_HEADER_BYTES +
                     index * PLOT_INDEX_ENTRY_BYTES,
                 out);
    return true;
}

bool ReportPlotIndexView::find_events(
    ReportPlotSectionDescriptor &out) const {
    return section(0, out) && out.is_events();
}

bool ReportPlotIndexView::find_series(
    const char *name,
    ReportPlotSectionDescriptor &out) const {
    if (!valid_name(name)) return false;

    for (size_t i = 1; i < section_count; ++i) {
        if (!section(i, out)) return false;
        if (strcmp(out.name, name) == 0) return true;
    }
    return false;
}

uint32_t report_plot_prefix_crc32(const uint8_t *data, size_t length) {
    if (!data || length < PLOT_INDEX_PREFIX_BYTES) return 0;
    return prefix_crc(data);
}

bool report_plot_encode_prefix(
    uint8_t *prefix,
    size_t prefix_size,
    int64_t base_ms,
    int64_t window_start_ms,
    int64_t window_end_ms,
    uint32_t total_size,
    const ReportPlotSectionDescriptor *sections,
    size_t section_count) {
    if (!prefix || prefix_size != PLOT_INDEX_PREFIX_BYTES ||
        window_end_ms <= window_start_ms || total_size < prefix_size ||
        !descriptors_are_valid(sections, section_count, total_size)) {
        return false;
    }

    memset(prefix, 0, prefix_size);
    put_u32(prefix, PLOT_BIN_MAGIC);
    put_u16(prefix + 4, PLOT_BIN_VERSION);
    put_u16(prefix + 6, 0);
    put_u32(prefix + 8, PLOT_INDEX_PREFIX_BYTES);
    put_u32(prefix + 12, total_size);
    put_i64(prefix + 16, base_ms);
    put_i64(prefix + 24, window_start_ms);
    put_i64(prefix + 32, window_end_ms);
    put_u16(prefix + HEADER_SECTION_COUNT_OFFSET,
            static_cast<uint16_t>(section_count));

    for (size_t i = 0; i < section_count; ++i) {
        const ReportPlotSectionDescriptor &section = sections[i];
        uint8_t *entry = prefix + PLOT_INDEX_HEADER_BYTES +
                         i * PLOT_INDEX_ENTRY_BYTES;
        entry[ENTRY_KIND_OFFSET] = section.kind;
        entry[ENTRY_ENCODING_OFFSET] = section.encoding;
        put_u16(entry + ENTRY_FLAGS_OFFSET, section.flags);
        memcpy(entry + ENTRY_NAME_OFFSET,
               section.name,
               sizeof(section.name));
        put_u32(entry + ENTRY_OFFSET_OFFSET, section.offset);
        put_u32(entry + ENTRY_LENGTH_OFFSET, section.length);
        put_u32(entry + ENTRY_CRC_OFFSET, section.crc32);
    }

    put_u32(prefix + PLOT_INDEX_PREFIX_CRC_OFFSET, prefix_crc(prefix));
    return true;
}

bool report_plot_decode_prefix(const uint8_t *data,
                               size_t length,
                               ReportPlotIndexView &view) {
    view = {};
    if (!data || length < PLOT_INDEX_PREFIX_BYTES ||
        get_u32(data) != PLOT_BIN_MAGIC ||
        get_u16(data + 4) != PLOT_BIN_VERSION ||
        get_u32(data + 8) != PLOT_INDEX_PREFIX_BYTES) {
        return false;
    }

    const uint32_t total_size = get_u32(data + 12);
    const uint16_t section_count = get_u16(data + HEADER_SECTION_COUNT_OFFSET);
    if (total_size < PLOT_INDEX_PREFIX_BYTES ||
        (length != PLOT_INDEX_PREFIX_BYTES && length != total_size) ||
        section_count == 0 || section_count > PLOT_INDEX_MAX_ENTRIES ||
        get_u32(data + PLOT_INDEX_PREFIX_CRC_OFFSET) != prefix_crc(data)) {
        return false;
    }

    ReportPlotSectionDescriptor sections[PLOT_INDEX_MAX_ENTRIES];
    for (size_t i = 0; i < section_count; ++i) {
        decode_entry(data + PLOT_INDEX_HEADER_BYTES +
                         i * PLOT_INDEX_ENTRY_BYTES,
                     sections[i]);
    }
    if (get_i64(data + 32) <= get_i64(data + 24) ||
        !descriptors_are_valid(sections, section_count, total_size)) {
        return false;
    }

    if (length == total_size) {
        for (size_t i = 0; i < section_count; ++i) {
            if (crc32_ieee(data + sections[i].offset,
                           sections[i].length) != sections[i].crc32) {
                return false;
            }
        }
    }

    view.flags = get_u16(data + 6);
    view.prefix_size = get_u32(data + 8);
    view.total_size = total_size;
    view.base_ms = get_i64(data + 16);
    view.window_start_ms = get_i64(data + 24);
    view.window_end_ms = get_i64(data + 32);
    view.section_count = section_count;
    view.prefix_crc32 = get_u32(data + PLOT_INDEX_PREFIX_CRC_OFFSET);
    view.prefix = data;
    return true;
}

}  // namespace aircannect
