#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "night_catalog.h"

namespace aircannect {

struct NightCatalogFileInfo {
    uint16_t version = 0;
    uint16_t record_bytes = 0;
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
    static constexpr uint16_t LegacyVersion = 11;
    static constexpr uint16_t Version = 17;
    static constexpr size_t HeaderBytes = 88;

    static bool sources_path(SleepDayId sleep_day,
                             char *out,
                             size_t out_size);

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

// Incremental input boundary for the existing table-ordered wire format.
// Owns the unpublished result; no input pointer survives append().
class NightCatalogFileDecoder {
public:
    NightCatalogFileDecoder() = default;
    ~NightCatalogFileDecoder();
    NightCatalogFileDecoder(const NightCatalogFileDecoder &) = delete;
    NightCatalogFileDecoder &operator=(const NightCatalogFileDecoder &) = delete;

    bool begin(const uint8_t *header, size_t header_length);
    bool append(const uint8_t *bytes, size_t length);
    std::shared_ptr<const NightCatalog> take_result();
    void reset();

private:
    struct Runtime;
    Runtime *runtime_ = nullptr;
};

class NightCatalogFileIndexReader {
public:
    bool start(std::shared_ptr<const NightCatalog> compact_index);
    size_t size() const { return total_bytes_; }
    size_t read(uint8_t *buffer, size_t length);
    void reset();

private:
    enum class Section : uint8_t {
        Records,
        Sessions,
        Masks,
        Done,
    };

    bool prepare_next();
    bool prepare_next_range();
    void begin_range_section(bool masks);

    // Immutable input and output length.
    std::shared_ptr<const NightCatalog> catalog_;
    size_t total_bytes_ = 0;
    size_t emitted_bytes_ = 0;
    Section section_ = Section::Done;

    // Current record and range-table cursors.
    size_t record_cursor_ = 0;
    uint32_t next_session_ = 0;
    uint32_t next_mask_ = 0;
    size_t range_record_cursor_ = 0;
    const NightCatalogRecord *range_record_ = nullptr;
    const NightCatalogTimeRange *ranges_ = nullptr;
    size_t range_count_ = 0;
    size_t range_cursor_ = 0;
    bool range_masks_ = false;

    // One wire record at most; partial reads resume within this scratch.
    static constexpr size_t ScratchBytes = 512;
    uint8_t scratch_[ScratchBytes] = {};
    size_t scratch_size_ = 0;
    size_t scratch_cursor_ = 0;
};

}  // namespace aircannect
