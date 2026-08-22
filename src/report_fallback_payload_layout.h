#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

template <typename Section>
bool report_fallback_section_valid(const Section &section,
                                   int64_t day_start_ms,
                                   int64_t day_end_ms,
                                   uint64_t metadata_bytes,
                                   uint64_t file_size) {
    if (!section.coverage.valid() ||
        section.coverage.start_ms < day_start_ms ||
        section.coverage.end_ms > day_end_ms ||
        section.data_offset < metadata_bytes ||
        section.data_offset > file_size ||
        section.data_size > file_size - section.data_offset) {
        return false;
    }

    if (section.kind == ReportFallbackSectionKind::Series) {
        const ReportSourceDef *source = report_source_def(section.source);
        const ReportSignalDef *signal = report_signal_def(section.signal);
        return source && signal && report_source_is_sampled(*source) &&
               (section.source == signal->preferred_source ||
                section.source == signal->fallback_source) &&
               report_signal_bit(section.signal) != 0 &&
               section.event_mask == 0 && section.record_count > 0 &&
               section.sample_interval_ms > 0 && section.data_size > 0 &&
               section.payload_schema ==
                   REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    }
    if (section.kind == ReportFallbackSectionKind::Events) {
        const size_t record_bytes = report_event_record_wire_size();
        if (section.record_count > SIZE_MAX / record_bytes) return false;

        return section.source == ReportSourceId::RespiratoryEvents &&
               section.signal == ReportSignalId::Invalid &&
               section.event_mask != 0 &&
               section.sample_interval_ms == 0 &&
               (section.event_mask & ~REPORT_EVENT_ALL) == 0 &&
               section.payload_schema ==
                   REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1 &&
               static_cast<size_t>(section.record_count) * record_bytes ==
                   section.data_size;
    }
    if (section.kind == ReportFallbackSectionKind::Unavailable) {
        const ReportSourceDef *source = report_source_def(section.source);
        const ReportSignalDef *signal = report_signal_def(section.signal);
        return source && signal && report_source_is_sampled(*source) &&
               (section.source == signal->preferred_source ||
                section.source == signal->fallback_source) &&
               report_signal_bit(section.signal) != 0 &&
               section.event_mask == 0 && section.payload_schema == 0 &&
               section.record_count == 0 &&
               section.sample_interval_ms == 0 && section.data_size == 0;
    }
    return false;
}

template <typename Section>
bool report_fallback_payload_layout_valid(uint64_t metadata_bytes,
                                          uint64_t file_size,
                                          const Section *sections,
                                          size_t section_count) {
    if (metadata_bytes > file_size ||
        (section_count > 0 && !sections)) {
        return false;
    }

    uint64_t payload_bytes = 0;
    for (size_t i = 0; i < section_count; ++i) {
        const Section &section = sections[i];
        if (section.data_offset < metadata_bytes ||
            section.data_offset > file_size ||
            section.data_size > file_size - section.data_offset ||
            payload_bytes > UINT64_MAX - section.data_size) {
            return false;
        }
        payload_bytes += section.data_size;

        if (section.data_size == 0) continue;
        const uint64_t section_end =
            section.data_offset + section.data_size;
        for (size_t previous_index = 0;
             previous_index < i;
             ++previous_index) {
            const Section &previous = sections[previous_index];
            if (previous.data_size == 0) continue;

            const uint64_t previous_end =
                previous.data_offset + previous.data_size;
            if (section.data_offset < previous_end &&
                previous.data_offset < section_end) {
                return false;
            }
        }
    }

    return payload_bytes == file_size - metadata_bytes;
}

}  // namespace aircannect
