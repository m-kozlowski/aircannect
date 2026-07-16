#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AC_REPORT_SUMMARY_RECORD_MAX = 256;
static constexpr size_t AC_REPORT_NIGHT_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_CACHE_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_RESULT_CHUNK_MAX = 512;
static constexpr size_t AC_REPORT_RESULT_STREAM_MAX = 16;
static constexpr size_t AC_REPORT_RESULT_SLOT_MAX = 4;
static constexpr size_t AC_REPORT_RANGE_CACHE_SLOT_MAX = 8;
static constexpr size_t AC_REPORT_RANGE_CACHE_MAX_BYTES = 4 * 1024 * 1024;
static constexpr size_t AC_REPORT_RESULT_CACHE_LOAD_QUEUE_MAX = 4;
static constexpr size_t AC_REPORT_RESULT_CACHE_PROBE_MAX = 8;
static constexpr size_t AC_REPORT_BUILD_QUEUE_MAX = 4;
static constexpr uint8_t AC_REPORT_BUILD_RETRY_MAX = 6;
static constexpr size_t AC_REPORT_RESULT_ETAG_MAX = 80;

static constexpr size_t PREFETCH_SKIP_MAX = 8;
static constexpr size_t AC_REPORT_COALESCE_SLOTS = 64;
static constexpr size_t AC_REPORT_COALESCE_TARGET_BYTES = 64 * 1024;
static constexpr size_t AC_REPORT_CACHE_WRITE_QUEUE_MAX = 8;
static constexpr size_t AC_REPORT_CACHE_WRITE_BACKPRESSURE_WATERMARK =
    AC_REPORT_CACHE_WRITE_QUEUE_MAX / 2;

}  // namespace aircannect
