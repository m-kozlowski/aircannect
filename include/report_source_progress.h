#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "large_byte_buffer.h"
#include "report_sources.h"

namespace aircannect {

enum class ReportSourceProgressStorage : uint8_t {
    Edf = 1,
    Fallback = 2,
};

struct ReportSourceProgressEntry {
    const char *path = nullptr;
    uint16_t path_length = 0;
    ReportSourceProgressStorage storage = ReportSourceProgressStorage::Edf;

    ReportSeriesDescriptor series;
    EdfSignalScale scale;
    uint32_t samples_per_record = 0;
    uint32_t byte_offset_in_record = 0;
    int64_t session_start_ms = 0;
    int64_t mapping_start_ms = 0;
    int64_t full_end_ms = 0;
    int64_t cursor_ms = 0;

    int64_t file_start_ms = 0;
    uint32_t file_header_size = 0;
    uint32_t file_record_size = 0;
    uint32_t file_record_duration_ms = 0;

    uint32_t fallback_payload_schema = 0;
    int64_t fallback_coverage_start_ms = 0;
};

class ReportSourceProgressReader {
public:
    bool open(const uint8_t *data, size_t length);

    size_t count() const { return count_; }
    bool entry(size_t index, ReportSourceProgressEntry &out) const;

private:
    const uint8_t *data_ = nullptr;
    size_t length_ = 0;
    size_t entries_offset_ = 0;
    size_t count_ = 0;
};

std::shared_ptr<const LargeByteBuffer> encode_report_source_progress(
    const ReportSourceProgressEntry *entries,
    size_t count);

}  // namespace aircannect
