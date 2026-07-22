#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "night_catalog.h"

namespace aircannect {

struct NightCatalogFileInfo {
    uint32_t record_count = 0;
    uint32_t session_count = 0;
    uint32_t mask_window_count = 0;
    uint32_t file_count = 0;
    uint32_t coverage_count = 0;
    uint32_t signal_layout_count = 0;
    uint32_t fallback_file_count = 0;
    uint32_t fallback_section_count = 0;
    uint32_t path_bytes = 0;
    size_t body_bytes = 0;
    size_t total_bytes = 0;
};

class NightCatalogFileCodec {
public:
    static constexpr uint16_t Version = 10;
    static constexpr size_t HeaderBytes = 88;
    static constexpr size_t MaximumFileBytes = 512 * 1024;

    static bool inspect(const uint8_t *header,
                        size_t header_length,
                        NightCatalogFileInfo &info);
    static std::shared_ptr<const LargeByteBuffer> encode(
        const NightCatalog &catalog);
    static std::shared_ptr<const NightCatalog> decode(
        const uint8_t *header,
        size_t header_length,
        const uint8_t *body,
        size_t body_length);
};

}  // namespace aircannect
