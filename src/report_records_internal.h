#pragma once

#include <stddef.h>
#include <stdint.h>

#include "little_endian.h"
#include "report_spool_types.h"
#include "report_records.h"

namespace aircannect {
namespace report_records_detail {

static constexpr size_t SERIES_V2_HEADER_SIZE = ReportSeriesV2UniformLayout::HeaderBytes;
static constexpr size_t EVENT_RECORD_WIRE_SIZE = 16;
static constexpr uint32_t SERIES_V2_MAGIC = 0x32535241u;  // "ARS2"
static constexpr uint32_t SERIES_V2_MODE_UNIFORM = 1;
static constexpr uint32_t SERIES_V2_VALUE_INT32_MILLI = 1;

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

inline bool valid_timestamp(int64_t timestamp_ms) {
    return timestamp_ms > 0;
}

size_t bitmap_bytes_for_count(uint32_t sample_count);
bool bitmap_missing(const uint8_t *bitmap,
                    size_t bitmap_bytes,
                    uint32_t index);
bool valid_missing_bitmap(uint32_t sample_count, size_t bitmap_bytes);
bool series_v2_size(uint32_t sample_count,
                    size_t missing_bitmap_bytes,
                    size_t &out);
bool put_series_v2_header(ReportSpoolBuffer &out,
                          uint32_t interval_ms,
                          uint32_t sample_count,
                          size_t missing_bitmap_bytes);

}  // namespace report_records_detail
}  // namespace aircannect
