#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

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
