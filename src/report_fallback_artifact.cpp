#include "report_fallback_artifact.h"

#include <limits>
#include <stdio.h>
#include <string.h>

#include "crc32.h"
#include "little_endian.h"
#include "storage_read_port.h"
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
    'A', 'C', 'F', 'B', 'A', 'C', 'K', '4',
};
constexpr uint8_t NO_SIGNAL = UINT8_MAX;
constexpr size_t IDENTITY_OFFSET = 56;
constexpr size_t METADATA_CRC_OFFSET = 64;
constexpr size_t HEADER_CRC_OFFSET = 68;
constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

bool add_size(size_t lhs, size_t rhs, size_t &total) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) return false;
    total = lhs + rhs;
    return true;
}

bool multiply_size(size_t count, size_t width, size_t &total) {
    if (count > std::numeric_limits<size_t>::max() / width) return false;
    total = count * width;
    return true;
}

uint64_t identity_update(uint64_t hash,
                         const uint8_t *bytes,
                         size_t length) {
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

uint64_t metadata_identity(const uint8_t *metadata,
                           size_t metadata_bytes) {
    if (!metadata || metadata_bytes <
            ReportFallbackArtifactCodec::HeaderBytes) {
        return 0;
    }

    uint64_t hash = identity_update(FNV_OFFSET, metadata, IDENTITY_OFFSET);
    hash = identity_update(
        hash,
        metadata + ReportFallbackArtifactCodec::HeaderBytes,
        metadata_bytes - ReportFallbackArtifactCodec::HeaderBytes);
    return hash == 0 ? 1 : hash;
}

bool range_valid(const NightCatalogTimeRange &range,
                 int64_t day_start_ms,
                 int64_t day_end_ms) {
    return range.valid() && range.start_ms >= day_start_ms &&
           range.end_ms <= day_end_ms;
}

bool sessions_valid(const NightCatalogTimeRange *sessions,
                    size_t session_count,
                    int64_t day_start_ms,
                    int64_t day_end_ms) {
    if (!sessions || session_count == 0 ||
        session_count > ReportFallbackArtifactCodec::MaxSessions) {
        return false;
    }

    for (size_t i = 0; i < session_count; ++i) {
        if (!range_valid(sessions[i], day_start_ms, day_end_ms) ||
            (i > 0 && sessions[i].start_ms < sessions[i - 1].end_ms)) {
            return false;
        }
    }
    return true;
}

bool valid_source(ReportSourceId source) {
    return static_cast<uint8_t>(source) <=
        static_cast<uint8_t>(ReportSourceId::Leak0p5Hz);
}

bool valid_series_source(ReportSourceId source) {
    const ReportSourceDef *definition = report_source_def(source);
    return definition && report_source_is_sampled(*definition);
}

bool valid_event_source(ReportSourceId source) {
    return source == ReportSourceId::RespiratoryEvents;
}

bool section_input_valid(const ReportFallbackSectionInput &section) {
    if (!section.coverage.valid() || !valid_source(section.source) ||
        section.payload_size > UINT32_MAX ||
        section.payload_size > AC_STORAGE_PREPARED_READ_MAX_BYTES ||
        (section.payload_size > 0 && !section.payload)) {
        return false;
    }

    if (section.kind == ReportFallbackSectionKind::Series) {
        const ReportSignalDef *signal = report_signal_def(section.signal);
        return signal && valid_series_source(section.source) &&
               (section.source == signal->preferred_source ||
                section.source == signal->fallback_source) &&
               report_signal_bit(section.signal) != 0 &&
               section.event_mask == 0 && section.record_count > 0 &&
               section.sample_interval_ms > 0 &&
               section.payload_schema ==
                   REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 &&
               section.payload_size > 0;
    }
    if (section.kind == ReportFallbackSectionKind::Events) {
        const size_t record_bytes = report_event_record_wire_size();
        if (section.record_count > SIZE_MAX / record_bytes) return false;
        const size_t expected =
            static_cast<size_t>(section.record_count) * record_bytes;
        return valid_event_source(section.source) &&
               section.signal == ReportSignalId::Count &&
               section.event_mask != 0 &&
               section.sample_interval_ms == 0 &&
               (section.event_mask & ~REPORT_EVENT_ALL) == 0 &&
               section.payload_schema ==
                   REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1 &&
               expected == section.payload_size;
    }
    if (section.kind == ReportFallbackSectionKind::Unavailable) {
        const ReportSignalDef *signal = report_signal_def(section.signal);
        return signal && valid_series_source(section.source) &&
               (section.source == signal->preferred_source ||
                section.source == signal->fallback_source) &&
               report_signal_bit(section.signal) != 0 &&
               section.event_mask == 0 && section.payload_schema == 0 &&
               section.record_count == 0 &&
               section.sample_interval_ms == 0 &&
               section.payload_size == 0;
    }
    return false;
}

bool section_valid(const ReportFallbackSection &section,
                   size_t file_bytes) {
    ReportFallbackSectionInput input;
    input.kind = section.kind;
    input.source = section.source;
    input.signal = section.signal;
    input.event_mask = section.event_mask;
    input.payload_schema = section.payload_schema;
    input.record_count = section.record_count;
    input.sample_interval_ms = section.sample_interval_ms;
    input.coverage = section.coverage;
    input.payload = section.data_size > 0
        ? reinterpret_cast<const uint8_t *>(1)
        : nullptr;
    input.payload_size = section.data_size;

    return section_input_valid(input) &&
           section.data_offset <= file_bytes &&
           section.data_size <= file_bytes - section.data_offset;
}

bool section_precedes(const ReportFallbackSectionInput &lhs,
                      const ReportFallbackSectionInput &rhs) {
    if (lhs.kind != rhs.kind) {
        return static_cast<uint8_t>(lhs.kind) <
               static_cast<uint8_t>(rhs.kind);
    }
    if (lhs.source != rhs.source) {
        return static_cast<uint8_t>(lhs.source) <
               static_cast<uint8_t>(rhs.source);
    }
    if (lhs.signal != rhs.signal) {
        return static_cast<uint8_t>(lhs.signal) <
               static_cast<uint8_t>(rhs.signal);
    }
    if (lhs.event_mask != rhs.event_mask) {
        return lhs.event_mask < rhs.event_mask;
    }
    if (lhs.coverage.start_ms != rhs.coverage.start_ms) {
        return lhs.coverage.start_ms < rhs.coverage.start_ms;
    }
    return lhs.coverage.end_ms < rhs.coverage.end_ms;
}

void encode_section(uint8_t *out,
                    const ReportFallbackSectionInput &section,
                    uint64_t data_offset,
                    uint32_t data_crc32) {
    out[0] = static_cast<uint8_t>(section.kind);
    out[1] = static_cast<uint8_t>(section.source);
    out[2] = section.kind != ReportFallbackSectionKind::Events
        ? static_cast<uint8_t>(section.signal)
        : NO_SIGNAL;
    out[3] = section.event_mask;
    put_le32(out + 4, section.payload_schema);
    put_le32(out + 8, section.record_count);
    put_le32(out + 12, section.sample_interval_ms);
    put_le64(out + 16,
             static_cast<uint64_t>(section.coverage.start_ms));
    put_le64(out + 24,
             static_cast<uint64_t>(section.coverage.end_ms));
    put_le64(out + 32, data_offset);
    put_le32(out + 40, static_cast<uint32_t>(section.payload_size));
    put_le32(out + 44, data_crc32);
}

bool decode_section(const uint8_t *in, ReportFallbackSection &section) {
    const uint8_t signal = in[2];
    section.kind = static_cast<ReportFallbackSectionKind>(in[0]);
    section.source = static_cast<ReportSourceId>(in[1]);
    section.signal = signal == NO_SIGNAL
        ? ReportSignalId::Count
        : static_cast<ReportSignalId>(signal);
    section.event_mask = in[3];
    section.payload_schema = get_le32(in + 4);
    section.record_count = get_le32(in + 8);
    section.sample_interval_ms = get_le32(in + 12);
    section.coverage.start_ms = static_cast<int64_t>(get_le64(in + 16));
    section.coverage.end_ms = static_cast<int64_t>(get_le64(in + 24));
    section.data_offset = get_le64(in + 32);
    section.data_size = get_le32(in + 40);
    section.data_crc32 = get_le32(in + 44);
    return true;
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

}  // namespace

bool ReportFallbackArtifactView::session(
    size_t index,
    NightCatalogTimeRange &out) const {
    if (!session_bytes || index >= info.session_count) return false;

    out = decode_range(session_bytes +
                       index * ReportFallbackArtifactCodec::SessionBytes);
    return range_valid(out, info.day_start_ms, info.day_end_ms);
}

bool ReportFallbackArtifactView::section(
    size_t index,
    ReportFallbackSection &out) const {
    if (!section_bytes || index >= info.section_count) return false;

    const uint8_t *record = section_bytes + index *
        ReportFallbackArtifactCodec::SectionBytes;
    if (!decode_section(record, out) ||
        !section_valid(out, info.total_bytes)) {
        return false;
    }
    return true;
}

bool ReportFallbackArtifactCodec::inspect_header(
    const uint8_t *header,
    size_t header_length,
    ReportFallbackArtifactInfo &info) {
    info = {};
    if (!header || header_length < HeaderBytes ||
        memcmp(header, FILE_MAGIC, sizeof(FILE_MAGIC)) != 0 ||
        get_le16(header + 8) != Version ||
        get_le16(header + 10) != HeaderBytes ||
        get_le16(header + 12) != SessionBytes ||
        get_le16(header + 14) != SectionBytes ||
        crc32_ieee(header, HEADER_CRC_OFFSET) !=
            get_le32(header + HEADER_CRC_OFFSET)) {
        return false;
    }

    SleepDayId sleep_day;
    if (!SleepDayId::from_epoch_days(
            static_cast<int32_t>(get_le32(header + 16)), sleep_day)) {
        return false;
    }

    const uint32_t session_count = get_le32(header + 20);
    const uint32_t section_count = get_le32(header + 24);
    const int64_t day_start_ms =
        static_cast<int64_t>(get_le64(header + 32));
    const int64_t day_end_ms =
        static_cast<int64_t>(get_le64(header + 40));
    const uint64_t payload_bytes_u64 = get_le64(header + 48);
    const uint64_t content_identity = get_le64(header + IDENTITY_OFFSET);
    if (session_count == 0 || session_count > MaxSessions ||
        section_count == 0 || section_count > MaxSections ||
        day_start_ms <= 0 || day_end_ms <= day_start_ms ||
        payload_bytes_u64 > MaxFileBytes || content_identity == 0) {
        return false;
    }

    size_t session_bytes = 0;
    size_t section_bytes = 0;
    size_t metadata_bytes = 0;
    size_t total_bytes = 0;
    if (!multiply_size(session_count, SessionBytes, session_bytes) ||
        !multiply_size(section_count, SectionBytes, section_bytes) ||
        !add_size(HeaderBytes, session_bytes, metadata_bytes) ||
        !add_size(metadata_bytes, section_bytes, metadata_bytes) ||
        payload_bytes_u64 > std::numeric_limits<size_t>::max() ||
        !add_size(metadata_bytes,
                  static_cast<size_t>(payload_bytes_u64),
                  total_bytes) ||
        total_bytes > MaxFileBytes) {
        return false;
    }

    info.sleep_day = sleep_day;
    info.day_start_ms = day_start_ms;
    info.day_end_ms = day_end_ms;
    info.content_identity = content_identity;
    info.session_count = session_count;
    info.section_count = section_count;
    info.metadata_bytes = metadata_bytes;
    info.payload_bytes = static_cast<size_t>(payload_bytes_u64);
    info.total_bytes = total_bytes;
    return true;
}

bool ReportFallbackArtifactCodec::decode_metadata(
    const uint8_t *metadata,
    size_t metadata_length,
    ReportFallbackArtifactView &view) {
    view = {};
    ReportFallbackArtifactInfo info;
    if (!inspect_header(metadata, metadata_length, info) ||
        metadata_length < info.metadata_bytes) {
        return false;
    }

    const uint8_t *session_bytes = metadata + HeaderBytes;
    const size_t sessions_size =
        static_cast<size_t>(info.session_count) * SessionBytes;
    const uint8_t *section_bytes = session_bytes + sessions_size;
    const size_t metadata_body_bytes = info.metadata_bytes - HeaderBytes;
    if (crc32_ieee(session_bytes, metadata_body_bytes) !=
            get_le32(metadata + METADATA_CRC_OFFSET) ||
        metadata_identity(metadata, info.metadata_bytes) !=
            info.content_identity) {
        return false;
    }

    NightCatalogTimeRange previous_session;
    for (size_t i = 0; i < info.session_count; ++i) {
        const NightCatalogTimeRange session =
            decode_range(session_bytes + i * SessionBytes);
        if (!range_valid(session, info.day_start_ms, info.day_end_ms) ||
            (i > 0 && session.start_ms < previous_session.end_ms)) {
            return false;
        }
        previous_session = session;
    }

    uint64_t expected_offset = info.metadata_bytes;
    ReportFallbackSection previous;
    bool have_previous = false;
    for (size_t i = 0; i < info.section_count; ++i) {
        ReportFallbackSection section;
        if (!decode_section(section_bytes + i * SectionBytes, section) ||
            !section_valid(section, info.total_bytes) ||
            section.coverage.start_ms < info.day_start_ms ||
            section.coverage.end_ms > info.day_end_ms ||
            section.data_offset != expected_offset) {
            return false;
        }
        if (have_previous) {
            ReportFallbackSectionInput lhs;
            lhs.kind = previous.kind;
            lhs.source = previous.source;
            lhs.signal = previous.signal;
            lhs.event_mask = previous.event_mask;
            lhs.coverage = previous.coverage;
            ReportFallbackSectionInput rhs;
            rhs.kind = section.kind;
            rhs.source = section.source;
            rhs.signal = section.signal;
            rhs.event_mask = section.event_mask;
            rhs.coverage = section.coverage;
            if (!section_precedes(lhs, rhs)) return false;
        }

        expected_offset += section.data_size;
        previous = section;
        have_previous = true;
    }
    if (expected_offset != info.total_bytes) return false;

    view.info = info;
    view.session_bytes = session_bytes;
    view.section_bytes = section_bytes;
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportFallbackArtifactCodec::encode(
    SleepDayId sleep_day,
    int64_t day_start_ms,
    int64_t day_end_ms,
    const NightCatalogTimeRange *sessions,
    size_t session_count,
    const ReportFallbackSectionInput *sections,
    size_t section_count) {
    if (!sleep_day.valid() || day_start_ms <= 0 ||
        day_end_ms <= day_start_ms ||
        !sessions_valid(sessions,
                        session_count,
                        day_start_ms,
                        day_end_ms) ||
        !sections || section_count == 0 ||
        section_count > MaxSections) {
        return {};
    }

    size_t session_bytes = 0;
    size_t table_bytes = 0;
    size_t metadata_bytes = 0;
    size_t payload_bytes = 0;
    if (!multiply_size(session_count, SessionBytes, session_bytes) ||
        !multiply_size(section_count, SectionBytes, table_bytes) ||
        !add_size(HeaderBytes, session_bytes, metadata_bytes) ||
        !add_size(metadata_bytes, table_bytes, metadata_bytes)) {
        return {};
    }

    for (size_t i = 0; i < section_count; ++i) {
        if (!section_input_valid(sections[i]) ||
            sections[i].coverage.start_ms < day_start_ms ||
            sections[i].coverage.end_ms > day_end_ms ||
            (i > 0 && !section_precedes(sections[i - 1], sections[i])) ||
            !add_size(payload_bytes,
                      sections[i].payload_size,
                      payload_bytes)) {
            return {};
        }
    }

    size_t total_bytes = 0;
    if (!add_size(metadata_bytes, payload_bytes, total_bytes) ||
        total_bytes > MaxFileBytes) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};
    memset(output->data(), 0, metadata_bytes);

    uint8_t *header = output->data();
    memcpy(header, FILE_MAGIC, sizeof(FILE_MAGIC));
    put_le16(header + 8, Version);
    put_le16(header + 10, HeaderBytes);
    put_le16(header + 12, SessionBytes);
    put_le16(header + 14, SectionBytes);
    put_le32(header + 16,
             static_cast<uint32_t>(sleep_day.epoch_days()));
    put_le32(header + 20, static_cast<uint32_t>(session_count));
    put_le32(header + 24, static_cast<uint32_t>(section_count));
    put_le64(header + 32, static_cast<uint64_t>(day_start_ms));
    put_le64(header + 40, static_cast<uint64_t>(day_end_ms));
    put_le64(header + 48, payload_bytes);

    uint8_t *session_table = header + HeaderBytes;
    for (size_t i = 0; i < session_count; ++i) {
        encode_range(session_table + i * SessionBytes, sessions[i]);
    }

    size_t payload_offset = metadata_bytes;
    for (size_t i = 0; i < section_count; ++i) {
        const ReportFallbackSectionInput &section = sections[i];
        uint8_t *section_record = session_table + session_bytes +
            i * SectionBytes;
        const uint32_t payload_crc =
            crc32_ieee(section.payload, section.payload_size);
        encode_section(section_record,
                       section,
                       payload_offset,
                       payload_crc);
        if (section.payload_size > 0) {
            memcpy(header + payload_offset,
                   section.payload,
                   section.payload_size);
        }
        payload_offset += section.payload_size;
    }

    put_le32(header + METADATA_CRC_OFFSET,
             crc32_ieee(header + HeaderBytes,
                        metadata_bytes - HeaderBytes));
    put_le64(header + IDENTITY_OFFSET,
             metadata_identity(header, metadata_bytes));
    put_le32(header + HEADER_CRC_OFFSET,
             crc32_ieee(header, HEADER_CRC_OFFSET));
    return LargeByteBuffer::freeze(std::move(output));
}

bool report_fallback_artifact_path(SleepDayId sleep_day,
                                   char *out,
                                   size_t out_size) {
    if (!sleep_day.valid() || !out || out_size == 0) return false;

    char day[9] = {};
    if (!sleep_day.format_yyyymmdd(day, sizeof(day))) return false;
    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s.bin",
                                 REPORT_FALLBACK_ARTIFACT_ROOT,
                                 day);
    return written > 0 && static_cast<size_t>(written) < out_size &&
           storage_user_path_valid(out);
}

}  // namespace aircannect
