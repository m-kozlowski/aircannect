#include "report_manager.h"

#include <algorithm>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "background_worker.h"
#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "edf_report_data_plan.h"
#include "edf_report_data_reader.h"
#include "edf_report_provider.h"
#include "edf_report_provider_token.h"
#include "json_util.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_materializer.h"
#include "report_night_index.h"
#include "report_night_index_store.h"
#include "report_records.h"
#include "report_source_resolver.h"
#include "report_store.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

const char *const REPORT_SUMMARY_FROM = "2000-01-01T00:00:00.000Z";
constexpr const char *REPORT_CACHE_BASE_DIR = "/aircannect/report/v4";
constexpr const char *REPORT_PLOT_CACHE_DIR =
    "/aircannect/report/v4/plots/v2";
constexpr const char *REPORT_RESULT_JSON_CACHE_DIR =
    "/aircannect/report/v4/results/v1";
constexpr size_t REPORT_CACHE_PATH_MAX = 192;
constexpr size_t REPORT_CACHE_WRITE_CHUNK = 4096;
constexpr size_t REPORT_RESULT_JSON_CACHE_MAX = 32 * 1024;
constexpr uint32_t REPORT_RESULT_ETAG_VERSION = 23;

const char *report_provider_id_name(ReportProviderId provider) {
    switch (provider) {
        case ReportProviderId::Edf: return "edf";
        case ReportProviderId::Spool: return "spool";
        default: return "unknown";
    }
}

struct ChunkWriteContext {
    ReportManager *manager = nullptr;
    ReportSourceId source = ReportSourceId::Summary;
    char *error = nullptr;
    size_t error_len = 0;
};

struct ResultChunkContext {
    ReportManager *manager = nullptr;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    bool required = false;
    size_t stream_index = SIZE_MAX;
    uint32_t entries = 0;
};

struct RangeChunkContext {
    ReportManager *manager = nullptr;
    size_t stream_index = SIZE_MAX;
    const char *name = nullptr;
};

struct SummaryRecordBufferContext {
    ReportSummaryRecord *records = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    uint32_t nights_with_therapy = 0;
};

const SpoolReportProvider &spool_report_provider() {
    static SpoolReportProvider provider;
    return provider;
}

const EdfReportProvider &edf_report_provider() {
    static EdfReportProvider provider;
    return provider;
}

bool parse_report_night_start_from_etag(const char *etag,
                                        uint64_t &night_start_ms) {
    if (!etag || !etag[0]) return false;
    char *end = nullptr;
    const unsigned long long parsed = strtoull(etag, &end, 10);
    if (end == etag || !end || *end != '-') return false;
    night_start_ms = static_cast<uint64_t>(parsed);
    return night_start_ms != 0;
}

const char *result_prepare_outcome_name(uint8_t outcome) {
    switch (outcome) {
        case 0:
            return "prepared";
        case 1:
            return "deferred";
        case 2:
            return "retry";
        case 3:
            return "failed";
    }
    return "unknown";
}

const char *build_queue_result_name(uint8_t result) {
    switch (result) {
        case 0:
            return "queued";
        case 1:
            return "already";
        case 2:
            return "full";
        case 3:
            return "unavailable";
    }
    return "unknown";
}

bool edf_session_same_identity(const EdfReportSessionDescriptor &a,
                               const EdfReportSessionDescriptor &b) {
    return strcmp(a.sleep_day, b.sleep_day) == 0 &&
           strcmp(a.session_stamp, b.session_stamp) == 0;
}

bool report_stream_bit(size_t stream_index, uint32_t &bit) {
    if (stream_index >= 32) return false;
    bit = 1u << static_cast<uint32_t>(stream_index);
    return true;
}

void append_u64(LargeTextBuffer &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out += buf;
}

void append_long(LargeTextBuffer &out, long value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", value);
    out += buf;
}

void format_stable_night_key(const ReportSummaryRecord &rec,
                             char *out,
                             size_t out_size) {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%llu-%lu-%lu",
             static_cast<unsigned long long>(rec.start_ms),
             static_cast<unsigned long>(rec.duration_min),
             static_cast<unsigned long>(rec.session_interval_count));
}

// Plot binary wire format (little-endian), served at /api/report/plot:
//   header: magic u32 'ACPB', version u16, flags u16, base_ms i64
//   events: count u32, then count * { t_delta i32, duration i32, code i32, flags i32 }
//   series: to EOF, repeated { name_len u16, name bytes,
//           mode u8, flags u8, reserved u16, mode-specific payload }
// Compact-point payload:
//           series_base_delta_ms i32, time_unit_ms u32, value_scale_milli u32,
//           point_count u32, point_count * { time_index u16, value_i16 }
// Envelope-run payload:
//           axis_base_delta_ms i32, bucket_ms u32, value_scale_milli u32,
//           run_count u32, run_count * {
//             start_bucket u32, bucket_count u16,
//             bucket_count * { min_value_i16, max_value_i16 }
//           }
// Compact time_index 0xFFFF is an explicit segment break. Envelope gaps are
// represented by separate runs. Values are value_i16 * scale / 1000.
constexpr uint32_t PLOT_BIN_MAGIC = 0x42504341u;  // "ACPB"
constexpr uint16_t PLOT_BIN_VERSION = 5;
constexpr uint8_t PLOT_SERIES_MODE_COMPACT = 0;
constexpr uint8_t PLOT_SERIES_MODE_ENVELOPE_RUNS = 1;
constexpr int32_t PLOT_POINT_GAP_DELTA = INT32_MIN;
constexpr uint16_t PLOT_POINT_GAP_INDEX = UINT16_MAX;
constexpr uint32_t PLOT_POINT_MAX_TIME_INDEX =
    static_cast<uint32_t>(PLOT_POINT_GAP_INDEX - 1);
constexpr uint32_t PLOT_ENVELOPE_GAP_BUCKET = UINT32_MAX;
constexpr int64_t PLOT_UNKNOWN_INTERVAL_GAP_MS = 5 * 60 * 1000;

bool bin_put_u16(ReportSpoolBuffer &b, uint16_t v) {
    const uint8_t x[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return b.append(x, sizeof(x));
}
bool bin_put_u8(ReportSpoolBuffer &b, uint8_t v) {
    return b.append(&v, sizeof(v));
}
bool bin_put_u32(ReportSpoolBuffer &b, uint32_t v) {
    const uint8_t x[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    return b.append(x, sizeof(x));
}
bool bin_put_i32(ReportSpoolBuffer &b, int32_t v) {
    return bin_put_u32(b, static_cast<uint32_t>(v));
}
bool bin_put_i16(ReportSpoolBuffer &b, int16_t v) {
    return bin_put_u16(b, static_cast<uint16_t>(v));
}
bool bin_put_i64(ReportSpoolBuffer &b, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    return bin_put_u32(b, static_cast<uint32_t>(u)) &&
           bin_put_u32(b, static_cast<uint32_t>(u >> 32));
}

uint16_t read_u16_le(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) |
           (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t read_u32_le(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t read_i32_le(const uint8_t *p) {
    return static_cast<int32_t>(read_u32_le(p));
}

uint32_t ceil_div_u64(uint64_t n, uint64_t d) {
    return d ? static_cast<uint32_t>((n + d - 1) / d) : 0;
}

int16_t quantize_plot_value(int32_t value_milli,
                            uint32_t scale_milli) {
    if (scale_milli == 0) scale_milli = 1;
    int64_t value = value_milli;
    int64_t encoded = 0;
    if (value >= 0) {
        encoded = (value + static_cast<int64_t>(scale_milli / 2)) /
                  static_cast<int64_t>(scale_milli);
    } else {
        encoded = -((-value + static_cast<int64_t>(scale_milli / 2)) /
                    static_cast<int64_t>(scale_milli));
    }
    if (encoded > INT16_MAX) encoded = INT16_MAX;
    if (encoded < INT16_MIN) encoded = INT16_MIN;
    return static_cast<int16_t>(encoded);
}

bool append_plot_series_compact(ReportSpoolBuffer &out,
                                const char *name,
                                const ReportSpoolBuffer &raw_points,
                                bool &ok) {
    if (!ok) return false;
    if (raw_points.size() == 0) return true;
    if ((raw_points.size() % 8) != 0) {
        ok = false;
        return false;
    }
    const size_t point_count = raw_points.size() / 8;
    if (point_count > UINT32_MAX) {
        ok = false;
        return false;
    }
    const size_t name_len = name ? strlen(name) : 0;
    if (name_len > UINT16_MAX) {
        ok = false;
        return false;
    }

    const uint8_t *raw = raw_points.data();
    bool have_real_point = false;
    int32_t min_delta = INT32_MAX;
    int32_t max_delta = INT32_MIN;
    int64_t max_abs_value = 0;
    for (size_t i = 0; i < point_count; ++i) {
        const uint8_t *p = raw + i * 8;
        const int32_t delta = read_i32_le(p);
        if (delta == PLOT_POINT_GAP_DELTA) continue;
        const int32_t value = read_i32_le(p + 4);
        min_delta = std::min(min_delta, delta);
        max_delta = std::max(max_delta, delta);
        const int64_t abs_value =
            value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(value));
        max_abs_value = std::max(max_abs_value, abs_value);
        have_real_point = true;
    }
    if (!have_real_point) return true;

    const uint64_t span = max_delta > min_delta
                              ? static_cast<uint64_t>(
                                    static_cast<int64_t>(max_delta) -
                                    static_cast<int64_t>(min_delta))
                              : 0;
    uint32_t time_unit_ms = std::max<uint32_t>(
        1, ceil_div_u64(span, PLOT_POINT_MAX_TIME_INDEX));
    if (time_unit_ms == 0) time_unit_ms = 1;
    const uint32_t value_scale_milli =
        std::max<uint32_t>(1, ceil_div_u64(static_cast<uint64_t>(max_abs_value),
                                          static_cast<uint64_t>(INT16_MAX)));

    ok &= bin_put_u16(out, static_cast<uint16_t>(name_len));
    if (name_len) {
        ok &= out.append(reinterpret_cast<const uint8_t *>(name), name_len);
    }
    ok &= bin_put_u8(out, PLOT_SERIES_MODE_COMPACT);
    ok &= bin_put_u8(out, 0);
    ok &= bin_put_u16(out, 0);
    ok &= bin_put_i32(out, min_delta);
    ok &= bin_put_u32(out, time_unit_ms);
    ok &= bin_put_u32(out, value_scale_milli);
    ok &= bin_put_u32(out, static_cast<uint32_t>(point_count));
    for (size_t i = 0; i < point_count; ++i) {
        const uint8_t *p = raw + i * 8;
        const int32_t delta = read_i32_le(p);
        const int32_t value = read_i32_le(p + 4);
        if (delta == PLOT_POINT_GAP_DELTA) {
            ok &= bin_put_u16(out, PLOT_POINT_GAP_INDEX);
            ok &= bin_put_i16(out, 0);
            continue;
        }
        const uint64_t offset =
            static_cast<uint64_t>(static_cast<int64_t>(delta) -
                                  static_cast<int64_t>(min_delta));
        uint64_t index =
            (offset + static_cast<uint64_t>(time_unit_ms / 2)) /
            static_cast<uint64_t>(time_unit_ms);
        if (index > PLOT_POINT_MAX_TIME_INDEX) index = PLOT_POINT_MAX_TIME_INDEX;
        ok &= bin_put_u16(out, static_cast<uint16_t>(index));
        ok &= bin_put_i16(out, quantize_plot_value(value, value_scale_milli));
    }
    return ok;
}

bool append_plot_series_envelope_runs(ReportSpoolBuffer &out,
                                      const char *name,
                                      const ReportSpoolBuffer &raw_buckets,
                                      int64_t bucket_ms,
                                      bool &ok) {
    if (!ok) return false;
    if (raw_buckets.size() == 0) return true;
    if (bucket_ms <= 0 || bucket_ms > UINT32_MAX ||
        (raw_buckets.size() % 12) != 0) {
        ok = false;
        return false;
    }
    const size_t record_count = raw_buckets.size() / 12;
    const size_t name_len = name ? strlen(name) : 0;
    if (name_len > UINT16_MAX) {
        ok = false;
        return false;
    }

    const uint8_t *raw = raw_buckets.data();
    bool have_real_bucket = false;
    int64_t max_abs_value = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const uint8_t *p = raw + i * 12;
        const uint32_t bucket = read_u32_le(p);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) continue;
        const int32_t min_value = read_i32_le(p + 4);
        const int32_t max_value = read_i32_le(p + 8);
        const int64_t min_abs =
            min_value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(min_value));
        const int64_t max_abs =
            max_value == INT32_MIN
                ? static_cast<int64_t>(INT32_MAX) + 1
                : llabs(static_cast<int64_t>(max_value));
        max_abs_value = std::max(max_abs_value, std::max(min_abs, max_abs));
        have_real_bucket = true;
    }
    if (!have_real_bucket) return true;

    uint32_t run_count = 0;
    bool in_run = false;
    uint32_t previous_bucket = 0;
    uint32_t buckets_in_run = 0;
    for (size_t i = 0; i < record_count; ++i) {
        const uint32_t bucket = read_u32_le(raw + i * 12);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) {
            in_run = false;
            buckets_in_run = 0;
            continue;
        }
        const bool starts_run =
            !in_run || bucket != previous_bucket + 1 ||
            buckets_in_run >= UINT16_MAX;
        if (starts_run) {
            ++run_count;
            buckets_in_run = 0;
            in_run = true;
        }
        previous_bucket = bucket;
        ++buckets_in_run;
    }
    if (run_count == 0) return true;

    const uint32_t value_scale_milli =
        std::max<uint32_t>(1, ceil_div_u64(static_cast<uint64_t>(max_abs_value),
                                          static_cast<uint64_t>(INT16_MAX)));

    ok &= bin_put_u16(out, static_cast<uint16_t>(name_len));
    if (name_len) {
        ok &= out.append(reinterpret_cast<const uint8_t *>(name), name_len);
    }
    ok &= bin_put_u8(out, PLOT_SERIES_MODE_ENVELOPE_RUNS);
    ok &= bin_put_u8(out, 0);
    ok &= bin_put_u16(out, 0);
    ok &= bin_put_i32(out, 0);
    ok &= bin_put_u32(out, static_cast<uint32_t>(bucket_ms));
    ok &= bin_put_u32(out, value_scale_milli);
    ok &= bin_put_u32(out, run_count);

    size_t i = 0;
    while (i < record_count) {
        uint32_t bucket = read_u32_le(raw + i * 12);
        if (bucket == PLOT_ENVELOPE_GAP_BUCKET) {
            ++i;
            continue;
        }
        const size_t run_start = i;
        const uint32_t start_bucket = bucket;
        uint16_t bucket_count = 1;
        ++i;
        while (i < record_count && bucket_count < UINT16_MAX) {
            const uint32_t next_bucket = read_u32_le(raw + i * 12);
            if (next_bucket == PLOT_ENVELOPE_GAP_BUCKET ||
                next_bucket != bucket + 1) {
                break;
            }
            bucket = next_bucket;
            ++bucket_count;
            ++i;
        }

        ok &= bin_put_u32(out, start_bucket);
        ok &= bin_put_u16(out, bucket_count);
        for (uint16_t n = 0; n < bucket_count; ++n) {
            const uint8_t *p = raw + (run_start + n) * 12;
            int32_t min_value = read_i32_le(p + 4);
            int32_t max_value = read_i32_le(p + 8);
            if (min_value > max_value) std::swap(min_value, max_value);
            ok &= bin_put_i16(out,
                              quantize_plot_value(min_value,
                                                  value_scale_milli));
            ok &= bin_put_i16(out,
                              quantize_plot_value(max_value,
                                                  value_scale_milli));
        }
    }
    return ok;
}

int64_t plot_gap_threshold_ms(uint32_t interval_ms) {
    if (interval_ms == 0) return PLOT_UNKNOWN_INTERVAL_GAP_MS;
    const int64_t by_interval =
        static_cast<int64_t>(interval_ms) * 3LL;
    return std::max<int64_t>(5000, by_interval);
}

uint32_t infer_chunk_interval_ms(uint32_t record_count,
                                 int64_t start_ms,
                                 int64_t end_ms) {
    if (record_count == 0 || end_ms <= start_ms) return 0;
    const int64_t duration_ms = end_ms - start_ms;
    const int64_t interval_ms =
        duration_ms / static_cast<int64_t>(record_count);
    return interval_ms > 0 && interval_ms <= UINT32_MAX
               ? static_cast<uint32_t>(interval_ms)
               : 0;
}

int64_t plot_bucket_ms_for_interval(int64_t target_bucket_ms,
                                    uint32_t interval_ms,
                                    bool preserve_slow_native_cadence) {
    if (target_bucket_ms < 1) target_bucket_ms = 1;
    if (preserve_slow_native_cadence &&
        interval_ms >= AC_REPORT_RANGE_PLOT_PRESERVE_INTERVAL_MIN_MS) {
        return std::min<int64_t>(target_bucket_ms,
                                 static_cast<int64_t>(interval_ms));
    }
    return target_bucket_ms;
}

int64_t plot_bucket_ms_for_signal(ReportSignalId signal,
                                  ReportSourceId source,
                                  int64_t target_bucket_ms,
                                  uint32_t interval_ms,
                                  bool preserve_slow_native_cadence) {
    int64_t bucket_ms = plot_bucket_ms_for_interval(
        target_bucket_ms, interval_ms, preserve_slow_native_cadence);
    if (!preserve_slow_native_cadence) {
        if (interval_ms > 0) {
            bucket_ms =
                std::max<int64_t>(bucket_ms, static_cast<int64_t>(interval_ms));
        }
        return std::max<int64_t>(1, bucket_ms);
    }
    if (interval_ms > 0) {
        int64_t cap_ms = 0;
        if ((signal == ReportSignalId::Flow &&
             source == ReportSourceId::RespiratoryFlow6p25Hz) ||
            (signal == ReportSignalId::MaskPressure &&
             source == ReportSourceId::MaskPressure6p25Hz) ||
            interval_ms <
                AC_REPORT_LOW_RATE_NATIVE_PLOT_MIN_INTERVAL_MS) {
            cap_ms = AC_REPORT_HIGH_RATE_PLOT_BUCKET_MAX_MS;
        } else if (interval_ms <=
                   AC_REPORT_LOW_RATE_NATIVE_PLOT_MAX_INTERVAL_MS) {
            cap_ms = preserve_slow_native_cadence
                         ? static_cast<int64_t>(interval_ms)
                         : AC_REPORT_LOW_RATE_OVERVIEW_BUCKET_MAX_MS;
        }
        if (cap_ms > 0) bucket_ms = std::min<int64_t>(bucket_ms, cap_ms);
        bucket_ms =
            std::max<int64_t>(bucket_ms, static_cast<int64_t>(interval_ms));
    }
    return std::max<int64_t>(1, bucket_ms);
}

int32_t plot_value_multiplier(ReportSignalId signal, ReportSourceId source) {
    if ((signal == ReportSignalId::Flow &&
         source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (signal == ReportSignalId::Leak &&
         source == ReportSourceId::Leak0p5Hz)) {
        return 60;
    }
    return 1;
}

void append_optional_float(LargeTextBuffer &json,
                           const char *key,
                           bool present,
                           float value) {
    if (present) json_add_float(json, key, value);
}

bool format_utc_ms_iso(uint64_t ms, std::string &out) {
    const time_t seconds = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;
    char buf[32];
    if (!strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv)) {
        return false;
    }
    out = buf;
    out += ".000Z";
    return true;
}

size_t collect_required_ranges_from_session_ranges(
    const ReportSessionRange *session_ranges,
    size_t session_range_count,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportRequiredRange *required_ranges,
    size_t max_ranges) {
    if (!session_ranges || !required_ranges || max_ranges == 0 ||
        range_end_ms <= range_start_ms) {
        return 0;
    }
    size_t required_range_count = 0;
    for (size_t i = 0; i < session_range_count &&
                       required_range_count < max_ranges; ++i) {
        if (!ranges_overlap(session_ranges[i].start_ms,
                            session_ranges[i].end_ms,
                            range_start_ms,
                            range_end_ms)) {
            continue;
        }
        EdfReportRequiredRange &range = required_ranges[required_range_count];
        range.start_ms = std::max(session_ranges[i].start_ms, range_start_ms);
        range.end_ms = std::min(session_ranges[i].end_ms, range_end_ms);
        if (range.end_ms > range.start_ms) required_range_count++;
    }
    return required_range_count;
}

size_t collect_required_edf_ranges(const ReportSummaryRecord &night,
                                   int64_t range_start_ms,
                                   int64_t range_end_ms,
                                   EdfReportRequiredRange *required_ranges,
                                   size_t max_ranges) {
    if (!required_ranges || max_ranges == 0 ||
        range_end_ms <= range_start_ms) {
        return 0;
    }

    ReportSessionRange session_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t session_range_count =
        collect_session_ranges(night,
                               session_ranges,
                               AC_REPORT_SUMMARY_SESSION_MAX);
    return collect_required_ranges_from_session_ranges(session_ranges,
                                                       session_range_count,
                                                       range_start_ms,
                                                       range_end_ms,
                                                       required_ranges,
                                                       max_ranges);
}

bool source_complete_for_range(const ReportSummaryRecord &night,
                               const ReportSourceDef &source,
                               int64_t from_ms) {
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;
    const int64_t coverage_start = std::max(span_start, from_ms);
    if (span_end <= coverage_start) return true;
    return spool_report_provider().coverage_complete(source,
                                                     coverage_start,
                                                     span_end);
}

// Series sources stream continuous samples; event sources are sparse (a covered
// range may legitimately contain zero events). Used to decide whether a night
// must show delivered samples before its coverage is claimed.
bool source_is_sampled(const ReportSourceDef &source) {
    return report_source_is_sampled(source);
}

bool source_is_sparse_event(const ReportSourceDef &source) {
    return report_source_is_sparse_event(source);
}

bool source_latest_cached_end_for_night(const ReportSourceDef &source,
                                        const ReportSummaryRecord &night,
                                        int64_t &out_end_ms);

bool deadline_before(uint32_t candidate, uint32_t current, uint32_t now_ms) {
    return static_cast<int32_t>(candidate - now_ms) <
           static_cast<int32_t>(current - now_ms);
}

const char *plot_cache_basename(const char *path) {
    if (!path) return "";
    const char *last = strrchr(path, '/');
    return last ? last + 1 : path;
}

bool cache_child_path(const char *dir,
                      const char *child_name,
                      char *out,
                      size_t out_size) {
    if (!child_name || !child_name[0] || !out || !out_size) {
        return false;
    }
    if (child_name[0] == '/') {
        const int written = snprintf(out, out_size, "%s", child_name);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }
    const int written = snprintf(out, out_size, "%s/%s", dir, child_name);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool plot_cache_name_for_night(const char *name, uint64_t night_start_ms) {
    const char *base = plot_cache_basename(name);
    char prefix[32];
    const int written = snprintf(prefix,
                                 sizeof(prefix),
                                 "%llu-",
                                 static_cast<unsigned long long>(
                                     night_start_ms));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(prefix)) {
        return false;
    }
    const size_t prefix_len = strlen(prefix);
    if (strncmp(base, prefix, prefix_len) != 0) return false;
    const char *dot = strrchr(base, '.');
    return dot && (strcmp(dot, ".bin") == 0 || strcmp(dot, ".tmp") == 0);
}

bool store_summary_record_to_buffer(void *context,
                                    const ReportSummaryRecord &record) {
    SummaryRecordBufferContext *ctx =
        static_cast<SummaryRecordBufferContext *>(context);
    if (!ctx || !ctx->records || ctx->count >= ctx->capacity) {
        return false;
    }
    ctx->records[ctx->count++] = record;
    if (record.duration_min > 0) ctx->nights_with_therapy++;
    return true;
}

// A built result is "ready" only when required coverage is complete; otherwise it
// is displayable best-effort but "partial" (the doc reserves complete for full
// required coverage, so a degraded night must not be presented as a valid one).
ReportResultState settled_result_state(uint32_t missing_required) {
    return missing_required == 0 ? ReportResultState::Ready
                                 : ReportResultState::Partial;
}

bool result_state_http_cacheable(ReportResultState state) {
    return state == ReportResultState::Ready ||
           state == ReportResultState::Partial;
}

bool result_state_materialized_slot_allowed(ReportResultState state) {
    return state != ReportResultState::Preparing;
}

bool source_latest_cached_end_for_night(const ReportSourceDef &source,
                                        const ReportSummaryRecord &night,
                                        int64_t &out_end_ms) {
    return spool_report_provider().latest_cached_end(
        source,
        static_cast<int64_t>(night.start_ms),
        static_cast<int64_t>(night.start_ms),
        static_cast<int64_t>(night.end_ms),
        out_end_ms);
}

bool report_ranges_overlap(int64_t a_start,
                           int64_t a_end,
                           int64_t b_start,
                           int64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

bool report_time_in_ranges(int64_t value,
                           const ReportSessionRange *ranges,
                           size_t range_count) {
    if (!ranges || value <= 0) return false;
    for (size_t i = 0; i < range_count; ++i) {
        if (value >= ranges[i].start_ms && value <= ranges[i].end_ms) {
            return true;
        }
    }
    return false;
}

bool report_event_overlaps_ranges(const ReportEventRecord &event,
                                  const ReportSessionRange *ranges,
                                  size_t range_count) {
    if (!ranges || event.start_ms <= 0 || event.duration_ms < 0) return false;
    if (event.duration_ms == 0) {
        return report_time_in_ranges(event.start_ms, ranges, range_count);
    }
    const int64_t event_end_ms =
        event.start_ms + static_cast<int64_t>(event.duration_ms);
    if (event_end_ms <= event.start_ms) return false;
    for (size_t i = 0; i < range_count; ++i) {
        if (report_ranges_overlap(event.start_ms,
                                  event_end_ms,
                                  ranges[i].start_ms,
                                  ranges[i].end_ms)) {
            return true;
        }
    }
    return false;
}

bool same_report_event(const ReportEventRecord &a,
                       const ReportEventRecord &b) {
    return a.start_ms == b.start_ms &&
           a.duration_ms == b.duration_ms &&
           a.code == b.code &&
           a.flags == b.flags;
}

bool report_event_seen(const ReportSpoolBuffer &seen,
                       const ReportEventRecord &event) {
    const size_t count = seen.size() / report_event_record_wire_size();
    for (size_t i = 0; i < count; ++i) {
        ReportEventRecord current;
        if (!report_read_event_record(seen.data(),
                                      seen.size(),
                                      i,
                                      current)) {
            continue;
        }
        if (same_report_event(current, event)) return true;
    }
    return false;
}

bool remember_report_event(ReportSpoolBuffer &seen,
                           const ReportEventRecord &event) {
    if (report_event_seen(seen, event)) return true;
    return report_append_event_record(seen, event);
}

bool indexed_night_by_newest_cursor(const ReportIndexedNight *nights,
                                    size_t count,
                                    size_t cursor,
                                    ReportIndexedNight &out,
                                    size_t &therapy_index) {
    if (!nights) return false;
    size_t seen = 0;
    for (size_t i = count; i > 0; --i) {
        const ReportIndexedNight &night = nights[i - 1];
        if (!night.summary.valid ||
            night.summary.start_ms == 0 ||
            night.summary.duration_min == 0) {
            continue;
        }
        if (seen == cursor) {
            out = night;
            therapy_index = seen;
            return true;
        }
        seen++;
    }
    return false;
}

void log_report_alloc_failed(const char *context, size_t bytes) {
    Log::logf(CAT_REPORT,
              LOG_ERROR,
              "allocation failed context=%s bytes=%u\n",
              context ? context : "--",
              static_cast<unsigned>(bytes));
}

class ScopedIndexedNight {
public:
    explicit ScopedIndexedNight(const char *context)
        : context_(context),
          night_(static_cast<ReportIndexedNight *>(Memory::alloc_large(
              sizeof(ReportIndexedNight),
              false))) {
        if (!night_) log_report_alloc_failed(context_, sizeof(ReportIndexedNight));
    }

    ~ScopedIndexedNight() {
        Memory::free(night_);
    }

    ScopedIndexedNight(const ScopedIndexedNight &) = delete;
    ScopedIndexedNight &operator=(const ScopedIndexedNight &) = delete;

    explicit operator bool() const { return night_ != nullptr; }
    ReportIndexedNight &get() { return *night_; }
    const ReportIndexedNight &get() const { return *night_; }
    ReportIndexedNight *operator->() { return night_; }
    const ReportIndexedNight *operator->() const { return night_; }

private:
    const char *context_ = nullptr;
    ReportIndexedNight *night_ = nullptr;
};

class ScopedIndexedNightList {
public:
    ScopedIndexedNightList(const char *context, size_t capacity)
        : context_(context),
          capacity_(capacity),
          nights_(static_cast<ReportIndexedNight *>(Memory::alloc_large(
              capacity * sizeof(ReportIndexedNight),
              false))) {
        if (!nights_) {
            log_report_alloc_failed(context_,
                                    capacity * sizeof(ReportIndexedNight));
        }
    }

    ~ScopedIndexedNightList() {
        Memory::free(nights_);
    }

    ScopedIndexedNightList(const ScopedIndexedNightList &) = delete;
    ScopedIndexedNightList &operator=(const ScopedIndexedNightList &) = delete;

    explicit operator bool() const { return nights_ != nullptr; }
    ReportIndexedNight *data() { return nights_; }
    const ReportIndexedNight *data() const { return nights_; }
    size_t capacity() const { return capacity_; }

private:
    const char *context_ = nullptr;
    size_t capacity_ = 0;
    ReportIndexedNight *nights_ = nullptr;
};

class ScopedReportResolveContext {
public:
    explicit ScopedReportResolveContext(const char *context,
                                        bool include_sessions = true)
        : context_(context),
          include_sessions_(include_sessions),
          sessions_(static_cast<EdfReportSessionDescriptor *>(
              include_sessions
                  ? Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                                         sizeof(EdfReportSessionDescriptor),
                                         false)
                  : nullptr)),
          plan_(static_cast<ReportResolvedPlan *>(
              Memory::calloc_large(1, sizeof(ReportResolvedPlan), false))),
          scratch_(static_cast<ReportResolveScratch *>(
              Memory::calloc_large(1, sizeof(ReportResolveScratch), false))) {
        if ((include_sessions_ && !sessions_) || !plan_ || !scratch_) {
            size_t bytes = sizeof(ReportResolvedPlan) +
                           sizeof(ReportResolveScratch);
            if (include_sessions_) {
                bytes += AC_REPORT_EDF_SESSION_MAX *
                         sizeof(EdfReportSessionDescriptor);
            }
            log_report_alloc_failed(
                context_,
                bytes);
        }
    }

    ~ScopedReportResolveContext() {
        Memory::free(scratch_);
        Memory::free(plan_);
        Memory::free(sessions_);
    }

    ScopedReportResolveContext(const ScopedReportResolveContext &) = delete;
    ScopedReportResolveContext &operator=(const ScopedReportResolveContext &) =
        delete;

    explicit operator bool() const {
        return (!include_sessions_ || sessions_) && plan_ && scratch_;
    }

    EdfReportSessionDescriptor *sessions() { return sessions_; }
    ReportResolvedPlan &plan() { return *plan_; }
    ReportResolveScratch &scratch() { return *scratch_; }

private:
    const char *context_ = nullptr;
    bool include_sessions_ = true;
    EdfReportSessionDescriptor *sessions_ = nullptr;
    ReportResolvedPlan *plan_ = nullptr;
    ReportResolveScratch *scratch_ = nullptr;
};

const char *prefetch_phase_name(ReportManager::PrefetchPhase phase) {
    switch (phase) {
        case ReportManager::PrefetchPhase::Idle: return "idle";
        case ReportManager::PrefetchPhase::Selecting: return "selecting";
        case ReportManager::PrefetchPhase::Pending: return "pending";
        case ReportManager::PrefetchPhase::Fetching: return "fetching";
        case ReportManager::PrefetchPhase::Done: return "done";
        case ReportManager::PrefetchPhase::Failed: return "failed";
        case ReportManager::PrefetchPhase::Drained: return "drained";
        default: return "?";
    }
}

}  // namespace

struct PlotBlobScan {
    bool valid = false;
    uint32_t events = 0;
    uint32_t points = 0;
};
PlotBlobScan scan_plot_blob(const ReportSpoolBuffer &b);

ReportManager::~ReportManager() {
    Memory::free(records_);
    records_ = nullptr;
    Memory::free(summary_scratch_);
    summary_scratch_ = nullptr;
    Memory::free(night_epochs_);
    night_epochs_ = nullptr;
    night_epoch_count_ = 0;
    Memory::free(cache_source_night_extent_ms_);
    cache_source_night_extent_ms_ = nullptr;
    if (cache_coalesce_) {
        discard_cache_coalesce_buffers();
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            cache_coalesce_[i].~CacheCoalesceBuffer();
        }
        Memory::free(cache_coalesce_);
        cache_coalesce_ = nullptr;
    }
    if (cache_write_queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            cache_write_queue_[i].~CacheWriteQueueSlot();
        }
        Memory::free(cache_write_queue_);
        cache_write_queue_ = nullptr;
    }
    if (result_slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            result_slots_[i].~MaterializedResult();
        }
        Memory::free(result_slots_);
        result_slots_ = nullptr;
    }
    Memory::free(result_chunks_);
    result_chunks_ = nullptr;
    result_chunk_capacity_ = 0;
    Memory::free(result_edf_sessions_);
    result_edf_sessions_ = nullptr;
    result_edf_session_count_ = 0;
    Memory::free(result_resolved_plan_);
    result_resolved_plan_ = nullptr;
    Memory::free(result_resolve_scratch_);
    result_resolve_scratch_ = nullptr;
    Memory::free(prepare_indexed_night_);
    prepare_indexed_night_ = nullptr;
    Memory::free(range_indexed_night_);
    range_indexed_night_ = nullptr;
    Memory::free(index_cache_);
    index_cache_ = nullptr;
    index_cache_count_ = 0;
    index_cache_valid_ = false;
    Memory::free(range_chunks_);
    range_chunks_ = nullptr;
    range_chunk_count_ = 0;
    Memory::free(range_edf_sessions_);
    range_edf_sessions_ = nullptr;
    range_edf_session_count_ = 0;
    Memory::free(durable_index_);
    durable_index_ = nullptr;
    durable_index_count_ = 0;
    durable_index_valid_ = false;
    Memory::free(durable_index_save_);
    durable_index_save_ = nullptr;
    durable_index_save_count_ = 0;
    durable_index_save_pending_ = false;
    if (summary_lock_) {
        vSemaphoreDelete(summary_lock_);
        summary_lock_ = nullptr;
    }
    if (summary_scratch_lock_) {
        vSemaphoreDelete(summary_scratch_lock_);
        summary_scratch_lock_ = nullptr;
    }
    if (result_slots_lock_) {
        vSemaphoreDelete(result_slots_lock_);
        result_slots_lock_ = nullptr;
    }
    if (index_cache_lock_) {
        vSemaphoreDelete(index_cache_lock_);
        index_cache_lock_ = nullptr;
    }
    if (durable_index_lock_) {
        vSemaphoreDelete(durable_index_lock_);
        durable_index_lock_ = nullptr;
    }
    if (plot_cache_write_lock_) {
        if (xSemaphoreTake(plot_cache_write_lock_, portMAX_DELAY) == pdTRUE) {
            reset_result_cache_write_locked();
            xSemaphoreGive(plot_cache_write_lock_);
        }
        vSemaphoreDelete(plot_cache_write_lock_);
        plot_cache_write_lock_ = nullptr;
    }
    if (build_queue_lock_) {
        vSemaphoreDelete(build_queue_lock_);
        build_queue_lock_ = nullptr;
    }
    if (prefetch_lock_) {
        vSemaphoreDelete(prefetch_lock_);
        prefetch_lock_ = nullptr;
    }
    if (cache_write_lock_) {
        vSemaphoreDelete(cache_write_lock_);
        cache_write_lock_ = nullptr;
    }
}

void ReportManager::begin() {
    if (!summary_lock_) summary_lock_ = xSemaphoreCreateMutex();
    if (!summary_scratch_lock_) summary_scratch_lock_ = xSemaphoreCreateMutex();
    if (!index_cache_lock_) index_cache_lock_ = xSemaphoreCreateMutex();
    if (!durable_index_lock_) durable_index_lock_ = xSemaphoreCreateMutex();
    if (!plot_cache_write_lock_) {
        plot_cache_write_lock_ = xSemaphoreCreateMutex();
    }
    if (!prefetch_lock_) prefetch_lock_ = xSemaphoreCreateMutex();
    if (!build_queue_lock_) build_queue_lock_ = xSemaphoreCreateMutex();
    if (!cache_write_lock_) cache_write_lock_ = xSemaphoreCreateMutex();
    if (!night_epochs_) {
        night_epochs_ = static_cast<NightEpoch *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX, sizeof(NightEpoch), false));
        if (!night_epochs_) {
            log_report_alloc_failed(
                "night_epochs",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(NightEpoch));
        }
    }
    ensure_cache_source_night_extents();
    ensure_cache_coalesce_slots();
    ensure_cache_write_queue_slots();
    load_durable_night_index();
    clear_summary_records();
    summary_status_ = {};
    if (!load_summary_from_store()) {
        publish_summary_json_snapshot();
    }
}

bool ReportManager::ensure_cache_source_night_extents() {
    if (cache_source_night_extent_ms_) return true;
    cache_source_night_extent_ms_ = static_cast<int64_t *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(int64_t),
                             false));
    if (!cache_source_night_extent_ms_) {
        log_report_alloc_failed(
            "cache_source_night_extents",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
        return false;
    }
    return true;
}

void ReportManager::set_edf_report_catalog(EdfReportCatalogJob *catalog) {
    edf_catalog_ = catalog;
}

bool ReportManager::take_summary_lock(TickType_t timeout) const {
    return !summary_lock_ || xSemaphoreTake(summary_lock_, timeout) == pdTRUE;
}

void ReportManager::give_summary_lock() const {
    if (summary_lock_) xSemaphoreGive(summary_lock_);
}

bool ReportManager::take_summary_scratch(TickType_t timeout,
                                         ReportSummaryRecord *&out) {
    out = nullptr;
    if (!summary_scratch_lock_ ||
        xSemaphoreTake(summary_scratch_lock_, timeout) != pdTRUE) {
        return false;
    }
    if (!summary_scratch_) {
        summary_scratch_ = static_cast<ReportSummaryRecord *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportSummaryRecord),
                                 false));
    }
    if (!summary_scratch_) {
        xSemaphoreGive(summary_scratch_lock_);
        log_report_alloc_failed(
            "summary_scratch",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        return false;
    }
    out = summary_scratch_;
    return true;
}

void ReportManager::give_summary_scratch() {
    if (summary_scratch_lock_) xSemaphoreGive(summary_scratch_lock_);
}

bool ReportManager::load_durable_night_index() {
    if (!durable_index_lock_) return false;
    if (!durable_index_) {
        durable_index_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportIndexedNight),
                                 false));
        if (!durable_index_) {
            log_report_alloc_failed(
                "durable_night_index",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            return false;
        }
    }

    size_t count = 0;
    uint32_t crc = 0;
    const bool loaded = ReportNightIndexStore::load(durable_index_,
                                                    AC_REPORT_SUMMARY_RECORD_MAX,
                                                    count,
                                                    crc);
    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    durable_index_count_ = loaded ? count : 0;
    durable_index_crc_ = loaded ? crc : 0;
    durable_index_valid_ = loaded;
    xSemaphoreGive(durable_index_lock_);
    if (loaded) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index loaded nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(crc));
    }
    return loaded;
}

bool ReportManager::seed_index_from_durable(ReportNightIndex &index) const {
    if (!durable_index_lock_ || !durable_index_) return false;
    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    const bool valid = durable_index_valid_;
    const size_t count = std::min(durable_index_count_,
                                  static_cast<size_t>(
                                      AC_REPORT_SUMMARY_RECORD_MAX));
    bool ok = valid;
    for (size_t i = 0; valid && i < count; ++i) {
        if (!index.add_indexed_night(durable_index_[i])) {
            ok = false;
            break;
        }
    }
    xSemaphoreGive(durable_index_lock_);
    return ok;
}

void ReportManager::schedule_durable_night_index_save(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t content_crc) const {
    if ((!src && count) || count > AC_REPORT_SUMMARY_RECORD_MAX ||
        !durable_index_lock_) {
        return;
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        const bool unchanged = durable_index_valid_ &&
                               durable_index_crc_ == content_crc &&
                               !durable_index_save_pending_;
        xSemaphoreGive(durable_index_lock_);
        if (unchanged) return;
    }

    if (!durable_index_save_) {
        durable_index_save_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportIndexedNight),
                                 false));
        if (!durable_index_save_) {
            log_report_alloc_failed(
                "durable_night_index_save",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            return;
        }
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (count > 0) {
        memcpy(durable_index_save_, src, count * sizeof(ReportIndexedNight));
    }
    durable_index_save_count_ = count;
    durable_index_save_crc_ = content_crc;
    durable_index_save_pending_ = true;
    durable_index_save_requested_ms_ = millis() + 1000;
    xSemaphoreGive(durable_index_lock_);
}

bool ReportManager::service_durable_night_index_writer() {
    if (!durable_index_lock_) return false;
    size_t count = 0;
    uint32_t expected_crc = 0;

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    if (!durable_index_save_pending_) {
        xSemaphoreGive(durable_index_lock_);
        return false;
    }
    const uint32_t now = millis();
    if (static_cast<int32_t>(now - durable_index_save_requested_ms_) < 0) {
        xSemaphoreGive(durable_index_lock_);
        return false;
    }
    count = durable_index_save_count_;
    expected_crc = durable_index_save_crc_;
    xSemaphoreGive(durable_index_lock_);

    ReportIndexedNight *snapshot = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!snapshot) {
        log_report_alloc_failed(
            "durable_night_index_write_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            durable_index_save_requested_ms_ = millis() + 60000;
            xSemaphoreGive(durable_index_lock_);
        }
        return false;
    }

    if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        Memory::free(snapshot);
        return false;
    }
    count = std::min(durable_index_save_count_,
                     static_cast<size_t>(AC_REPORT_SUMMARY_RECORD_MAX));
    expected_crc = durable_index_save_crc_;
    if (count > 0) {
        memcpy(snapshot,
               durable_index_save_,
               count * sizeof(ReportIndexedNight));
    }
    xSemaphoreGive(durable_index_lock_);

    uint32_t written_crc = 0;
    const bool ok = ReportNightIndexStore::save(snapshot, count, written_crc);
    if (ok) {
        if (!durable_index_) {
            durable_index_ = static_cast<ReportIndexedNight *>(
                Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                     sizeof(ReportIndexedNight),
                                     false));
        }
        if (durable_index_ &&
            xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
            memcpy(durable_index_,
                   snapshot,
                   count * sizeof(ReportIndexedNight));
            durable_index_count_ = count;
            durable_index_crc_ = written_crc;
            durable_index_valid_ = true;
            if (durable_index_save_pending_ &&
                durable_index_save_crc_ == expected_crc &&
                durable_index_save_count_ == count) {
                durable_index_save_pending_ = false;
            }
            xSemaphoreGive(durable_index_lock_);
        }
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Durable night index saved nights=%lu crc=0x%08lx\n",
                  static_cast<unsigned long>(count),
                  static_cast<unsigned long>(written_crc));
    } else {
        if (xSemaphoreTake(durable_index_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            durable_index_save_requested_ms_ = millis() + 60000;
            xSemaphoreGive(durable_index_lock_);
        }
        Log::logf(CAT_REPORT, LOG_WARN,
                  "Durable night index save failed\n");
    }
    Memory::free(snapshot);
    return ok;
}

bool ReportManager::build_indexed_nights(ReportIndexedNight *out,
                                         size_t capacity,
                                         size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    uint32_t catalog_refresh_id = 0;
    if (!index_cache_key(summary_revision,
                         catalog_present,
                         catalog_state,
                         catalog_refresh_id)) {
        return false;
    }

    if (index_cache_lock_ &&
        xSemaphoreTake(index_cache_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        const bool copied = copy_index_cache_locked(out,
                                                    capacity,
                                                    count,
                                                    summary_revision,
                                                    catalog_present,
                                                    catalog_state,
                                                    catalog_refresh_id);
        xSemaphoreGive(index_cache_lock_);
        if (copied) return true;
    }

    ReportIndexedNight *fresh =
        static_cast<ReportIndexedNight *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportIndexedNight),
            false));
    if (!fresh) {
        log_report_alloc_failed(
            "report_night_index_build",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t fresh_count = 0;
    const bool ready =
        build_indexed_nights_uncached(fresh,
                                      AC_REPORT_SUMMARY_RECORD_MAX,
                                      fresh_count);
    if (!ready) {
        Memory::free(fresh);
        return false;
    }

    if (index_cache_lock_ &&
        xSemaphoreTake(index_cache_lock_, pdMS_TO_TICKS(20)) == pdTRUE) {
        (void)publish_index_cache_locked(fresh,
                                         fresh_count,
                                         summary_revision,
                                         catalog_present,
                                         catalog_state,
                                         catalog_refresh_id);
        xSemaphoreGive(index_cache_lock_);
    }

    count = fresh_count < capacity ? fresh_count : capacity;
    if (count > 0) {
        memcpy(out, fresh, count * sizeof(ReportIndexedNight));
    }
    Memory::free(fresh);
    return true;
}

bool ReportManager::build_indexed_nights_uncached(ReportIndexedNight *out,
                                                  size_t capacity,
                                                  size_t &count) const {
    count = 0;
    if (!out || capacity == 0) return false;

    ReportNightIndex index(out, capacity);
    (void)seed_index_from_durable(index);

    if (take_summary_lock(pdMS_TO_TICKS(20))) {
        const size_t raw_count = records_ ? record_count_ : 0;
        for (size_t i = 0; i < raw_count; ++i) {
            if (!index.add_summary_record(records_[i])) break;
        }
        give_summary_lock();
    }

    ReportIndexedNight *sort_scratch =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            sizeof(ReportIndexedNight),
            false));
    if (!sort_scratch) {
        log_report_alloc_failed("report_night_index_sort",
                                sizeof(ReportIndexedNight));
        return false;
    }

    auto finish_index = [&](bool catalog_pending,
                            bool authoritative) -> bool {
        if (!index.finish(sort_scratch)) {
            Memory::free(sort_scratch);
            return false;
        }
        count = index.count();
        if (catalog_pending) {
            for (size_t i = 0; i < count; ++i) {
                out[i].edf_catalog_pending =
                    !out[i].has_edf ||
                    !indexed_night_summary_ranges_covered_by_data(out[i]);
            }
        }
        if (authoritative) {
            uint32_t crc = 0;
            if (ReportNightIndexStore::content_crc(out, count, crc)) {
                schedule_durable_night_index_save(out, count, crc);
            }
        }
        Memory::free(sort_scratch);
        sort_scratch = nullptr;
        return true;
    };

    if (!edf_catalog_) {
        return finish_index(false, false);
    }

    EdfReportCatalogStatus catalog_status;
    const bool have_catalog_status = edf_catalog_->status(catalog_status, 0);
    if (!have_catalog_status ||
        catalog_status.state != EdfReportCatalogState::Ready) {
        if (!have_catalog_status ||
            catalog_status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_->request_refresh();
        }
        return finish_index(!have_catalog_status ||
                            catalog_status.state !=
                                EdfReportCatalogState::Error,
                            false);
    }

    int32_t timezone_offset_min = 0;
    const bool have_timezone =
        edf_catalog_->timezone_offset_minutes(timezone_offset_min);
    const size_t catalog_count = edf_catalog_->session_count();
    EdfReportSessionDescriptor *session_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!session_scratch) {
        Memory::free(sort_scratch);
        log_report_alloc_failed("report_night_edf_session_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        Memory::free(session_scratch);
        Memory::free(sort_scratch);
        log_report_alloc_failed("report_night_edf_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < catalog_count; ++i) {
            if (!edf_catalog_->copy_session(i, *session_scratch)) continue;
            const bool has_numeric =
                edf_session_has_report_numeric(*session_scratch);
            const bool has_annotation =
                edf_session_has_report_annotation(*session_scratch);
            if (pass == 0) {
                if (!has_numeric ||
                    !edf_catalog_session_reportable(*session_scratch,
                                                    *marker_scratch)) {
                    continue;
                }
            } else {
                if (has_numeric || !has_annotation ||
                    !edf_catalog_session_reportable(*session_scratch,
                                                    *marker_scratch)) {
                    continue;
                }
            }
            if (!index.add_edf_session(*session_scratch,
                                       have_timezone,
                                       timezone_offset_min)) {
                break;
            }
        }
    }
    Memory::free(marker_scratch);
    Memory::free(session_scratch);

    return finish_index(false, true);
}

bool ReportManager::index_cache_key(
    uint32_t &summary_revision,
    bool &catalog_present,
    uint8_t &catalog_state,
    uint32_t &catalog_refresh_id) const {
    summary_revision = 0;
    catalog_present = edf_catalog_ != nullptr;
    catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    catalog_refresh_id = 0;

    if (take_summary_lock(pdMS_TO_TICKS(20))) {
        summary_revision = summary_status_.revision;
        give_summary_lock();
    } else {
        return false;
    }

    if (!edf_catalog_) return true;

    EdfReportCatalogStatus catalog_status;
    if (!edf_catalog_->status(catalog_status, 0)) {
        catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Refreshing);
        return true;
    }
    catalog_state = static_cast<uint8_t>(catalog_status.state);
    catalog_refresh_id = catalog_status.refresh_id;
    return true;
}

bool ReportManager::index_cache_matches(
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    return index_cache_valid_ &&
           index_cache_summary_revision_ == summary_revision &&
           index_cache_catalog_present_ == catalog_present &&
           index_cache_catalog_state_ == catalog_state &&
           index_cache_catalog_refresh_id_ == catalog_refresh_id &&
           index_cache_ != nullptr;
}

bool ReportManager::copy_index_cache_locked(
    ReportIndexedNight *out,
    size_t capacity,
    size_t &count,
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    count = 0;
    if (!out || capacity == 0 ||
        !index_cache_matches(summary_revision,
                             catalog_present,
                             catalog_state,
                             catalog_refresh_id)) {
        return false;
    }
    count = index_cache_count_ < capacity ? index_cache_count_ : capacity;
    if (count > 0) {
        memcpy(out, index_cache_, count * sizeof(ReportIndexedNight));
    }
    return true;
}

bool ReportManager::publish_index_cache_locked(
    const ReportIndexedNight *src,
    size_t count,
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    if (!src) return false;
    if (!index_cache_) {
        index_cache_ = static_cast<ReportIndexedNight *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportIndexedNight),
            false));
        if (!index_cache_) {
            log_report_alloc_failed(
                "report_night_index_cache",
                AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
            index_cache_valid_ = false;
            index_cache_count_ = 0;
            return false;
        }
    }

    const size_t stored = count < AC_REPORT_SUMMARY_RECORD_MAX
                              ? count
                              : AC_REPORT_SUMMARY_RECORD_MAX;
    if (stored > 0) {
        memcpy(index_cache_, src, stored * sizeof(ReportIndexedNight));
    }
    if (stored < index_cache_count_) {
        memset(index_cache_ + stored,
               0,
               (index_cache_count_ - stored) * sizeof(ReportIndexedNight));
    }

    index_cache_count_ = stored;
    index_cache_summary_revision_ = summary_revision;
    index_cache_catalog_present_ = catalog_present;
    index_cache_catalog_state_ = catalog_state;
    index_cache_catalog_refresh_id_ = catalog_refresh_id;
    index_cache_valid_ = true;
    return true;
}

bool ReportManager::indexed_night_by_therapy_index(
    size_t therapy_index,
    ReportIndexedNight &out) const {
    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "report_night_index_lookup",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }
    size_t count = 0;
    bool found = false;
    if (build_indexed_nights(snapshot,
                             AC_REPORT_SUMMARY_RECORD_MAX,
                             count)) {
        found = ReportNightIndex::by_therapy_index(snapshot,
                                                   count,
                                                   therapy_index,
                                                   out);
    }
    Memory::free(snapshot);
    return found;
}

bool ReportManager::indexed_night_by_start(uint64_t night_start_ms,
                                           ReportIndexedNight &out,
                                           size_t *therapy_index_out) const {
    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "report_night_index_lookup",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }
    size_t count = 0;
    bool found = false;
    if (build_indexed_nights(snapshot,
                             AC_REPORT_SUMMARY_RECORD_MAX,
                             count)) {
        found = ReportNightIndex::by_start(snapshot,
                                           count,
                                           night_start_ms,
                                           out,
                                           therapy_index_out);
    }
    Memory::free(snapshot);
    return found;
}

bool ReportManager::request_summary_refresh(bool force) {
    if (summary_fetch_active_ && !force) return true;
    if (cache_fetch_active_) return false;

    SpoolClientRequest request;
    request.spool_type = "Summary";
    request.from_dt = REPORT_SUMMARY_FROM;
    request.max_size = AC_REPORT_SUMMARY_SPOOL_ROUND_BYTES;
    request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
    request.max_notifications = AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
    request.max_rounds = 64;
    request.pace_on_backpressure = true;
    request.stream_rounds = false;

    if (!spool_.begin(request)) {
        fail_summary("summary_start_failed");
        return false;
    }

    summary_fetch_active_ = true;
    summary_started_ms_ = millis();
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Fetching;
        summary_status_.active_spool = "Summary";
        summary_status_.error.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }
    publish_summary_json_snapshot();
    Log::logf(CAT_REPORT, LOG_INFO, "Summary refresh queued\n");
    return true;
}

void ReportManager::poll(RpcArbiter &arbiter) {
    const bool realtime_active =
        arbiter.stream_realtime_active() ||
        arbiter.as11_state().therapy_state() == As11TherapyState::Running;

    // Foreground service points
    if (drain_source_events(arbiter)) return;

    service_build_queue(realtime_active);
    service_range_plot(realtime_active);
    service_prefetch(realtime_active);

    // EDF catalog changes invalidate summary-derived materialization.
    if (edf_catalog_) {
        EdfReportCatalogStatus catalog_status;

        if (edf_catalog_->status(catalog_status, 0) &&
            catalog_status.state == EdfReportCatalogState::Ready &&
            catalog_status.refresh_id != 0 &&
            catalog_status.refresh_id != edf_catalog_summary_refresh_id_) {
            invalidate_materialized(0, true);

            if (publish_summary_json_snapshot()) {
                edf_catalog_summary_refresh_id_ = catalog_status.refresh_id;
            }
        }
    }

    // Cross-task summary revision publication
    if (take_summary_lock(0)) {
        summary_revision_pub_.store(summary_status_.revision);
        give_summary_lock();
    }

    if (!spool_.active()) {
        observed_spool_rx_queue_full_alerts_ =
            arbiter.can_driver().stats().rx_queue_full_alerts;
    }

    // Idle trash cleanup
    if (!realtime_active &&
        !summary_fetch_active_ && !cache_fetch_active_ &&
        !plot_build_active_ && !range_build_active_ &&
        static_cast<int32_t>(millis() - next_trash_cleanup_ms_) >= 0) {
        next_trash_cleanup_ms_ = millis() + 250;

        uint32_t removed = 0;
        ReportStore::cleanup_trash_step(4, removed);
    }

    // Active cache fetch
    if (cache_fetch_active_) {
        poll_cache_fetch(arbiter);
    }

    // Active full-plot build
    if (plot_build_active_) {
        if (plot_build_idle_prebuild_) {
            const char *reason = "idle";

            if (realtime_active || !idle_prebuild_gate_open(&reason)) {
                const uint32_t elapsed_ms =
                    plot_build_started_ms_
                        ? static_cast<uint32_t>(millis() -
                                                plot_build_started_ms_)
                        : 0;
                Log::logf(CAT_REPORT,
                          LOG_DEBUG,
                          "Idle plot prebuild aborted reason=%s "
                          "night=%llu elapsed_ms=%lu\n",
                          realtime_active ? "realtime" :
                                            (reason ? reason : "gate"),
                          static_cast<unsigned long long>(
                              plot_build_night_start_ms_.load()),
                          static_cast<unsigned long>(elapsed_ms));
                reset_plot_build();
                release_result_edf_sessions();

                return;
            }
        }

        poll_result_plot_build();
        return;
    }

    // Active range-plot build
    if (range_build_active_) {
        if (realtime_active) return;

        poll_range_plot_build();
        return;
    }

    // Summary spool fetch
    if (!summary_fetch_active_) return;

    spool_.poll(arbiter);
    log_spool_can_pressure(arbiter);
    bool publish_progress = false;
    const uint32_t now_ms = millis();
    if (take_summary_lock(0)) {
        summary_status_.spool = spool_.status();
        summary_status_.elapsed_ms = summary_started_ms_
            ? now_ms - summary_started_ms_
            : 0;
        give_summary_lock();
        if (static_cast<int32_t>(now_ms - next_summary_progress_snapshot_ms_) >=
            0) {
            next_summary_progress_snapshot_ms_ = now_ms + 500;
            publish_progress = true;
        }
    }
    if (publish_progress) publish_summary_json_snapshot();

    if (spool_.complete()) {
        finish_summary_fetch();
    } else if (spool_.failed()) {
        fail_summary(spool_.status().error.c_str());
    }
}

bool ReportManager::handle_event(const RpcEvent &event) {
    if (cache_fetch_active_ && spool_.handle_event(event)) return true;
    if (summary_fetch_active_ && spool_.handle_event(event)) return true;
    return false;
}

bool ReportManager::drain_source_events(RpcArbiter &arbiter) {
    bool handled = false;
    for (size_t i = 0; i < AC_REPORT_SOURCE_EVENT_DRAIN_BUDGET; ++i) {
        RpcEvent event;
        if (!arbiter.next_source_event(RpcSource::Report, event)) break;
        (void)handle_event(event);
        handled = true;
    }
    return handled;
}

bool ReportManager::write_parsed_chunk(void *context,
                                       const ReportParsedChunk &chunk) {
    ChunkWriteContext *ctx = static_cast<ChunkWriteContext *>(context);
    if (!ctx || !ctx->manager || !chunk.name || !chunk.name[0] ||
        !chunk.payload || chunk.payload_schema == 0 ||
        chunk.record_count == 0 ||
        chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        if (ctx && ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_chunk");
        }
        return false;
    }
    if (!report_source_spool_type(chunk.source)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s", "bad_cache_source");
        }
        return false;
    }
    if (chunk.kind == ReportStoreChunkKind::Series &&
        chunk.payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s",
                     "bad_series_payload_schema");
        }
        return false;
    }
    if (chunk.kind == ReportStoreChunkKind::Events &&
        chunk.payload_schema != REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            snprintf(ctx->error, ctx->error_len, "%s",
                     "bad_event_payload_schema");
        }
        return false;
    }
    if (!ctx->manager->buffer_parsed_chunk(chunk)) {
        if (ctx->error && ctx->error_len && !ctx->error[0]) {
            const std::string &detail = ctx->manager->cache_status_.error;
            snprintf(ctx->error,
                     ctx->error_len,
                     "%s",
                     detail.empty() ? "cache_chunk_store_failed"
                                    : detail.c_str());
        }
        return false;
    }
    return true;
}

// Map a timestamp to its summary-night bucket start (the partition key). Bucket
// boundaries sit around local noon (no therapy), so a chunk straddling one is
// filed whole by its start timestamp.
int64_t ReportManager::night_start_for_timestamp(int64_t timestamp_ms) const {
    if (!take_summary_lock(portMAX_DELAY)) return timestamp_ms;
    if (!records_ || record_count_ == 0) {
        give_summary_lock();
        return timestamp_ms;
    }
    int64_t nearest_start = 0;
    bool have_nearest = false;
    for (size_t i = 0; i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &r = records_[i];
        if (!r.valid || !r.duration_min) continue;
        const int64_t s = static_cast<int64_t>(r.start_ms);
        const int64_t e = static_cast<int64_t>(r.end_ms);
        if (timestamp_ms >= s && timestamp_ms < e) {
            give_summary_lock();
            return s;
        }
        if (s <= timestamp_ms && (!have_nearest || s > nearest_start)) {
            nearest_start = s;
            have_nearest = true;
        }
    }
    const int64_t result = have_nearest ? nearest_start : timestamp_ms;
    give_summary_lock();
    return result;
}

// Coalesce parsed chunks per compatible (kind,name) segment. A night/timebase
// break or size cap opens another active segment instead of forcing an immediate
// writer-queue flush from the parser callback; source finalization drains the
// pending segments with normal backpressure handling.
bool ReportManager::ensure_cache_coalesce_slots() {
    if (cache_coalesce_) return true;
    cache_coalesce_ = static_cast<CacheCoalesceBuffer *>(Memory::alloc_large(
        AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer), false));
    if (!cache_coalesce_) {
        log_report_alloc_failed(
            "cache_coalesce",
            AC_REPORT_COALESCE_SLOTS * sizeof(CacheCoalesceBuffer));
        return false;
    }
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        new (&cache_coalesce_[i]) CacheCoalesceBuffer();
    }
    return true;
}

bool ReportManager::ensure_cache_write_queue_slots() {
    if (cache_write_queue_) return true;
    cache_write_queue_ = static_cast<CacheWriteQueueSlot *>(Memory::alloc_large(
        AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot), false));
    if (!cache_write_queue_) {
        log_report_alloc_failed(
            "cache_write_queue",
            AC_REPORT_CACHE_WRITE_QUEUE_MAX * sizeof(CacheWriteQueueSlot));
        return false;
    }
    for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
        new (&cache_write_queue_[i]) CacheWriteQueueSlot();
    }
    return true;
}

bool ReportManager::buffer_parsed_chunk(const ReportParsedChunk &chunk) {
    if (!ensure_cache_coalesce_slots()) {
        fail_cache_fetch("cache_coalesce_alloc_failed");
        return false;
    }

    const int64_t night = night_start_for_timestamp(chunk.start_ms);
    const bool is_series = chunk.kind == ReportStoreChunkKind::Series;
    ReportSeriesV2UniformView series_view;
    if (is_series) {
        if (!report_series_payload_v2_uniform_view(chunk.payload,
                                                   chunk.payload_len,
                                                   chunk.record_count,
                                                   series_view) ||
            series_view.missing_bitmap_bytes != 0) {
            fail_cache_fetch("cache_series_v2_invalid");
            return false;
        }
    }

    const size_t incoming_bytes =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    const size_t fixed_bytes =
        is_series ? report_series_v2_header_size() : 0;

    size_t slot = AC_REPORT_COALESCE_SLOTS;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (cache_coalesce_[i].active &&
            cache_coalesce_[i].kind == chunk.kind &&
            cache_coalesce_[i].name == chunk.name) {
            CacheCoalesceBuffer &buf = cache_coalesce_[i];
            bool size_overflow = incoming_bytes > SIZE_MAX - buf.payload.size();
            size_t projected_size =
                size_overflow ? SIZE_MAX : buf.payload.size() + incoming_bytes;
            if (!size_overflow) {
                size_overflow = fixed_bytes > SIZE_MAX - projected_size;
                if (!size_overflow) projected_size += fixed_bytes;
            }
            if (size_overflow ||
                projected_size > AC_REPORT_COALESCE_TARGET_BYTES) {
                continue;
            }
            if (buf.night_start_ms != night) continue;
            if (is_series &&
                (buf.series_interval_ms != series_view.interval_ms ||
                 chunk.start_ms != buf.last_ms)) {
                continue;
            }
            slot = i;
            break;
        }
    }

    if (slot == AC_REPORT_COALESCE_SLOTS) {
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            if (!cache_coalesce_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == AC_REPORT_COALESCE_SLOTS) {
            const CacheFlushResult flush = flush_cache_coalesce_buffer(0);
            if (flush == CacheFlushResult::Blocked) {
                cache_status_.error = "cache_coalesce_backpressure";
                return false;
            }
            if (flush == CacheFlushResult::Failed) {
                cache_status_.error = "cache_coalesce_flush_failed";
                return false;
            }
            slot = 0;
        }
        CacheCoalesceBuffer &buf = cache_coalesce_[slot];
        buf.active = true;
        buf.kind = chunk.kind;
        buf.source = chunk.source;
        buf.name = chunk.name;
        buf.night_start_ms = night;
        buf.first_ms = chunk.start_ms;
        buf.last_ms = chunk.end_ms;
        buf.record_count = 0;
        buf.payload_schema = chunk.payload_schema;
        buf.series_interval_ms = is_series ? series_view.interval_ms : 0;
        buf.series_values_pending = is_series;
        buf.payload.clear();
        buf.payload.set_max_size(AC_REPORT_COALESCE_TARGET_BYTES + 65536);
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    const uint8_t *append_data =
        is_series ? series_view.values_milli_le : chunk.payload;
    const size_t append_len =
        is_series ? series_view.values_milli_bytes : chunk.payload_len;
    if (!buf.payload.append(append_data, append_len)) {
        fail_cache_fetch("cache_coalesce_alloc");
        return false;
    }
    if (chunk.start_ms < buf.first_ms) buf.first_ms = chunk.start_ms;
    if (chunk.end_ms > buf.last_ms) buf.last_ms = chunk.end_ms;
    buf.record_count += chunk.record_count;
    note_cache_chunk_coverage(chunk);
    return true;
}

ReportManager::CacheFlushResult ReportManager::flush_cache_coalesce_buffer(
    size_t slot) {
    if (!cache_coalesce_ || slot >= AC_REPORT_COALESCE_SLOTS) {
        return CacheFlushResult::Flushed;
    }
    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    if (!buf.active) return CacheFlushResult::Flushed;

    CacheFlushResult result = CacheFlushResult::Flushed;
    if (buf.record_count > 0 && buf.payload.size() > 0 &&
        buf.last_ms > buf.first_ms) {
        ReportStoreChunkKey key;
        key.kind = buf.kind;
        key.source = report_source_spool_type(buf.source);
        key.name = buf.name;
        key.start_ms = buf.first_ms;
        key.end_ms = buf.last_ms;
        key.night_start_ms = buf.night_start_ms;
        if (!key.source || !key.source[0]) {
            result = CacheFlushResult::Failed;
        } else {
            ReportStoreChunkMeta meta;
            meta.record_count = buf.record_count;
            meta.payload_schema = buf.payload_schema;
            if (buf.kind == ReportStoreChunkKind::Series &&
                buf.series_values_pending) {
                ReportSpoolBuffer built;
                if (!report_build_series_payload_v2_uniform_values_le(
                        built,
                        buf.series_interval_ms,
                        buf.payload.data(),
                        buf.record_count)) {
                    Log::logf(CAT_REPORT,
                              LOG_WARN,
                              "Cache series v2 build failed source=%s "
                              "name=%s records=%lu\n",
                              key.source ? key.source : "",
                              key.name ? key.name : "",
                              static_cast<unsigned long>(buf.record_count));
                    result = CacheFlushResult::Failed;
                } else {
                    buf.payload.move_from(built);
                    buf.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
                    buf.series_values_pending = false;
                    meta.payload_schema = buf.payload_schema;
                }
            }
            if (result == CacheFlushResult::Flushed) {
                const CacheWriteEnqueueResult queued =
                    enqueue_cache_write(buf, key, meta);
                if (queued == CacheWriteEnqueueResult::Blocked) {
                    return CacheFlushResult::Blocked;
                }
                if (queued == CacheWriteEnqueueResult::Failed) {
                    result = CacheFlushResult::Failed;
                }
            }
        }
    }

    buf.active = false;
    buf.payload.clear();
    buf.record_count = 0;
    buf.first_ms = 0;
    buf.last_ms = 0;
    buf.night_start_ms = 0;
    buf.name = nullptr;
    buf.payload_schema = 0;
    buf.series_interval_ms = 0;
    buf.series_values_pending = false;
    return result;
}

ReportManager::CacheWriteEnqueueResult ReportManager::enqueue_cache_write(
    CacheCoalesceBuffer &buf,
    const ReportStoreChunkKey &key,
    const ReportStoreChunkMeta &meta) {
    if (!cache_write_lock_ || !ensure_cache_write_queue_slots()) {
        return CacheWriteEnqueueResult::Failed;
    }
    if (!xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return CacheWriteEnqueueResult::Blocked;
    }
    if (cache_write_count_ >= AC_REPORT_CACHE_WRITE_QUEUE_MAX) {
        xSemaphoreGive(cache_write_lock_);
        Log::logf(CAT_REPORT, LOG_DEBUG,
                  "Cache chunk writer backpressure source=%s name=%s\n",
                  key.source ? key.source : "",
                  key.name ? key.name : "");
        return CacheWriteEnqueueResult::Blocked;
    }

    CacheWriteQueueSlot &job = cache_write_queue_[cache_write_tail_];
    job.active = true;
    job.fetch_id = cache_write_fetch_id_;
    job.key = key;
    job.meta = meta;
    job.payload.clear();
    job.payload.move_from(buf.payload);
    cache_write_tail_ =
        (cache_write_tail_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    cache_write_count_++;
    cache_write_pending_++;
    xSemaphoreGive(cache_write_lock_);

    if (BackgroundWorker *worker = background_worker()) {
        worker->wake();
    }
    return CacheWriteEnqueueResult::Queued;
}

ReportManager::CacheFlushResult
ReportManager::flush_all_cache_coalesce_buffers() {
    if (!cache_coalesce_) return CacheFlushResult::Flushed;
    CacheFlushResult result = CacheFlushResult::Flushed;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        const CacheFlushResult flush = flush_cache_coalesce_buffer(i);
        if (flush == CacheFlushResult::Failed) return flush;
        if (flush == CacheFlushResult::Blocked) {
            result = CacheFlushResult::Blocked;
        }
    }
    return result;
}

void ReportManager::discard_cache_coalesce_buffers() {
    if (!cache_coalesce_) return;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        CacheCoalesceBuffer &buf = cache_coalesce_[i];
        buf.active = false;
        buf.payload.clear();
        buf.record_count = 0;
        buf.first_ms = 0;
        buf.last_ms = 0;
        buf.night_start_ms = 0;
        buf.name = nullptr;
        buf.payload_schema = 0;
        buf.series_interval_ms = 0;
        buf.series_values_pending = false;
    }
}

void ReportManager::reset_cache_write_fetch_state_locked() {
    if (cache_write_queue_) {
        for (size_t i = 0; i < AC_REPORT_CACHE_WRITE_QUEUE_MAX; ++i) {
            CacheWriteQueueSlot &job = cache_write_queue_[i];
            job.active = false;
            job.payload.clear();
        }
    }
    cache_write_head_ = 0;
    cache_write_tail_ = 0;
    cache_write_count_ = 0;
    cache_write_pending_ = 0;
    cache_write_failed_fetch_id_ = 0;
    cache_write_error_.clear();
    ++cache_write_fetch_id_;
    if (cache_write_fetch_id_ == 0) ++cache_write_fetch_id_;
}

void ReportManager::begin_cache_write_fetch() {
    if (!cache_write_lock_) return;
    xSemaphoreTake(cache_write_lock_, portMAX_DELAY);
    reset_cache_write_fetch_state_locked();
    xSemaphoreGive(cache_write_lock_);
    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
}

void ReportManager::abort_cache_write_fetch() {
    if (!cache_write_lock_) return;
    xSemaphoreTake(cache_write_lock_, portMAX_DELAY);
    reset_cache_write_fetch_state_locked();
    xSemaphoreGive(cache_write_lock_);
    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
}

bool ReportManager::cache_writes_pending_for_active_fetch() const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return true;
    }
    const bool pending = cache_write_pending_ > 0;
    xSemaphoreGive(cache_write_lock_);
    return pending;
}

bool ReportManager::cache_write_failed_for_active_fetch(
    std::string &error) const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return false;
    }
    const bool failed =
        cache_write_failed_fetch_id_ != 0 &&
        cache_write_failed_fetch_id_ == cache_write_fetch_id_;
    if (failed) error = cache_write_error_;
    xSemaphoreGive(cache_write_lock_);
    return failed;
}

bool ReportManager::cache_write_backpressure_active() const {
    if (!cache_write_lock_ ||
        !xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(5))) {
        return true;
    }
    const bool active =
        cache_write_count_ >=
        AC_REPORT_CACHE_WRITE_BACKPRESSURE_WATERMARK;
    xSemaphoreGive(cache_write_lock_);
    return active;
}

void ReportManager::note_cache_chunk_committed(uint64_t night_start_ms) {
    cache_status_.chunks_written++;
    if (!take_summary_lock(portMAX_DELAY)) return;
    cache_data_epoch_++;
    if (night_start_ms && night_epochs_) {
        for (size_t i = 0; i < night_epoch_count_; ++i) {
            if (night_epochs_[i].night_start_ms == night_start_ms) {
                night_epochs_[i].epoch++;
                give_summary_lock();
                return;
            }
        }
        if (night_epoch_count_ < AC_REPORT_SUMMARY_RECORD_MAX) {
            night_epochs_[night_epoch_count_].night_start_ms = night_start_ms;
            night_epochs_[night_epoch_count_].epoch = 1;
            ++night_epoch_count_;
        }
    }
    give_summary_lock();
}

bool ReportManager::service_cache_writer() {
    if (!cache_write_lock_ || !cache_write_queue_) {
        if (service_result_cache_writer()) return true;
        return service_durable_night_index_writer();
    }

    CacheWriteQueueSlot job;
    if (!xSemaphoreTake(cache_write_lock_, pdMS_TO_TICKS(20))) return false;
    if (cache_write_count_ == 0) {
        xSemaphoreGive(cache_write_lock_);
        if (service_result_cache_writer()) return true;
        return service_durable_night_index_writer();
    }

    CacheWriteQueueSlot &slot = cache_write_queue_[cache_write_head_];
    job.active = slot.active;
    job.fetch_id = slot.fetch_id;
    job.key = slot.key;
    job.meta = slot.meta;
    job.payload.move_from(slot.payload);
    slot.active = false;
    cache_write_head_ =
        (cache_write_head_ + 1) % AC_REPORT_CACHE_WRITE_QUEUE_MAX;
    cache_write_count_--;
    xSemaphoreGive(cache_write_lock_);

    const uint64_t night_start_ms =
        static_cast<uint64_t>(job.key.night_start_ms);
    const size_t payload_size = job.payload.size();
    const bool ok = job.active &&
                    ReportStore::write_chunk(job.key,
                                             job.meta,
                                             job.payload.data(),
                                             job.payload.size());
    job.payload.clear();

    bool current_fetch = false;
    if (xSemaphoreTake(cache_write_lock_, portMAX_DELAY)) {
        current_fetch = job.fetch_id == cache_write_fetch_id_;
        if (current_fetch) {
            if (cache_write_pending_ > 0) cache_write_pending_--;
            if (!ok) {
                cache_write_failed_fetch_id_ = job.fetch_id;
                cache_write_error_ = "cache_write_failed";
            }
        }
        xSemaphoreGive(cache_write_lock_);
    }

    if (ok && current_fetch) {
        note_cache_chunk_committed(night_start_ms);
    } else if (!ok && current_fetch) {
        Log::logf(CAT_REPORT, LOG_WARN,
                  "Cache chunk write failed source=%s name=%s "
                  "start=%lld end=%lld bytes=%u\n",
                  job.key.source ? job.key.source : "",
                  job.key.name ? job.key.name : "",
                  static_cast<long long>(job.key.start_ms),
                  static_cast<long long>(job.key.end_ms),
                  static_cast<unsigned>(payload_size));
    }
    return true;
}

bool ReportManager::ensure_summary_records() {
    if (records_) return true;
    records_ = static_cast<ReportSummaryRecord *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportSummaryRecord),
                             false));
    if (!records_) {
        log_report_alloc_failed(
            "summary_records",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        fail_summary("summary_alloc_failed");
        return false;
    }
    return true;
}

bool ReportManager::parse_summary_result(ReportSpoolResult &result) {
    if (!ensure_summary_records()) return false;
    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) {
        fail_summary("summary_staging_alloc_failed");
        return false;
    }
    memset(staging, 0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;
    char error[64] = {};
    if (!report_parse_summary_spool(result,
                                    store_summary_record_to_buffer,
                                    &context,
                                    error,
                                    sizeof(error))) {
        give_summary_scratch();
        fail_summary(error[0] ? error : "summary_parse_failed");
        return false;
    }
    size_t write_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;
    finalize_summary_records();
    write_count = record_count_;
    if (write_count) {
        memcpy(staging, records_, write_count * sizeof(ReportSummaryRecord));
    }
    give_summary_lock();
    if (write_count &&
        !ReportStore::write_summary_records(staging, write_count)) {
        Log::logf(CAT_REPORT, LOG_WARN,
                  "Summary store write failed records=%lu\n",
                  static_cast<unsigned long>(write_count));
    }
    give_summary_scratch();
    invalidate_materialized(0, true);
    return true;
}

bool ReportManager::load_summary_from_store() {
    if (!ensure_summary_records()) return false;
    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) return false;
    memset(staging, 0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;
    if (!ReportStore::read_summary_records(store_summary_record_to_buffer,
                                           &context)) {
        give_summary_scratch();
        return false;
    }
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;
    finalize_summary_records();
    summary_status_.state = ReportSummaryState::Ready;
    summary_status_.revision++;
    summary_status_.error.clear();
    summary_status_.active_spool.clear();
    const uint32_t records_total = summary_status_.records_total;
    const uint32_t nights_with_therapy = summary_status_.nights_with_therapy;
    give_summary_lock();
    Log::logf(CAT_REPORT, LOG_INFO,
              "Summary loaded from store records=%lu "
              "therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));
    give_summary_scratch();
    publish_summary_json_snapshot();
    invalidate_materialized(0, true);
    return true;
}

void ReportManager::finalize_summary_records() {
    if (records_ && record_count_ > 1) {
        std::sort(records_, records_ + record_count_,
                  [](const ReportSummaryRecord &a,
                     const ReportSummaryRecord &b) {
                      return a.start_ms < b.start_ms;
                  });
    }
    summary_status_.records_total = static_cast<uint32_t>(record_count_);
    summary_status_.nights_with_therapy = nights_with_therapy_;
}

void ReportManager::finish_summary_fetch() {
    ReportSpoolResult result;
    spool_.move_result_to(result);
    summary_fetch_active_ = false;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.active_spool.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }
    if (!parse_summary_result(result)) return;
    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Ready;
        summary_status_.revision++;
        summary_status_.error.clear();
        records_total = summary_status_.records_total;
        nights_with_therapy = summary_status_.nights_with_therapy;
        give_summary_lock();
    }
    publish_summary_json_snapshot();
    Log::logf(CAT_REPORT, LOG_INFO,
              "Summary ready records=%lu therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));
    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        const bool refresh_cache = pending_result_refresh_cache_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index,
                                                 refresh_cache);
    }
}

void ReportManager::fail_summary(const char *message) {
    summary_fetch_active_ = false;
    std::string error;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Error;
        summary_status_.revision++;
        summary_status_.active_spool.clear();
        summary_status_.error = message ? message : "summary_error";
        summary_status_.spool = spool_.status();
        error = summary_status_.error;
        give_summary_lock();
    } else {
        error = message ? message : "summary_error";
    }
    Log::logf(CAT_REPORT, LOG_WARN, "Summary failed: %s\n",
              error.c_str());
    publish_summary_json_snapshot();
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(error.c_str());
    }
}

void ReportManager::clear_summary_records() {
    if (records_) {
        memset(records_, 0,
               AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    }
    record_count_ = 0;
    nights_with_therapy_ = 0;
}

static const char *summary_state_name_for(ReportSummaryState state) {
    switch (state) {
        case ReportSummaryState::Idle: return "idle";
        case ReportSummaryState::Fetching: return "fetching";
        case ReportSummaryState::Ready: return "ready";
        case ReportSummaryState::Error: return "error";
    }
    return "unknown";
}

const char *ReportManager::summary_state_name() const {
    return summary_state_name_for(summary_status().state);
}

ReportSummaryStatus ReportManager::summary_status() const {
    ReportSummaryStatus snapshot;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        snapshot.state = ReportSummaryState::Error;
        snapshot.error = "summary_busy";
        return snapshot;
    }
    snapshot = summary_status_;
    give_summary_lock();
    return snapshot;
}

uint32_t indexed_night_display_duration_min(const ReportIndexedNight &night) {
    uint32_t duration_min = 0;
    const size_t range_count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < range_count; ++i) {
        duration_min += report_ceil_duration_min(night.ranges[i].start_ms,
                                                 night.ranges[i].end_ms);
    }
    if (duration_min > 0) return duration_min;
    return night.summary.duration_min;
}

bool indexed_night_visible_in_summary(const ReportIndexedNight &night) {
    return night.summary.valid && night.range_count > 0 &&
           indexed_night_display_duration_min(night) > 0;
}

void append_summary_sessions_json(LargeTextBuffer &json,
                                  const ReportIndexedNight &night) {
    const size_t range_count =
        std::min(night.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    bool first = true;
    if (range_count > 0) {
        for (size_t i = 0; i < range_count; ++i) {
            const ReportSessionRange &range = night.ranges[i];
            if (range.end_ms <= range.start_ms) continue;
            if (!first) json += ',';
            first = false;
            json += "{";
            json_add_uint64(json,
                            "start",
                            static_cast<uint64_t>(range.start_ms),
                            false);
            json_add_int(json,
                         "duration_min",
                         static_cast<long>(report_ceil_duration_min(
                             range.start_ms,
                             range.end_ms)));
            json += "}";
        }
        return;
    }

    const ReportSummaryRecord &record = night.summary;
    for (uint32_t session_index = 0;
         session_index < record.session_interval_count;
         ++session_index) {
        const ReportSummarySession &session = record.sessions[session_index];
        if (!first) json += ',';
        first = false;
        json += "{";
        json_add_uint64(json, "start", session.start_ms, false);
        json_add_int(json, "duration_min",
                     static_cast<long>(session.duration_min));
        json += "}";
    }
}

void append_summary_json_from_indexed(LargeTextBuffer &json,
                                      const ReportSummaryStatus &status_snapshot,
                                      uint32_t data_epoch_snapshot,
                                      const ReportIndexedNight *snapshot,
                                      size_t record_count_snapshot) {
    json.clear();
    if (record_count_snapshot > AC_REPORT_SUMMARY_RECORD_MAX) {
        record_count_snapshot = AC_REPORT_SUMMARY_RECORD_MAX;
    }
    if (!snapshot) record_count_snapshot = 0;

    json += "{";
    json_add_string(json,
                    "state",
                    summary_state_name_for(status_snapshot.state),
                    false);
    json_add_int(json, "revision",
                 static_cast<long>(status_snapshot.revision));
    json_add_int(json, "data_epoch",
                 static_cast<long>(data_epoch_snapshot));
    json_add_string(json, "error", status_snapshot.error.c_str());
    json_add_int(json, "records_total",
                 static_cast<long>(status_snapshot.records_total));
    json_add_int(json, "nights_with_therapy",
                 static_cast<long>(status_snapshot.nights_with_therapy));
    json_add_int(json, "elapsed_ms",
                 static_cast<long>(status_snapshot.elapsed_ms));
    json_add_string(json, "active_spool",
                    status_snapshot.active_spool.c_str());
    json += ",\"spool\":{\"state\":\"";
    json += spool_client_state_name(status_snapshot.spool.state);
    json += "\",\"round\":";
    append_long(json, static_cast<long>(status_snapshot.spool.current_round));
    json += ",\"fragments\":";
    append_long(json, static_cast<long>(status_snapshot.spool.fragments));
    json += ",\"bytes\":";
    append_long(json, static_cast<long>(status_snapshot.spool.bytes));
    json += "},\"nights\":[";
    bool first = true;
    size_t therapy_seen = 0;
    for (size_t i = 0; i < record_count_snapshot; ++i) {
        const ReportIndexedNight &night = snapshot[i];
        const ReportSummaryRecord &record = night.summary;
        const uint32_t duration_min =
            indexed_night_display_duration_min(night);
        if (!indexed_night_visible_in_summary(night)) continue;
        if (!first) json += ',';
        first = false;
        const size_t therapy_index =
            status_snapshot.nights_with_therapy > therapy_seen
                ? status_snapshot.nights_with_therapy - therapy_seen - 1
                : 0;
        therapy_seen++;
        json += "{";
        json_add_int(json, "index", static_cast<long>(i), false);
        json_add_int(json, "therapy_index",
                     static_cast<long>(therapy_index));
        json_add_uint64(json, "start", record.start_ms);
        json_add_uint64(json, "end", record.end_ms);
        json_add_int(json, "duration_min",
                     static_cast<long>(duration_min));
        if (record.has_tz_offset_min) {
            json_add_int(json, "tz_offset_min",
                         static_cast<long>(record.tz_offset_min));
        }
        append_optional_float(json, "ahi", record.has_ahi, record.ahi);
        append_optional_float(json, "apnea_index",
                              record.has_apnea_index,
                              record.apnea_index);
        append_optional_float(json, "hypopnea_index",
                              record.has_hypopnea_index,
                              record.hypopnea_index);
        append_optional_float(json, "oa_index",
                              record.has_oa_index,
                              record.oa_index);
        append_optional_float(json, "ca_index",
                              record.has_ca_index,
                              record.ca_index);
        append_optional_float(json, "ua_index",
                              record.has_ua_index,
                              record.ua_index);
        append_optional_float(json, "rera_index",
                              record.has_rera_index,
                              record.rera_index);
        json_add_int(json, "session_count",
                     static_cast<long>(night.range_count > 0
                                           ? night.range_count
                                           : record.session_count));
        json += ",\"sessions\":[";
        append_summary_sessions_json(json, night);
        json += "]";
        json_add_int(json, "has_summary", night.has_summary ? 1 : 0);
        json_add_int(json, "has_edf", night.has_edf ? 1 : 0);
        json += ",\"id\":\"";
        append_u64(json, record.start_ms);
        json += "\"}";
    }
    json += "]}";
}

bool ReportManager::publish_summary_json_snapshot() {
    ReportIndexedNight *indexed_snapshot = nullptr;
    ReportSummaryStatus status_snapshot;
    uint32_t data_epoch_snapshot = 0;
    size_t record_count_snapshot = 0;
    bool ok = true;

    if (take_summary_lock(portMAX_DELAY)) {
        status_snapshot = summary_status_;
        data_epoch_snapshot = cache_data_epoch_;
        give_summary_lock();
    } else {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_busy";
        ok = false;
    }

    indexed_snapshot = static_cast<ReportIndexedNight *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportIndexedNight),
                             false));
    if (!indexed_snapshot) {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_snapshot_alloc";
        record_count_snapshot = 0;
        ok = false;
    } else if (!build_indexed_nights(indexed_snapshot,
                                     AC_REPORT_SUMMARY_RECORD_MAX,
                                     record_count_snapshot)) {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_snapshot_build";
        record_count_snapshot = 0;
        Memory::free(indexed_snapshot);
        indexed_snapshot = nullptr;
        ok = false;
    } else {
        status_snapshot.records_total =
            static_cast<uint32_t>(record_count_snapshot);
    }
    uint32_t nights_with_therapy = 0;
    for (size_t i = 0; indexed_snapshot && i < record_count_snapshot; ++i) {
        if (indexed_night_visible_in_summary(indexed_snapshot[i])) {
            nights_with_therapy++;
        }
    }
    status_snapshot.nights_with_therapy = nights_with_therapy;

    summary_json_build_.clear();
    append_summary_json_from_indexed(summary_json_build_,
                                     status_snapshot,
                                     data_epoch_snapshot,
                                     indexed_snapshot,
                                     record_count_snapshot);
    Memory::free(indexed_snapshot);

    if (summary_json_build_.overflowed()) {
        Log::logf(CAT_REPORT, LOG_WARN,
                  "Summary JSON snapshot allocation failed\n");
        summary_json_build_ =
            "{\"state\":\"error\",\"error\":\"summary_snapshot_alloc\","
            "\"nights\":[]}";
        ok = false;
    }
    summary_json_snapshot_.swap(summary_json_build_);
    return ok;
}

void ReportManager::build_summary_json(LargeTextBuffer &json) const {
    if (!summary_json_snapshot_.length()) {
        json = "{\"state\":\"idle\",\"error\":\"summary_snapshot_missing\","
               "\"nights\":[]}";
        return;
    }
    json.clear();
    json.append(summary_json_snapshot_.c_str(), summary_json_snapshot_.length());
}

uint32_t ReportManager::night_epoch_for_unlocked(uint64_t night_start_ms) const {
    if (!night_epochs_) return 0;
    for (size_t i = 0; i < night_epoch_count_; ++i) {
        if (night_epochs_[i].night_start_ms == night_start_ms) {
            return night_epochs_[i].epoch;
        }
    }
    return 0;
}

void ReportManager::format_night_etag_unlocked(
    const ReportSummaryRecord &rec,
    uint64_t source_signature,
    char *out,
    size_t out_size) const {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%llu-%lu-%lu-%08lx%08lx-%lu-r%lu",
             static_cast<unsigned long long>(rec.start_ms),
             static_cast<unsigned long>(rec.duration_min),
             static_cast<unsigned long>(rec.session_interval_count),
             static_cast<unsigned long>(source_signature >> 32),
             static_cast<unsigned long>(source_signature & 0xffffffffULL),
             static_cast<unsigned long>(night_epoch_for_unlocked(rec.start_ms)),
             static_cast<unsigned long>(REPORT_RESULT_ETAG_VERSION));
}

bool ReportManager::night_etag(size_t therapy_index, char *out,
                               size_t out_size) const {
    ScopedIndexedNight night("night_etag_index");
    if (!night ||
        !indexed_night_by_therapy_index(therapy_index, night.get())) {
        return false;
    }
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    format_night_etag_unlocked(night->summary,
                               night->source_signature,
                               out,
                               out_size);
    give_summary_lock();
    return true;
}

static const char *result_state_name_for(ReportResultState state) {
    switch (state) {
        case ReportResultState::Idle: return "idle";
        case ReportResultState::Preparing: return "preparing";
        case ReportResultState::Ready: return "ready";
        case ReportResultState::Incomplete: return "incomplete";
        case ReportResultState::Partial: return "partial";
        case ReportResultState::Error: return "error";
    }
    return "unknown";
}

void ReportManager::build_result_json_from(
    const ReportResultStatus &result_status_,
    const ReportIndexedNight &indexed_night,
    const PlotRange *ranges,
    size_t range_count,
    const ReportResultStream *result_streams_,
    size_t result_stream_count_,
    const ReportCacheFetchStatus &cache_status_,
    LargeTextBuffer &json) const {
    const ReportSummaryRecord &result_night_ = indexed_night.summary;
    json.clear();
    json += "{";
    json_add_string(json, "state", result_state_name_for(result_status_.state),
                    false);
    json_add_string(json, "error", result_status_.error.c_str());
    json_add_int(json, "therapy_index",
                 static_cast<long>(result_status_.therapy_index));
    json_add_uint64(json, "start", result_status_.night_start_ms);
    json_add_uint64(json, "end", result_status_.night_end_ms);
    json_add_int(json, "duration_min",
                 static_cast<long>(result_status_.duration_min));
    json_add_int(json, "missing_required",
                 static_cast<long>(result_status_.missing_required));
    json_add_int(json, "missing_streams",
                 static_cast<long>(result_status_.missing_streams));
    json_add_int(json, "streams",
                 static_cast<long>(result_status_.stream_count));
    json_add_int(json, "chunks",
                 static_cast<long>(result_status_.chunk_count));
    json_add_int(json, "records",
                 static_cast<long>(result_status_.record_count));
    json_add_int(json, "bytes",
                 static_cast<long>(result_status_.payload_bytes));
    json += ",\"cache\":{";
    json_add_bool(json, "active", cache_status_.active, false);
    json_add_int(json, "revision",
                 static_cast<long>(cache_status_.revision));
    json_add_string(json,
                    "source",
                    report_source_spool_type(cache_status_.active_source));
    json_add_int(json,
                 "source_index",
                 static_cast<long>(cache_status_.source_index));
    json_add_int(json,
                 "source_count",
                 static_cast<long>(cache_status_.source_count));
    json_add_int(json,
                 "chunks_written",
                 static_cast<long>(cache_status_.chunks_written));
    json_add_string(json, "error", cache_status_.error.c_str());
    json_add_string(json,
                    "spool_state",
                    spool_client_state_name(cache_status_.spool.state));
    json_add_int(json,
                 "spool_round",
                 static_cast<long>(cache_status_.spool.current_round));
    json_add_int(json,
                 "spool_fragments",
                 static_cast<long>(cache_status_.spool.fragments));
    json_add_int(json,
                 "spool_bytes",
                 static_cast<long>(cache_status_.spool.bytes));
    json_add_int(json,
                 "spool_elapsed_ms",
                 static_cast<long>(cache_status_.spool.elapsed_ms));
    json += "}";
    append_optional_float(json,
                          "ahi",
                          result_status_.event_metrics_valid,
                          result_status_.ahi);
    append_optional_float(json,
                          "oa_index",
                          result_status_.event_metrics_valid,
                          result_status_.oa_index);
    append_optional_float(json,
                          "ca_index",
                          result_status_.event_metrics_valid,
                          result_status_.ca_index);
    append_optional_float(json,
                          "ua_index",
                          result_status_.event_metrics_valid,
                          result_status_.ua_index);
    append_optional_float(json,
                          "hypopnea_index",
                          result_status_.event_metrics_valid,
                          result_status_.hypopnea_index);
    append_optional_float(json,
                          "arousal_index",
                          result_status_.event_metrics_valid,
                          result_status_.arousal_index);
    json_add_int(json, "oa_count",
                 static_cast<long>(result_status_.oa_count));
    json_add_int(json, "ca_count",
                 static_cast<long>(result_status_.ca_count));
    json_add_int(json, "ua_count",
                 static_cast<long>(result_status_.ua_count));
    json_add_int(json, "hypopnea_count",
                 static_cast<long>(result_status_.hypopnea_count));
    json_add_int(json, "arousal_count",
                 static_cast<long>(result_status_.arousal_count));
    json_add_bool(json, "events_available", result_status_.events_available);
    json += ",\"sessions\":[";
    const bool have_index_ranges = indexed_night.range_count > 0;
    const bool have_resolved_ranges = !have_index_ranges &&
                                      ranges &&
                                      range_count > 0;
    const size_t session_json_count =
        have_index_ranges
            ? std::min(indexed_night.range_count,
                       static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX))
            : (have_resolved_ranges
                   ? range_count
                   : result_night_.session_interval_count);
    for (size_t i = 0; i < session_json_count; ++i) {
        uint64_t session_start = 0;
        uint64_t session_end = 0;
        uint32_t duration_min = 0;
        if (have_index_ranges) {
            const ReportSessionRange &range = indexed_night.ranges[i];
            session_start = static_cast<uint64_t>(range.start_ms);
            session_end = static_cast<uint64_t>(range.end_ms);
            duration_min =
                report_ceil_duration_min(range.start_ms, range.end_ms);
        } else if (have_resolved_ranges) {
            const PlotRange &range = ranges[i];
            session_start = static_cast<uint64_t>(range.start_ms);
            session_end = static_cast<uint64_t>(range.end_ms);
            duration_min =
                report_ceil_duration_min(range.start_ms, range.end_ms);
        } else {
            const ReportSummarySession &session =
                result_night_.sessions[i];
            session_start = session.start_ms;
            session_end = session.start_ms +
                          static_cast<uint64_t>(session.duration_min) *
                              60000ULL;
            duration_min = session.duration_min;
        }
        if (i) json += ',';
        json += "{";
        json_add_uint64(json, "start", session_start, false);
        json_add_uint64(json, "end", session_end);
        json_add_int(json, "duration_min",
                     static_cast<long>(duration_min));
        json += "}";
    }
    json += "],\"stream_details\":[";
    for (size_t i = 0; i < result_stream_count_; ++i) {
        const ReportResultStream &stream = result_streams_[i];
        if (i) json += ',';
        json += "{";
        json_add_string(json,
                        "kind",
                        ReportStore::kind_name(stream.kind),
                        false);
        json_add_string(json,
                        "source",
                        report_source_spool_type(stream.source));
        json_add_string(json, "name", stream.name ? stream.name : "");
        json_add_bool(json, "required", stream.required);
        json_add_bool(json, "complete", stream.complete);
        const char *provider_name = "none";
        if (stream.has_edf_segment && stream.has_spool_segment) {
            provider_name = "mixed";
        } else if (stream.has_edf_segment) {
            provider_name = "edf";
        } else if (stream.has_spool_segment) {
            provider_name = "spool";
        }
        json_add_string(json, "provider", provider_name);
        json_add_bool(json, "has_edf", stream.has_edf_segment);
        json_add_bool(json, "has_spool", stream.has_spool_segment);
        json_add_int(json, "chunks",
                     static_cast<long>(stream.chunk_count));
        json_add_int(json, "records",
                     static_cast<long>(stream.record_count));
        json_add_int(json, "bytes",
                     static_cast<long>(stream.payload_bytes));
        // low_res: this series fell back to a lower-resolution source than the
        // signal prefers (high-res aged out -> 1-minute trend); the UI badges it.
        bool low_res = false;
        if (stream.kind == ReportStoreChunkKind::Series) {
            size_t signal_def_count = 0;
            const ReportSignalDef *signal_defs =
                report_signal_defs(signal_def_count);
            for (size_t s = 0; s < signal_def_count; ++s) {
                if (signal_defs[s].id == stream.signal) {
                    low_res = stream.source != signal_defs[s].preferred_source;
                    break;
                }
            }
        }
        json_add_bool(json, "low_res", low_res);
        json += "}";
    }
    json += "]}";
}

bool ReportManager::publish_result_to_slot(bool cache_plot) {
    if (!ensure_result_slots()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=slot_alloc_failed "
                  "index=%lu night=%llu state=%s error=%s\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }
    if (!result_state_materialized_slot_allowed(result_status_.state)) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result publish skipped reason=state index=%lu night=%llu "
                  "state=%s error=%s\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }
    if (!result_etag_[0] || result_indexed_night_.summary.start_ms == 0) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped reason=%s index=%lu night=%llu "
                  "state=%s error=%s\n",
                  !result_etag_[0] ? "missing_etag" : "missing_night",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long long>(
                      result_indexed_night_.summary.start_ms),
                  result_state_name(),
                  result_status_.error.length() ? result_status_.error.c_str()
                                                : "--");
        return false;
    }

    // Snapshot the plot bytes outside the slot lock (the copy is the heavy
    // part); a shared_ptr lets a GET stream it even after the blob is evicted.
    std::shared_ptr<ReportSpoolBuffer> plot;
    if (result_plot_bin_.size() > 0) {
        plot = std::make_shared<ReportSpoolBuffer>();
        plot->set_max_size(result_plot_bin_.size());
        if (!plot->reserve_capacity(result_plot_bin_.size()) ||
            !plot->append(result_plot_bin_.data(), result_plot_bin_.size())) {
            plot.reset();
        }
    }
    if (result_plot_bin_.size() > 0 && !plot) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result publish skipped: plot snapshot failed "
                  "index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        return false;
    }
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            result_slots_[i].night_start_ms ==
                result_indexed_night_.summary.start_ms) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid) {
                pick = i;
                break;
            }
            if (result_slots_[i].last_used < result_slots_[pick].last_used) {
                pick = i;
            }
        }
    }
    MaterializedResult &slot = result_slots_[pick];
    slot.valid = true;
    slot.night_start_ms = result_indexed_night_.summary.start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", result_etag_);
    slot.status = result_status_;
    slot.night = result_indexed_night_;
    slot.range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        slot.ranges[i] = (i < slot.range_count) ? result_ranges_[i]
                                                : PlotRange{};
    }
    slot.stream_count = result_stream_count_;
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        slot.streams[i] = (i < result_stream_count_) ? result_streams_[i]
                                                     : ReportResultStream{};
    }
    slot.chunk_count =
        std::min(static_cast<size_t>(result_status_.chunk_count),
                 static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        slot.chunks[i] = (result_chunks_ && i < slot.chunk_count)
                             ? result_chunks_[i]
                             : ReportResultChunk{};
    }
    slot.edf_session_count =
        std::min(result_edf_session_count_,
                 static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_EDF_SESSION_MAX; ++i) {
        slot.edf_sessions[i] =
            (result_edf_sessions_ && i < slot.edf_session_count)
                ? result_edf_sessions_[i]
                : EdfReportSessionDescriptor{};
    }
    slot.plot = plot;
    slot.last_used = ++result_slot_tick_;
    update_materialized_status_locked();
    xSemaphoreGive(result_slots_lock_);
    if (cache_plot && plot) {
        LargeTextBuffer result_json_text;
        result_json_text.reserve(8192);
        const ReportCacheFetchStatus inactive_cache{};
        build_result_json_from(result_status_,
                               result_indexed_night_,
                               result_ranges_,
                               result_range_count_,
                               result_streams_,
                               result_stream_count_,
                               inactive_cache,
                               result_json_text);
        std::shared_ptr<ReportSpoolBuffer> result_json;
        if (!result_json_text.overflowed() && result_json_text.length() > 0) {
            result_json = std::make_shared<ReportSpoolBuffer>();
            if (result_json) {
                result_json->set_max_size(result_json_text.length());
                if (!result_json->reserve_capacity(result_json_text.length()) ||
                    !result_json->append(
                        reinterpret_cast<const uint8_t *>(
                            result_json_text.c_str()),
                        result_json_text.length())) {
                    result_json.reset();
                }
            }
        }
        if (result_json) {
            enqueue_result_cache_write(result_indexed_night_,
                                       result_etag_,
                                       result_json,
                                       plot);
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result cache write skipped: result JSON snapshot "
                      "failed index=%lu night=%llu\n",
                      static_cast<unsigned long>(
                          result_status_.therapy_index),
                      static_cast<unsigned long long>(
                          result_indexed_night_.summary.start_ms));
        }
    }
    return true;
}

void ReportManager::update_materialized_status_locked() {
    uint32_t slots = 0;
    uint32_t plot_slots = 0;
    if (result_slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid) continue;
            slots++;
            if (result_slots_[i].plot && result_slots_[i].plot->size() > 0) {
                plot_slots++;
            }
        }
    }
    result_status_.materialized_slots = slots;
    result_status_.materialized_plot_slots = plot_slots;
}

void ReportManager::clear_materialized_slot_locked(MaterializedResult &slot) {
    slot.valid = false;
    slot.night_start_ms = 0;
    slot.etag[0] = '\0';
    slot.status = ReportResultStatus{};
    memset(&slot.night, 0, sizeof(slot.night));
    memset(slot.ranges, 0, sizeof(slot.ranges));
    slot.range_count = 0;
    memset(slot.streams, 0, sizeof(slot.streams));
    slot.stream_count = 0;
    memset(slot.chunks, 0, sizeof(slot.chunks));
    slot.chunk_count = 0;
    memset(slot.edf_sessions, 0, sizeof(slot.edf_sessions));
    slot.edf_session_count = 0;
    slot.plot.reset();
}

void ReportManager::clear_range_plot_locked(uint64_t night_start_ms, bool all) {
    const bool matches_request =
        range_req_active_ &&
        (all || range_req_night_start_ms_ == night_start_ms);
    const bool matches_plot =
        range_plot_bytes_ &&
        (all || range_plot_night_start_ms_ == night_start_ms);
    if (!matches_request && !matches_plot) return;

    if (matches_request) {
        range_req_active_ = false;
        range_req_index_ = 0;
        range_req_night_start_ms_ = 0;
        range_req_from_ = 0;
        range_req_to_ = 0;
    }
    if (matches_plot) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }
}

void ReportManager::invalidate_materialized_locked(uint64_t night_start_ms,
                                                   bool all) {
    if (!result_slots_) return;
    bool invalidated = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            (all || result_slots_[i].night_start_ms == night_start_ms)) {
            clear_materialized_slot_locked(result_slots_[i]);
            invalidated = true;
        }
    }
    if (invalidated) {
        clear_range_plot_locked(night_start_ms, all);
    }
    update_materialized_status_locked();
}

void ReportManager::invalidate_materialized(uint64_t night_start_ms, bool all) {
    if (!result_slots_lock_) return;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    invalidate_materialized_locked(night_start_ms, all);
    xSemaphoreGive(result_slots_lock_);
}

ReportManager::ResultRead ReportManager::read_result(
    size_t therapy_index, const char *if_none_match, char *etag_out,
    size_t etag_out_size, LargeTextBuffer &json_out) {
    ScopedIndexedNight indexed_night("read_result_index");
    if (!indexed_night ||
        !indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "not_found");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::NotFound;
    }
    return read_result_for_indexed_night(therapy_index,
                                         indexed_night.get(),
                                         if_none_match,
                                         etag_out,
                                         etag_out_size,
                                         json_out);
}

ReportManager::ResultRead ReportManager::read_result_by_start(
    uint64_t night_start_ms, const char *if_none_match, char *etag_out,
    size_t etag_out_size, LargeTextBuffer &json_out) {
    ScopedIndexedNight indexed_night("read_result_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms,
                                indexed_night.get(),
                                &therapy_index)) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "not_found");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::NotFound;
    }
    return read_result_for_indexed_night(therapy_index,
                                         indexed_night.get(),
                                         if_none_match,
                                         etag_out,
                                         etag_out_size,
                                         json_out);
}

ReportManager::ResultRead ReportManager::read_result_for_indexed_night(
    size_t therapy_index, const ReportIndexedNight &indexed_night,
    const char *if_none_match, char *etag_out, size_t etag_out_size,
    LargeTextBuffer &json_out) {
    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "summary_busy");
            xSemaphoreGive(build_queue_lock_);
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::Busy;
    }
    format_night_etag_unlocked(indexed_night.summary,
                               indexed_night.source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }

    if (indexed_night.edf_catalog_pending) {
        if (load_result_json_cache_for_night(indexed_night,
                                             current_etag,
                                             json_out)) {
            const bool not_modified = if_none_match && if_none_match[0] &&
                                      strcmp(if_none_match, current_etag) == 0;
            if (build_queue_lock_ &&
                xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) ==
                    pdTRUE) {
                copy_cstr(build_queue_last_read_,
                          sizeof(build_queue_last_read_),
                          not_modified
                              ? "not_modified_sd_pending"
                              : "ready_sd_pending");
                xSemaphoreGive(build_queue_lock_);
            }
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result cache pending-catalog hit index=%lu "
                      "night=%llu etag=%s bytes=%lu\n",
                      static_cast<unsigned long>(therapy_index),
                      static_cast<unsigned long long>(
                          indexed_night.summary.start_ms),
                      current_etag,
                      static_cast<unsigned long>(json_out.length()));
            if (not_modified) {
                json_out.clear();
                return ResultRead::NotModified;
            }
            return ResultRead::Ready;
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      "edf_catalog");
            xSemaphoreGive(build_queue_lock_);
        }
        return ResultRead::Building;
    }

    ResultRead slot_read = ResultRead::NotFound;
    bool have_slot_read = false;
    if (result_slots_ && result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (result_slots_[i].valid &&
                result_slots_[i].night_start_ms ==
                    indexed_night.summary.start_ms) {
                if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    clear_range_plot_locked(indexed_night.summary.start_ms,
                                            false);
                    update_materialized_status_locked();
                    continue;
                }
                if (!result_state_materialized_slot_allowed(
                        result_slots_[i].status.state)) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    clear_range_plot_locked(indexed_night.summary.start_ms,
                                            false);
                    update_materialized_status_locked();
                    continue;
                }
                result_slots_[i].last_used = ++result_slot_tick_;
                snprintf(etag, sizeof(etag), "%s", result_slots_[i].etag);
                const ReportResultStatus &status = result_slots_[i].status;
                const bool cacheable =
                    result_state_http_cacheable(status.state);
                if (etag_out && etag_out_size) {
                    snprintf(etag_out, etag_out_size, "%s",
                             cacheable ? etag : "");
                }
                if (cacheable && if_none_match && if_none_match[0] &&
                    strcmp(if_none_match, etag) == 0) {
                    slot_read = ResultRead::NotModified;
                } else {
                    const ReportCacheFetchStatus inactive{};
                    build_result_json_from(
                        status,
                        result_slots_[i].night,
                        result_slots_[i].ranges,
                        std::min(result_slots_[i].range_count,
                                 static_cast<size_t>(
                                     AC_REPORT_SUMMARY_SESSION_MAX)),
                        result_slots_[i].streams,
                        std::min(result_slots_[i].stream_count,
                                 static_cast<size_t>(
                                     AC_REPORT_RESULT_STREAM_MAX)),
                        inactive,
                        json_out);
                    slot_read = ResultRead::Ready;
                }
                have_slot_read = true;
                break;
            }
        }
        xSemaphoreGive(result_slots_lock_);
    }
    if (have_slot_read) {
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      slot_read == ResultRead::NotModified ? "not_modified"
                                                           : "ready");
            xSemaphoreGive(build_queue_lock_);
        }
        return slot_read;
    }

    if (load_result_json_cache_for_night(indexed_night,
                                         current_etag,
                                         json_out)) {
        const bool not_modified = if_none_match && if_none_match[0] &&
                                  strcmp(if_none_match, current_etag) == 0;
        if (etag_out && etag_out_size) {
            snprintf(etag_out, etag_out_size, "%s", current_etag);
        }
        if (build_queue_lock_ &&
            xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
            copy_cstr(build_queue_last_read_,
                      sizeof(build_queue_last_read_),
                      not_modified ? "not_modified_sd" : "ready_sd");
            xSemaphoreGive(build_queue_lock_);
        }
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result cache direct hit index=%lu night=%llu bytes=%lu\n",
                  static_cast<unsigned long>(therapy_index),
                  static_cast<unsigned long long>(
                      indexed_night.summary.start_ms),
                  static_cast<unsigned long>(json_out.length()));
        if (not_modified) {
            json_out.clear();
            return ResultRead::NotModified;
        }
        return ResultRead::Ready;
    }
    if (etag_out && etag_out_size) etag_out[0] = '\0';

    const BuildQueueResult queued =
        enqueue_build(indexed_night.summary.start_ms,
                      therapy_index,
                      false);
    if (build_queue_lock_ &&
        xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) == pdTRUE) {
        copy_cstr(build_queue_last_read_,
                  sizeof(build_queue_last_read_),
                  build_queue_result_name(static_cast<uint8_t>(queued)));
        xSemaphoreGive(build_queue_lock_);
    }
    switch (queued) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return ResultRead::Building;
        case BuildQueueResult::Full:
            return ResultRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return ResultRead::Unavailable;
    }
}

ReportManager::PlotRead ReportManager::read_plot(
    size_t therapy_index, const char *version,
    char *etag_out, size_t etag_out_size,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    ScopedIndexedNight indexed_night("read_plot_index");
    if (!indexed_night) return PlotRead::Unavailable;
    size_t resolved_therapy_index = therapy_index;
    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (etag_out && etag_out_size) etag_out[0] = '\0';
    uint64_t version_night_start_ms = 0;
    const bool have_version_start =
        parse_report_night_start_from_etag(version, version_night_start_ms);
    const bool found_night = have_version_start
        ? indexed_night_by_start(version_night_start_ms,
                                 indexed_night.get(),
                                 &resolved_therapy_index)
        : indexed_night_by_therapy_index(therapy_index, indexed_night.get());
    if (!found_night) {
        return PlotRead::NotFound;
    }
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        return PlotRead::Busy;
    }
    format_night_etag_unlocked(indexed_night->summary,
                               indexed_night->source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return PlotRead::Stale;
    }
    if (indexed_night->edf_catalog_pending) {
        auto cached_plot = std::make_shared<ReportSpoolBuffer>();
        if (cached_plot &&
            load_result_plot_cache_for_night(indexed_night.get(),
                                             current_etag,
                                             *cached_plot)) {
            out = cached_plot;
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Plot cache pending-catalog hit index=%lu "
                      "night=%llu bytes=%lu\n",
                      static_cast<unsigned long>(resolved_therapy_index),
                      static_cast<unsigned long long>(
                          indexed_night->summary.start_ms),
                      static_cast<unsigned long>(cached_plot->size()));
            return PlotRead::Ready;
        }
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return PlotRead::Building;
    }
    bool matching_result_without_plot = false;
    if (result_slots_ && result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid ||
                result_slots_[i].night_start_ms !=
                    indexed_night->summary.start_ms) {
                continue;
            }
            if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                clear_materialized_slot_locked(result_slots_[i]);
                update_materialized_status_locked();
                continue;
            }
            result_slots_[i].last_used = ++result_slot_tick_;
            if (result_slots_[i].status.state == ReportResultState::Error) {
                xSemaphoreGive(result_slots_lock_);
                return PlotRead::Error;
            }
            if (result_slots_[i].plot) {
                out = result_slots_[i].plot;
                xSemaphoreGive(result_slots_lock_);
                return PlotRead::Ready;
            }
            matching_result_without_plot = true;
            break;
        }
        xSemaphoreGive(result_slots_lock_);
    }
    auto cached_plot = std::make_shared<ReportSpoolBuffer>();
    if (cached_plot &&
        load_result_plot_cache_for_night(indexed_night.get(),
                                         current_etag,
                                         *cached_plot)) {
        if (result_slots_ && result_slots_lock_) {
            xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
            for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
                if (!result_slots_[i].valid ||
                    result_slots_[i].night_start_ms !=
                        indexed_night->summary.start_ms) {
                    continue;
                }
                if (strcmp(result_slots_[i].etag, current_etag) != 0) {
                    clear_materialized_slot_locked(result_slots_[i]);
                    update_materialized_status_locked();
                    continue;
                }
                result_slots_[i].plot = cached_plot;
                result_slots_[i].last_used = ++result_slot_tick_;
                update_materialized_status_locked();
                break;
            }
            xSemaphoreGive(result_slots_lock_);
        }
        out = cached_plot;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Plot cache direct hit index=%lu night=%llu bytes=%lu\n",
                  static_cast<unsigned long>(resolved_therapy_index),
                  static_cast<unsigned long long>(
                      indexed_night->summary.start_ms),
                  static_cast<unsigned long>(cached_plot->size()));
        return PlotRead::Ready;
    }
    if (matching_result_without_plot &&
        plot_build_night_start_ms_.load() == indexed_night->summary.start_ms) {
        return PlotRead::Building;
    }
    switch (enqueue_build(indexed_night->summary.start_ms,
                          resolved_therapy_index,
                          false)) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return PlotRead::Building;
        case BuildQueueResult::Full:
            return PlotRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return PlotRead::Unavailable;
    }
}

void ReportManager::build_result_chunks_json(LargeTextBuffer &json,
                                             size_t offset,
                                             size_t limit) const {
    if (limit > 128) limit = 128;
    const size_t total = result_status_.chunk_count;
    if (offset > total) offset = total;
    const size_t end = std::min(total, offset + limit);

    json.clear();
    json += "{";
    json_add_string(json, "state", result_state_name(), false);
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "limit", static_cast<long>(limit));
    json_add_int(json, "total", static_cast<long>(total));
    json_add_bool(json, "more", end < total);
    json += ",\"chunks\":[";
    for (size_t i = offset; i < end; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (i != offset) json += ',';
        json += "{";
        json_add_int(json, "id", static_cast<long>(i), false);
        json_add_string(json,
                        "kind",
                        ReportStore::kind_name(chunk.kind));
        json_add_string(json,
                        "source",
                        report_source_spool_type(chunk.source));
        json_add_string(json,
                        "provider",
                        report_provider_id_name(chunk.provider_ref.provider));
        json_add_string(json, "name", chunk.name ? chunk.name : "");
        json_add_int(json,
                     "stream",
                     static_cast<long>(chunk.stream_index));
        json_add_uint64(json,
                        "start",
                        static_cast<uint64_t>(chunk.start_ms));
        json_add_uint64(json,
                        "end",
                        static_cast<uint64_t>(chunk.end_ms));
        json_add_int(json, "schema",
                     static_cast<long>(chunk.payload_schema));
        json_add_int(json, "records",
                     static_cast<long>(chunk.record_count));
        json_add_int(json, "bytes",
                     static_cast<long>(chunk.payload_len));
        json += "}";
    }
    json += "]}";
}

bool ReportManager::night_coverage(uint64_t night_start_ms,
                                   ReportNightCoverageStatus &out) const {
    out = {};
    ScopedIndexedNight indexed_night("night_coverage_index");
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms, indexed_night.get())) {
        return false;
    }
    const ReportIndexedNight &indexed = indexed_night.get();
    const ReportSummaryRecord &night = indexed.summary;

    out.found = true;
    out.start_ms = night.start_ms;
    out.end_ms = night.end_ms;
    out.duration_min = indexed_night_display_duration_min(indexed);

    int64_t span_start_ms = 0;
    int64_t span_end_ms = 0;
    if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
        return true;
    }

    ScopedReportResolveContext resolve("night_coverage_resolver");
    if (!resolve) {
        return false;
    }

    bool pending = false;
    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night,
                                        span_start_ms,
                                        span_end_ms,
                                        resolve.sessions(),
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        &pending) ||
        pending) {
        return false;
    }

    EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    if (!resolver.build_plan(indexed,
                             span_start_ms,
                             span_end_ms,
                             resolve.plan())) {
        return false;
    }

    const ReportResolvedPlan &plan = resolve.plan();
    out.missing_required = plan.missing_required;
    for (size_t i = 0; i < plan.stream_count; ++i) {
        const ReportResolvedStream &stream = plan.streams[i];
        if (!cache_source_supported(stream.selected_source)) continue;
        ReportNightSourceCoverage *entry = nullptr;
        for (size_t existing = 0; existing < out.source_count; ++existing) {
            if (out.sources[existing].source == stream.selected_source) {
                entry = &out.sources[existing];
                break;
            }
        }
        if (!entry) {
            if (out.source_count >= AC_REPORT_NIGHT_SOURCE_MAX) break;
            entry = &out.sources[out.source_count++];
            entry->source = stream.selected_source;
            entry->complete = true;
        }
        entry->required = entry->required || stream.required;
        if (stream.required && !stream.complete) entry->complete = false;
    }
    return true;
}

bool ReportManager::next_night_needing_cache(
    uint64_t &night_start_ms_out) const {
    const uint32_t now = millis();
    ReportIndexedNight *nights =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!nights) {
        log_report_alloc_failed(
            "prefetch_night_index",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }
    size_t count = 0;
    if (!build_indexed_nights(nights,
                              AC_REPORT_SUMMARY_RECORD_MAX,
                              count)) {
        Memory::free(nights);
        return false;
    }
    ScopedReportResolveContext resolve("prefetch_resolver");
    if (!resolve) {
        Memory::free(nights);
        return false;
    }

    // Oldest-first: the spool is open-ended (fromDateTime -> now), so fetching
    // the OLDEST night with a gap streams every source from there forward and
    // backfills all newer nights in a single sweep (deduped on write)
    for (size_t i = 0; i < count; ++i) {
        const ReportIndexedNight &indexed = nights[i];
        const ReportSummaryRecord &record = indexed.summary;
        if (!record.valid || !record.duration_min) continue;
        if (prefetch_in_cooldown(record.start_ms, now)) continue;
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (!indexed_night_data_span(indexed, span_start_ms, span_end_ms)) {
            continue;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        memset(resolve.sessions(),
               0,
               AC_REPORT_EDF_SESSION_MAX *
                   sizeof(EdfReportSessionDescriptor));
        if (!collect_edf_sessions_for_night(record,
                                            span_start_ms,
                                            span_end_ms,
                                            resolve.sessions(),
                                            AC_REPORT_EDF_SESSION_MAX,
                                            session_count,
                                            &edf_pending)) {
            Memory::free(nights);
            return false;
        }
        if (edf_pending) continue;

        EdfReportDataProvider edf_provider(resolve.sessions(), session_count);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());
        if (!resolver.build_plan(indexed,
                                 span_start_ms,
                                 span_end_ms,
                                 resolve.plan())) {
            Memory::free(nights);
            return false;
        }
        const ReportResolvedPlan &plan = resolve.plan();
        for (size_t segment_index = 0; segment_index < plan.segment_count;
             ++segment_index) {
            const ReportResolvedSegment &segment =
                plan.segments[segment_index];
            if (segment.provider == ReportResolvedProvider::Spool &&
                !segment.complete &&
                segment.required &&
                cache_source_supported(segment.source)) {
                night_start_ms_out = record.start_ms;
                Memory::free(nights);
                return true;
            }
        }
    }
    Memory::free(nights);
    return false;
}

bool ReportManager::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    if (!callback) return false;

    ReportIndexedNight *snapshot =
        static_cast<ReportIndexedNight *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "summary_night_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportIndexedNight));
        return false;
    }

    size_t count = 0;
    if (!build_indexed_nights(snapshot,
                              AC_REPORT_SUMMARY_RECORD_MAX,
                              count)) {
        Memory::free(snapshot);
        return false;
    }

    bool any = false;
    size_t therapy_index = 0;
    for (size_t i = count; i > 0; --i) {
        const size_t summary_index = i - 1;
        const ReportIndexedNight &indexed = snapshot[summary_index];
        if (!indexed_night_visible_in_summary(indexed)) continue;
        ReportSummaryRecord record = indexed.summary;
        record.duration_min = indexed_night_display_duration_min(indexed);

        ReportSummaryNight night;
        night.summary_index = summary_index;
        night.therapy_index = therapy_index++;
        night.record = record;
        any = true;
        if (!callback(context, night)) break;
    }
    Memory::free(snapshot);
    return any;
}

bool ReportManager::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    ScopedIndexedNight night("summary_night_index");
    if (!night ||
        !indexed_night_by_therapy_index(therapy_index, night.get())) {
        return false;
    }
    out = night->summary;
    out.duration_min = indexed_night_display_duration_min(night.get());
    return true;
}

bool ReportManager::latest_summary_night(ReportSummaryRecord &out) const {
    return summary_night_by_therapy_index(0, out);
}

bool ReportManager::night_coverage_by_therapy_index(
    size_t therapy_index,
    ReportNightCoverageStatus &out) const {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return night_coverage(night.start_ms, out);
}

bool ReportManager::latest_night_coverage(
    ReportNightCoverageStatus &out) const {
    return night_coverage_by_therapy_index(0, out);
}

bool ReportManager::request_night_cache(uint64_t night_start_ms, bool force) {
    if (summary_fetch_active_ || cache_fetch_active_ || range_build_active_) {
        return false;
    }
    ScopedIndexedNight indexed_night("request_night_cache_index");
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms, indexed_night.get()) ||
        !indexed_night_visible_in_summary(indexed_night.get())) {
        return false;
    }
    if (!build_cache_plan(indexed_night.get(), force, false)) return false;
    if (cache_source_count_ == 0) {
        finish_cache_fetch();
        return true;
    }
    return start_next_cache_source();
}

bool ReportManager::request_night_cache_by_therapy_index(
    size_t therapy_index,
    bool force) {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return request_night_cache(night.start_ms, force);
}

bool ReportManager::request_latest_night_cache(bool force) {
    return request_night_cache_by_therapy_index(0, force);
}

bool ReportManager::prefetch_in_cooldown(uint64_t night_ms,
                                         uint32_t now_ms) const {
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    bool in_cooldown = false;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms &&
            prefetch_skip_[i].until_ms != 0 &&
            static_cast<int32_t>(now_ms - prefetch_skip_[i].until_ms) < 0) {
            in_cooldown = true;
            break;
        }
    }
    if (locked) xSemaphoreGive(prefetch_lock_);
    return in_cooldown;
}

void ReportManager::clear_sparse_event_empty_markers(
    uint64_t night_start_ms) {
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        if (night_start_ms == 0 ||
            sparse_event_empty_[i].night_ms == night_start_ms) {
            sparse_event_empty_[i] = SparseEventEmptyMarker{};
        }
    }
    if (locked) xSemaphoreGive(prefetch_lock_);
}

void ReportManager::note_sparse_event_confirmed_empty(
    const ReportSummaryRecord &night,
    const ReportSourceDef &source) {
    char night_key[48];
    format_stable_night_key(night, night_key, sizeof(night_key));
    if (!night_key[0]) return;

    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    size_t pick = 0;
    bool found = false;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        const SparseEventEmptyMarker &marker = sparse_event_empty_[i];
        if ((marker.source == source.id && marker.night_ms == night.start_ms) ||
            marker.night_ms == 0) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) pick = 0;

    SparseEventEmptyMarker &marker = sparse_event_empty_[pick];
    marker.source = source.id;
    marker.night_ms = night.start_ms;
    snprintf(marker.night_key,
             sizeof(marker.night_key),
             "%s",
             night_key);
    if (locked) xSemaphoreGive(prefetch_lock_);
}

bool ReportManager::sparse_event_confirmed_empty(
    const ReportSummaryRecord &night,
    const ReportSourceDef &source) const {
    char night_key[48];
    format_stable_night_key(night, night_key, sizeof(night_key));
    if (!night_key[0]) return false;
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    bool confirmed = false;
    const size_t marker_count =
        sizeof(sparse_event_empty_) / sizeof(sparse_event_empty_[0]);
    for (size_t i = 0; i < marker_count; ++i) {
        const SparseEventEmptyMarker &marker = sparse_event_empty_[i];
        if (marker.source == source.id &&
            marker.night_ms == night.start_ms &&
            marker.night_key[0] != '\0' &&
            strcmp(night_key, marker.night_key) == 0) {
            confirmed = true;
            break;
        }
    }
    if (locked) xSemaphoreGive(prefetch_lock_);
    return confirmed;
}

void ReportManager::prefetch_note_failure(uint64_t night_ms) {
    const uint32_t now_ms = millis();
    uint32_t until = now_ms + AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS;
    if (until == 0) until = 1;
    const bool locked = prefetch_lock_ &&
                        xSemaphoreTake(prefetch_lock_, portMAX_DELAY) ==
                            pdTRUE;
    size_t pick = 0;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms ||
            prefetch_skip_[i].night_ms == 0) {
            pick = i;
            break;
        }
        if (deadline_before(prefetch_skip_[i].until_ms,
                            prefetch_skip_[pick].until_ms,
                            now_ms)) {
            pick = i;
        }
    }
    prefetch_skip_[pick].night_ms = night_ms;
    prefetch_skip_[pick].until_ms = until;
    if (locked) xSemaphoreGive(prefetch_lock_);
}

void ReportManager::set_prefetch_phase(PrefetchPhase phase,
                                       uint64_t night_ms,
                                       bool inc_completed,
                                       bool inc_failed) {
    if (!prefetch_lock_) return;
    uint64_t failed_night = 0;
    uint32_t failed_total = 0;
    char failed_source[32] = {};
    char failed_error[64] = {};
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const uint64_t phase_night =
        night_ms != 0 ? night_ms : prefetch_active_night_;
    prefetch_phase_ = phase;
    prefetch_active_night_ = night_ms;
    if (night_ms != 0) prefetch_last_night_ = night_ms;
    if (inc_completed) prefetch_completed_++;
    if (inc_failed) {
        prefetch_failed_++;
        prefetch_last_failed_night_ = phase_night;
        if (phase_night != 0) prefetch_last_night_ = phase_night;
        const char *source =
            cache_status_.source_count
                ? report_source_spool_type(cache_status_.active_source)
                : "";
        snprintf(prefetch_last_source_, sizeof(prefetch_last_source_), "%s",
                 source ? source : "");
        snprintf(prefetch_last_error_, sizeof(prefetch_last_error_), "%s",
                 cache_status_.error.length() ? cache_status_.error.c_str()
                                              : "");
        failed_night = phase_night;
        failed_total = prefetch_failed_;
        snprintf(failed_source, sizeof(failed_source), "%s",
                 prefetch_last_source_);
        snprintf(failed_error, sizeof(failed_error), "%s",
                 prefetch_last_error_);
    }
    xSemaphoreGive(prefetch_lock_);
    if (inc_failed) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "prefetch failed phase=%s night=%llu source=%s "
                  "error=%s total=%lu\n",
                  prefetch_phase_name(phase),
                  static_cast<unsigned long long>(failed_night),
                  failed_source[0] ? failed_source : "--",
                  failed_error[0] ? failed_error : "--",
                  static_cast<unsigned long>(failed_total));
    }
}

bool ReportManager::prefetch_request_night(uint64_t night_start_ms) {
    if (!prefetch_lock_ || night_start_ms == 0) return false;
    bool accepted = false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Pending;
        prefetch_active_night_ = night_start_ms;
        prefetch_last_night_ = night_start_ms;
        accepted = true;
    }
    xSemaphoreGive(prefetch_lock_);
    return accepted;
}

bool ReportManager::prefetch_request_candidate() {
    if (!prefetch_lock_) return false;
    bool accepted = false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Selecting;
        prefetch_active_night_ = 0;
        accepted = true;
    }
    xSemaphoreGive(prefetch_lock_);
    return accepted;
}

void ReportManager::prefetch_mark_drained() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Selecting &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Drained;
        prefetch_active_night_ = 0;
    }
    xSemaphoreGive(prefetch_lock_);
}

void ReportManager::prefetch_preempt() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    prefetch_preempt_req_ = true;
    xSemaphoreGive(prefetch_lock_);
}

ReportManager::PrefetchSnapshot ReportManager::prefetch_snapshot() const {
    PrefetchSnapshot snap;
    if (!prefetch_lock_) return snap;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    snap.phase = prefetch_phase_;
    snap.night_ms = prefetch_active_night_;
    snap.last_night_ms = prefetch_last_night_;
    snap.last_failed_night_ms = prefetch_last_failed_night_;
    snap.completed = prefetch_completed_;
    snap.failed = prefetch_failed_;
    snprintf(snap.last_source, sizeof(snap.last_source), "%s",
             prefetch_last_source_);
    snprintf(snap.last_error, sizeof(snap.last_error), "%s",
             prefetch_last_error_);
    xSemaphoreGive(prefetch_lock_);
    return snap;
}

ReportManager::BuildQueueSnapshot ReportManager::build_queue_snapshot() const {
    BuildQueueSnapshot snap;
    snap.available = build_queue_lock_ != nullptr;
    if (!build_queue_lock_) return snap;
    if (xSemaphoreTake(build_queue_lock_, 0) != pdTRUE) return snap;
    const uint32_t now_ms = millis();
    snap.lock_ok = true;
    snap.count = build_queue_count_;
    if (build_queue_count_ > 0) {
        const ResultBuildJob &job = build_queue_[build_queue_head_];
        snap.head_night_ms = job.night_start_ms;
        snap.head_therapy_index = job.therapy_index;
        snap.head_refresh = job.refresh;
        snap.head_idle_prebuild = job.idle_prebuild;
        snap.head_age_ms = job.queued_ms ? now_ms - job.queued_ms : 0;
    }
    snap.last_night_ms = build_queue_last_night_ms_;
    snap.last_therapy_index = build_queue_last_therapy_index_;
    snap.enqueue_total = build_queue_enqueue_total_;
    snap.queued_total = build_queue_queued_total_;
    snap.already_total = build_queue_already_total_;
    snap.service_total = build_queue_service_total_;
    snap.last_enqueue_night_ms = build_queue_last_enqueue_night_ms_;
    snap.last_enqueue_therapy_index = build_queue_last_enqueue_therapy_index_;
    copy_cstr(snap.last_read,
              sizeof(snap.last_read),
              build_queue_last_read_);
    copy_cstr(snap.last_enqueue_result,
              sizeof(snap.last_enqueue_result),
              build_queue_last_enqueue_result_);
    copy_cstr(snap.last_service_block,
              sizeof(snap.last_service_block),
              build_queue_last_service_block_);
    copy_cstr(snap.last_outcome,
              sizeof(snap.last_outcome),
              build_queue_last_outcome_);
    copy_cstr(snap.last_state, sizeof(snap.last_state), build_queue_last_state_);
    copy_cstr(snap.last_error, sizeof(snap.last_error), build_queue_last_error_);
    xSemaphoreGive(build_queue_lock_);
    return snap;
}

bool ReportManager::edf_catalog_status(EdfReportCatalogStatus &out,
                                       uint32_t timeout_ms) const {
    if (!edf_catalog_) return false;
    return edf_catalog_->status(out, timeout_ms);
}

bool ReportManager::foreground_busy() const {
    // A queued build is pending work, not foreground ownership. Keeping it out
    // of this gate lets dependency jobs (notably the EDF catalog refresh) run;
    // background_work_active() still reports the queue so export/backfill waits.
    if (summary_fetch_active_ || range_build_active_) {
        return true;
    }
    if (plot_build_active_) return !plot_build_idle_prebuild_;
    if (!cache_fetch_active_) return false;
    // A cache fetch is in flight: it's foreground unless it's the prefetch's own.
    if (!prefetch_lock_) return true;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool prefetch_owned = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);
    return !prefetch_owned;
}

bool ReportManager::background_work_active() const {
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return true;
    }
    if (build_queue_lock_) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
        const bool queued = build_queue_count_ > 0;
        xSemaphoreGive(build_queue_lock_);
        if (queued) return true;
    }
    if (!prefetch_lock_) return false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const PrefetchPhase phase = prefetch_phase_;
    xSemaphoreGive(prefetch_lock_);
    return phase == PrefetchPhase::Selecting ||
           phase == PrefetchPhase::Pending ||
           phase == PrefetchPhase::Fetching ||
           phase == PrefetchPhase::Done;
}

void ReportManager::log_spool_can_pressure(const RpcArbiter &arbiter) {
    const uint32_t alerts =
        arbiter.can_driver().stats().rx_queue_full_alerts;
    if (alerts == observed_spool_rx_queue_full_alerts_) return;
    observed_spool_rx_queue_full_alerts_ = alerts;
    if (!spool_.active()) return;

    const SpoolClientStatus &spool = spool_.status();
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "spool CAN RX pressure source=%s state=%s round=%u "
              "spool_id=%lu round_fragments=%lu round_bytes=%lu "
              "total_fragments=%lu total_bytes=%lu alerts=%lu\n",
              spool.spool_type.c_str(),
              spool_client_state_name(spool.state),
              static_cast<unsigned>(spool.current_round),
              static_cast<unsigned long>(spool.active_spool_id),
              static_cast<unsigned long>(spool.round_fragments),
              static_cast<unsigned long>(spool.round_bytes),
              static_cast<unsigned long>(spool.fragments),
              static_cast<unsigned long>(spool.bytes),
              static_cast<unsigned long>(alerts));
}

void ReportManager::prefetch_yield_to_foreground() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool owns = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);
    if (!owns) return;
    if (cache_fetch_active_) {
        spool_.reset();
        abort_cache_write_fetch();
        cache_fetch_active_ = false;
        cache_status_.active = false;
        cache_status_.revision++;
        cache_status_.error = "preempted_by_user";
    }
    set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "prefetch yielded to foreground prepare\n");
}

bool ReportManager::idle_prebuild_gate_open(const char **reason) const {
    BackgroundWorker *worker = background_worker();
    if (!worker) {
        if (reason) *reason = "no_worker";
        return false;
    }
    return worker->idle_gate_open(reason);
}

bool ReportManager::plot_prebuild_key_matches(
    uint32_t summary_revision,
    bool catalog_present,
    uint8_t catalog_state,
    uint32_t catalog_refresh_id) const {
    return plot_prebuild_key_valid_ &&
           plot_prebuild_summary_revision_ == summary_revision &&
           plot_prebuild_catalog_present_ == catalog_present &&
           plot_prebuild_catalog_state_ == catalog_state &&
           plot_prebuild_catalog_refresh_id_ == catalog_refresh_id;
}

void ReportManager::set_plot_prebuild_key(uint32_t summary_revision,
                                          bool catalog_present,
                                          uint8_t catalog_state,
                                          uint32_t catalog_refresh_id) {
    plot_prebuild_key_valid_ = true;
    plot_prebuild_summary_revision_ = summary_revision;
    plot_prebuild_catalog_present_ = catalog_present;
    plot_prebuild_catalog_state_ = catalog_state;
    plot_prebuild_catalog_refresh_id_ = catalog_refresh_id;
    plot_prebuild_cursor_ = 0;
    plot_prebuild_next_scan_ms_ = 0;
}

ReportManager::PlotPrebuildResult ReportManager::request_idle_plot_prebuild() {
    const char *gate_reason = "idle";
    if (!idle_prebuild_gate_open(&gate_reason)) {
        return PlotPrebuildResult::Waiting;
    }
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_ || plot_cache_writer_active()) {
        return PlotPrebuildResult::Waiting;
    }
    {
        Storage::Guard g;
        if (!Storage::mounted()) return PlotPrebuildResult::Unavailable;
    }

    uint32_t summary_revision = 0;
    bool catalog_present = false;
    uint8_t catalog_state = static_cast<uint8_t>(EdfReportCatalogState::Idle);
    uint32_t catalog_refresh_id = 0;
    if (!index_cache_key(summary_revision,
                         catalog_present,
                         catalog_state,
                         catalog_refresh_id)) {
        return PlotPrebuildResult::Waiting;
    }

    if (catalog_present &&
        catalog_state != static_cast<uint8_t>(EdfReportCatalogState::Ready) &&
        catalog_state != static_cast<uint8_t>(EdfReportCatalogState::Error)) {
        return PlotPrebuildResult::Waiting;
    }

    if (!plot_prebuild_key_matches(summary_revision,
                                   catalog_present,
                                   catalog_state,
                                   catalog_refresh_id)) {
        set_plot_prebuild_key(summary_revision,
                              catalog_present,
                              catalog_state,
                              catalog_refresh_id);
    } else if (plot_prebuild_next_scan_ms_ != 0 &&
               static_cast<int32_t>(millis() -
                                    plot_prebuild_next_scan_ms_) < 0) {
        return PlotPrebuildResult::Drained;
    }
    if (!build_queue_has_capacity()) {
        return PlotPrebuildResult::Waiting;
    }

    ScopedIndexedNightList snapshot("report_night_index_prebuild",
                                    AC_REPORT_SUMMARY_RECORD_MAX);
    if (!snapshot) return PlotPrebuildResult::Unavailable;
    size_t snapshot_count = 0;
    if (!build_indexed_nights(snapshot.data(),
                              snapshot.capacity(),
                              snapshot_count)) {
        return PlotPrebuildResult::Waiting;
    }

    constexpr size_t SCAN_STEPS_PER_CALL = 4;
    for (size_t step = 0; step < SCAN_STEPS_PER_CALL; ++step) {
        ReportIndexedNight night;
        size_t therapy_index = 0;
        const bool found =
            indexed_night_by_newest_cursor(snapshot.data(),
                                           snapshot_count,
                                           plot_prebuild_cursor_,
                                           night,
                                           therapy_index);
        if (!found) {
            plot_prebuild_next_scan_ms_ =
                millis() + AC_REPORT_PLOT_PREBUILD_RESCAN_MS;
            if (plot_prebuild_next_scan_ms_ == 0) {
                plot_prebuild_next_scan_ms_ = 1;
            }
            return PlotPrebuildResult::Drained;
        }
        plot_prebuild_cursor_++;

        if (night.edf_catalog_pending) return PlotPrebuildResult::Waiting;

        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        if (!take_summary_lock(pdMS_TO_TICKS(20))) {
            return PlotPrebuildResult::Waiting;
        }
        format_night_etag_unlocked(night.summary,
                                   night.source_signature,
                                   etag,
                                   sizeof(etag));
        give_summary_lock();
        if (result_plot_cache_exists_for_night(night, etag)) {
            continue;
        }

        const BuildQueueResult queued =
            enqueue_build(night.summary.start_ms,
                          therapy_index,
                          false,
                          true);
        if (queued == BuildQueueResult::Queued) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Idle plot prebuild queued night=%llu index=%lu\n",
                      static_cast<unsigned long long>(
                          night.summary.start_ms),
                      static_cast<unsigned long>(therapy_index));
            return PlotPrebuildResult::Queued;
        }
        if (queued == BuildQueueResult::AlreadyQueued) {
            return PlotPrebuildResult::AlreadyQueued;
        }
        if (queued == BuildQueueResult::Full) {
            if (plot_prebuild_cursor_ > 0) plot_prebuild_cursor_--;
            return PlotPrebuildResult::Waiting;
        }
        return PlotPrebuildResult::Unavailable;
    }
    return PlotPrebuildResult::Scanned;
}

void ReportManager::preempt_idle_plot_prebuild() {
    if (!plot_build_active_ || !plot_build_idle_prebuild_) return;
    const uint32_t elapsed_ms =
        plot_build_started_ms_
            ? static_cast<uint32_t>(millis() - plot_build_started_ms_)
            : 0;
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Idle plot prebuild preempted night=%llu elapsed_ms=%lu\n",
              static_cast<unsigned long long>(
                  plot_build_night_start_ms_.load()),
              static_cast<unsigned long>(elapsed_ms));
    reset_plot_build();
    release_result_edf_sessions();
}

ReportManager::BuildQueueResult ReportManager::enqueue_build(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh,
    bool idle_prebuild) {
    if (!build_queue_lock_ || !night_start_ms) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result build enqueue rejected night=%llu index=%lu "
                  "refresh=%u idle_prebuild=%u reason=unavailable\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u);
        return BuildQueueResult::Unavailable;
    }
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    build_queue_enqueue_total_++;
    build_queue_last_enqueue_night_ms_ = night_start_ms;
    build_queue_last_enqueue_therapy_index_ = therapy_index;
    copy_cstr(build_queue_last_service_block_,
              sizeof(build_queue_last_service_block_),
              "");
    for (size_t k = 0; k < build_queue_count_; ++k) {
        size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        if (build_queue_[idx].night_start_ms == night_start_ms) {
            build_queue_[idx].therapy_index = therapy_index;
            if (refresh) {
                build_queue_[idx].refresh = true;
                build_queue_[idx].next_attempt_ms = 0;
            }
            if (!idle_prebuild) {
                if (build_queue_[idx].idle_prebuild) {
                    build_queue_[idx].next_attempt_ms = 0;
                }
                build_queue_[idx].idle_prebuild = false;
            }
            build_queue_already_total_++;
            copy_cstr(build_queue_last_enqueue_result_,
                      sizeof(build_queue_last_enqueue_result_),
                      "already");
            const size_t count = build_queue_count_;
            xSemaphoreGive(build_queue_lock_);
            Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result build already queued night=%llu index=%lu "
                  "refresh=%u idle_prebuild=%u count=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u,
                  static_cast<unsigned long>(count));
            return BuildQueueResult::AlreadyQueued;
        }
    }
    if (build_queue_count_ < AC_REPORT_BUILD_QUEUE_MAX) {
        size_t tail =
            (build_queue_head_ + build_queue_count_) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_[tail].night_start_ms = night_start_ms;
        build_queue_[tail].therapy_index = therapy_index;
        build_queue_[tail].refresh = refresh;
        build_queue_[tail].idle_prebuild = idle_prebuild;
        build_queue_[tail].queued_ms = millis();
        build_queue_[tail].next_attempt_ms = 0;
        build_queue_count_++;
        build_queue_queued_total_++;
        copy_cstr(build_queue_last_enqueue_result_,
                  sizeof(build_queue_last_enqueue_result_),
                  "queued");
        const size_t count = build_queue_count_;
        xSemaphoreGive(build_queue_lock_);
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result build queued night=%llu index=%lu refresh=%u "
                  "idle_prebuild=%u count=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index),
                  refresh ? 1u : 0u,
                  idle_prebuild ? 1u : 0u,
                  static_cast<unsigned long>(count));
        return BuildQueueResult::Queued;
    }
    copy_cstr(build_queue_last_enqueue_result_,
              sizeof(build_queue_last_enqueue_result_),
              "full");
    xSemaphoreGive(build_queue_lock_);
    Log::logf(CAT_REPORT,
              idle_prebuild ? LOG_DEBUG : LOG_WARN,
              "Result build enqueue rejected night=%llu index=%lu "
              "refresh=%u idle_prebuild=%u reason=full count=%lu\n",
              static_cast<unsigned long long>(night_start_ms),
              static_cast<unsigned long>(therapy_index),
              refresh ? 1u : 0u,
              idle_prebuild ? 1u : 0u,
              static_cast<unsigned long>(AC_REPORT_BUILD_QUEUE_MAX));
    return BuildQueueResult::Full;
}

bool ReportManager::build_queue_has_capacity() const {
    if (!build_queue_lock_) return false;
    if (xSemaphoreTake(build_queue_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    const bool available = build_queue_count_ < AC_REPORT_BUILD_QUEUE_MAX;
    xSemaphoreGive(build_queue_lock_);
    return available;
}

void ReportManager::clear_build_queue(uint64_t night_start_ms, bool all) {
    if (!build_queue_lock_) return;
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    ResultBuildJob kept[AC_REPORT_BUILD_QUEUE_MAX];
    size_t kept_count = 0;
    for (size_t k = 0; k < build_queue_count_; ++k) {
        const size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        const ResultBuildJob &job = build_queue_[idx];
        if (!all && job.night_start_ms != night_start_ms) {
            kept[kept_count++] = job;
        }
    }
    for (size_t i = 0; i < AC_REPORT_BUILD_QUEUE_MAX; ++i) {
        build_queue_[i] = i < kept_count ? kept[i] : ResultBuildJob{};
    }
    build_queue_head_ = 0;
    build_queue_count_ = kept_count;
    xSemaphoreGive(build_queue_lock_);
}

void ReportManager::service_build_queue(bool realtime_active) {
    if (!build_queue_lock_) return;

    // Existing report work owns the materialization pipeline.
    if (summary_fetch_active_ || plot_build_active_ || range_build_active_) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      summary_fetch_active_
                          ? "summary"
                          : (plot_build_active_ ? "plot" : "range"));
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    // Realtime stream/therapy work preempts report materialization.
    if (realtime_active) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "realtime");
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    // Dequeue candidate
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    const bool have = build_queue_count_ > 0;
    ResultBuildJob job =
        have ? build_queue_[build_queue_head_] : ResultBuildJob{};

    xSemaphoreGive(build_queue_lock_);

    if (!have) return;

    // Retry backoff
    const uint32_t now_ms = millis();

    if (job.next_attempt_ms != 0 &&
        static_cast<int32_t>(now_ms - job.next_attempt_ms) < 0) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0) {
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "retry_wait");
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    // Idle prebuild gate
    if (job.idle_prebuild) {
        const char *reason = "idle";

        if (!idle_prebuild_gate_open(&reason)) {
            xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      reason ? reason : "gate");
            xSemaphoreGive(build_queue_lock_);
            return;
        }
    }

    // Prefetch interaction
    if (cache_fetch_active_) {
        if (!job.idle_prebuild) prefetch_yield_to_foreground();

        if (cache_fetch_active_) {
            xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
            copy_cstr(build_queue_last_service_block_,
                      sizeof(build_queue_last_service_block_),
                      "cache_fetch");
            xSemaphoreGive(build_queue_lock_);
            return;  // a non-prefetch fetch is in flight; wait
        }
    }

    // Materialize one job
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    build_queue_service_total_++;
    copy_cstr(build_queue_last_service_block_,
              sizeof(build_queue_last_service_block_),
              "");
    xSemaphoreGive(build_queue_lock_);

    active_build_idle_prebuild_ = job.idle_prebuild;

    const ResultPrepareOutcome outcome =
        prepare_result_by_night_start_internal(job.night_start_ms,
                                               job.therapy_index,
                                               job.refresh);

    active_build_idle_prebuild_ = false;

    const char *outcome_name =
        result_prepare_outcome_name(static_cast<uint8_t>(outcome));

    // Publish diagnostics
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    build_queue_last_night_ms_ = job.night_start_ms;
    build_queue_last_therapy_index_ = job.therapy_index;
    copy_cstr(build_queue_last_outcome_,
              sizeof(build_queue_last_outcome_),
              outcome_name);
    copy_cstr(build_queue_last_state_,
              sizeof(build_queue_last_state_),
              result_state_name());
    copy_cstr(build_queue_last_error_,
              sizeof(build_queue_last_error_),
              result_status_.error.c_str());
    xSemaphoreGive(build_queue_lock_);

    Log::logf(CAT_REPORT,
              outcome == ResultPrepareOutcome::Failed ? LOG_WARN : LOG_DEBUG,
              "Result build step night=%llu index=%lu refresh=%u "
              "idle_prebuild=%u outcome=%s state=%s error=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long long>(job.night_start_ms),
              static_cast<unsigned long>(job.therapy_index),
              job.refresh ? 1u : 0u,
              job.idle_prebuild ? 1u : 0u,
              outcome_name,
              result_state_name(),
              result_status_.error.length() ? result_status_.error.c_str()
                                            : "--",
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_status_.record_count),
              static_cast<unsigned long>(result_status_.payload_bytes));

    // Retry/defer current head
    if (outcome == ResultPrepareOutcome::Deferred ||
        outcome == ResultPrepareOutcome::Retry) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

        if (build_queue_count_ > 0 &&
            build_queue_[build_queue_head_].night_start_ms ==
                job.night_start_ms) {
            build_queue_[build_queue_head_].next_attempt_ms =
                millis() + AC_BG_WORKER_BUSY_RECHECK_MS;
        }

        xSemaphoreGive(build_queue_lock_);
        return;
    }

    // Pop completed/failed head
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);

    if (build_queue_count_ > 0 &&
        build_queue_[build_queue_head_].night_start_ms == job.night_start_ms) {
        build_queue_head_ = (build_queue_head_ + 1) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_count_--;
    }

    xSemaphoreGive(build_queue_lock_);
}

void ReportManager::service_prefetch(bool realtime_active) {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const PrefetchPhase phase = prefetch_phase_;
    const bool preempt = prefetch_preempt_req_;
    prefetch_preempt_req_ = false;
    const uint64_t active = prefetch_active_night_;
    xSemaphoreGive(prefetch_lock_);

    if (preempt && (phase == PrefetchPhase::Selecting ||
                    phase == PrefetchPhase::Fetching ||
                    phase == PrefetchPhase::Pending)) {
        if (cache_fetch_active_) {
            spool_.reset();
            abort_cache_write_fetch();
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error = "prefetch_preempted";
        }
        set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
        return;
    }

    if (phase == PrefetchPhase::Selecting) {
        if (realtime_active || busy()) return;
        if (edf_report_catalog_pending()) return;
        uint64_t night = 0;
        if (next_night_needing_cache(night) && night != 0) {
            set_prefetch_phase(PrefetchPhase::Pending, night, false, false);
        } else {
            set_prefetch_phase(PrefetchPhase::Drained, 0, false, false);
        }
        return;
    }

    if (phase == PrefetchPhase::Fetching && !cache_fetch_active_) {
        // The fetch concluded inside poll_cache_fetch; success = night covered.
        ReportNightCoverageStatus coverage;
        const bool covered =
            night_coverage(active, coverage) && coverage.missing_required == 0;
        if (!covered) prefetch_note_failure(active);
        set_prefetch_phase(covered ? PrefetchPhase::Done : PrefetchPhase::Failed,
                           active, covered, !covered);
        return;
    }

    if (realtime_active) {
        if (phase == PrefetchPhase::Fetching && cache_fetch_active_) {
            spool_.reset();
            abort_cache_write_fetch();
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error = "preempted_by_stream";
            set_prefetch_phase(PrefetchPhase::Idle, 0, false, false);
            Log::logf(CAT_REPORT, LOG_DEBUG,
                      "prefetch yielded to stream activity\n");
        }
        return;
    }

    if (phase == PrefetchPhase::Pending && !busy()) {
        if (active != 0 && request_night_cache(active, false)) {
            set_prefetch_phase(PrefetchPhase::Fetching, active, false, false);
        } else if (active != 0) {
            set_prefetch_phase(PrefetchPhase::Failed, active, false, true);
        } else {
            set_prefetch_phase(PrefetchPhase::Drained, 0, false, false);
        }
    }
}

bool ReportManager::clear_plot_cache_for_night(const ReportSummaryRecord &night,
                                               uint32_t &deleted) const {
    deleted = 0;
    if (!night.start_ms) return false;
    Storage::Guard g;
    if (!Storage::mounted()) return false;
    File dir = Storage::open(REPORT_PLOT_CACHE_DIR, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        const bool is_dir = file.isDirectory();
        const bool match =
            !is_dir && plot_cache_name_for_night(file.name(), night.start_ms);
        char path[REPORT_CACHE_PATH_MAX];
        const bool path_ok = match && cache_child_path(REPORT_PLOT_CACHE_DIR,
                                                       file.name(),
                                                       path,
                                                       sizeof(path));
        file.close();
        if (!match) continue;
        if (!path_ok || !Storage::remove(path)) {
            ok = false;
            continue;
        }
        deleted++;
    }
    dir.close();
    return ok;
}

bool ReportManager::clear_cache_range(int64_t start_ms,
                                      int64_t end_ms,
                                      ReportCacheClearResult &out) {
    if (start_ms < 0 || end_ms <= start_ms) return false;

    ReportSummaryRecord *night_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, night_batch)) return false;

    size_t night_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    for (size_t n = 0; records_ && n < record_count_ &&
                       n < AC_REPORT_SUMMARY_RECORD_MAX; ++n) {
        const ReportSummaryRecord &r = records_[n];
        if (!r.valid) continue;
        const int64_t ns = static_cast<int64_t>(r.start_ms);
        const int64_t ne = static_cast<int64_t>(r.end_ms);
        if (ne <= ns || ns >= end_ms || ne <= start_ms) continue;
        if (night_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;
        night_batch[night_count++] = r;
    }
    give_summary_lock();

    bool ok = true;
    uint32_t deleted = 0;

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);
    for (size_t i = 0; i < source_count; ++i) {
        const ReportSourceDef &source = sources[i];
        if (source.id == ReportSourceId::Summary) continue;
        if (!source.spool_type || !source.spool_type[0]) continue;

        deleted = 0;
        if (!ReportStore::clear_coverage(source.spool_type,
                                         start_ms,
                                         end_ms,
                                         deleted)) {
            ok = false;
        }
        out.coverage_deleted += deleted;

        // Chunks live in per-night dirs; clear each night overlapping the range.
        for (size_t n = 0; n < night_count; ++n) {
            const ReportSummaryRecord &r = night_batch[n];
            const int64_t ns = static_cast<int64_t>(r.start_ms);
            const int64_t ne = static_cast<int64_t>(r.end_ms);
            deleted = 0;
            if (!ReportStore::clear_chunks(ReportStoreChunkKind::Events,
                                           source.spool_type,
                                           source.spool_type,
                                           ns,
                                           start_ms,
                                           end_ms,
                                           deleted)) {
                ok = false;
            }
            out.chunks_deleted += deleted;
        }
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        const ReportSignalDef &signal = signals[i];
        const ReportSourceId source_ids[] = {
            signal.preferred_source,
            signal.fallback_source,
        };
        for (ReportSourceId source_id : source_ids) {
            const ReportSourceDef *source = report_source_def(source_id);
            if (!source || source->id == ReportSourceId::Summary ||
                !source->spool_type || !source->spool_type[0] ||
                !signal.store_name || !signal.store_name[0]) {
                continue;
            }
            for (size_t n = 0; n < night_count; ++n) {
                const ReportSummaryRecord &r = night_batch[n];
                const int64_t ns = static_cast<int64_t>(r.start_ms);
                const int64_t ne = static_cast<int64_t>(r.end_ms);
                deleted = 0;
                if (!ReportStore::clear_chunks(ReportStoreChunkKind::Series,
                                               source->spool_type,
                                               signal.store_name,
                                               ns,
                                               start_ms,
                                               end_ms,
                                               deleted)) {
                    ok = false;
                }
                out.chunks_deleted += deleted;
            }
        }
    }
    give_summary_scratch();
    return ok;
}

bool ReportManager::clear_cache_all(ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    uint32_t store_reset = 0;
    if (!ReportStore::reset_cache_store(store_reset)) {
        return false;
    }
    out.store_reset = store_reset;

    clear_build_queue(0, true);
    invalidate_materialized(0, true);
    clear_sparse_event_empty_markers(0);

    if (!take_summary_lock(portMAX_DELAY)) return false;
    clear_summary_records();
    night_epoch_count_ = 0;
    summary_status_.state = ReportSummaryState::Idle;
    summary_status_.revision++;
    summary_status_.records_total = 0;
    summary_status_.nights_with_therapy = 0;
    summary_status_.elapsed_ms = 0;
    summary_status_.active_spool.clear();
    summary_status_.error.clear();
    give_summary_lock();
    publish_summary_json_snapshot();

    clear_result_prepare();
    return true;
}

static void merge_cache_clear_result(ReportCacheClearResult &dst,
                                     const ReportCacheClearResult &src) {
    dst.store_reset += src.store_reset;
    dst.summary_deleted += src.summary_deleted;
    dst.nights_cleared += src.nights_cleared;
    dst.chunks_deleted += src.chunks_deleted;
    dst.coverage_deleted += src.coverage_deleted;
    dst.plots_deleted += src.plots_deleted;
    dst.result_json_deleted += src.result_json_deleted;
}

bool ReportManager::clear_cache_night(uint64_t night_start_ms,
                                      ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    ScopedIndexedNight indexed_night("clear_cache_night_index");
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms, indexed_night.get()) ||
        indexed_night->summary.end_ms <= indexed_night->summary.start_ms) {
        return false;
    }
    const ReportSummaryRecord &night = indexed_night->summary;
    clear_build_queue(night.start_ms, false);
    invalidate_materialized(night.start_ms, false);

    const bool ok = clear_cache_range(static_cast<int64_t>(night.start_ms),
                                      static_cast<int64_t>(night.end_ms),
                                      out);
    clear_sparse_event_empty_markers(night.start_ms);
    uint32_t plot_deleted = 0;
    if (clear_plot_cache_for_night(night, plot_deleted)) {
        out.plots_deleted += plot_deleted;
    }
    uint32_t result_json_deleted = 0;
    if (clear_result_json_cache_for_night(night, result_json_deleted)) {
        out.result_json_deleted += result_json_deleted;
    }
    if (result_status_.night_start_ms == night.start_ms) {
        clear_result_prepare();
    }
    if (take_summary_lock(portMAX_DELAY)) {
        for (size_t i = 0; i < night_epoch_count_; ++i) {
            if (night_epochs_[i].night_start_ms == night.start_ms) {
                night_epochs_[i] = night_epochs_[night_epoch_count_ - 1];
                --night_epoch_count_;
                break;
            }
        }
        give_summary_lock();
    }
    if (ok) out.nights_cleared = 1;
    return ok;
}

bool ReportManager::clear_oldest_cache_nights(size_t max_nights,
                                              ReportCacheClearResult &out) {
    out = {};
    if (max_nights == 0) return true;
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }

    ReportSummaryRecord *snapshot =
        static_cast<ReportSummaryRecord *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord),
            false));
    if (!snapshot) {
        log_report_alloc_failed(
            "cache_prune_snapshot",
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
        return false;
    }

    size_t count = 0;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        Memory::free(snapshot);
        return false;
    }
    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records_[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;
        snapshot[count++] = record;
    }
    give_summary_lock();

    bool ok = true;
    const size_t limit = count < max_nights ? count : max_nights;
    for (size_t i = 0; i < limit; ++i) {
        ReportCacheClearResult current_clear;
        if (!clear_cache_night(snapshot[i].start_ms, current_clear)) {
            ok = false;
            break;
        }
        merge_cache_clear_result(out, current_clear);
    }
    Memory::free(snapshot);
    if (out.nights_cleared > 0) {
        Log::logf(CAT_REPORT,
                  LOG_INFO,
                  "Cache pruned oldest nights=%lu chunks=%lu coverage=%lu "
                  "plots=%lu result_json=%lu\n",
                  static_cast<unsigned long>(out.nights_cleared),
                  static_cast<unsigned long>(out.chunks_deleted),
                  static_cast<unsigned long>(out.coverage_deleted),
                  static_cast<unsigned long>(out.plots_deleted),
                  static_cast<unsigned long>(out.result_json_deleted));
    }
    return ok;
}

bool ReportManager::prune_cache_to_latest_nights(size_t keep_latest,
                                                 ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_ ||
        range_build_active_) {
        return false;
    }
    size_t report_nights = 0;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &record = records_[i];
        if (!record.valid || record.end_ms <= record.start_ms) continue;
        report_nights++;
    }
    give_summary_lock();
    if (report_nights <= keep_latest) return true;
    return clear_oldest_cache_nights(report_nights - keep_latest, out);
}

bool ReportManager::cancel_cache_fetch() {
    if (!cache_fetch_active_) return false;
    spool_.reset();
    abort_cache_write_fetch();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = "cancelled";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT,
              LOG_INFO,
              "Cache fetch cancelled night=%llu source=%s\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              report_source_spool_type(cache_status_.active_source));
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare("cache_cancelled");
    }
    return true;
}

bool ReportManager::ensure_result_chunks() {
    if (result_chunks_) return true;
    result_chunks_ = static_cast<ReportResultChunk *>(
        Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                             sizeof(ReportResultChunk),
                             false));
    if (!result_chunks_) {
        log_report_alloc_failed(
            "result_chunks",
            AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
        fail_result_prepare("result_manifest_alloc_failed");
        return false;
    }
    result_chunk_capacity_ = AC_REPORT_RESULT_CHUNK_MAX;
    return true;
}

bool ReportManager::ensure_result_slots() {
    if (!result_slots_lock_) {
        result_slots_lock_ = xSemaphoreCreateMutex();
        if (!result_slots_lock_) {
            log_report_alloc_failed("result_slots_lock", 0);
            return false;
        }
    }
    if (result_slots_) return true;
    result_slots_ = static_cast<MaterializedResult *>(Memory::alloc_large(
        AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult), false));
    if (!result_slots_) {
        log_report_alloc_failed(
            "result_slots",
            AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult));
        return false;
    }
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        new (&result_slots_[i]) MaterializedResult();
    }
    return true;
}

bool ReportManager::ensure_result_edf_sessions() {
    if (result_edf_sessions_) return true;
    result_edf_sessions_ = static_cast<EdfReportSessionDescriptor *>(
        Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                             sizeof(EdfReportSessionDescriptor),
                             false));
    if (!result_edf_sessions_) {
        log_report_alloc_failed(
            "result_edf_sessions",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    return true;
}

bool ReportManager::ensure_result_resolve_buffers() {
    if (!result_resolved_plan_) {
        result_resolved_plan_ = static_cast<ReportResolvedPlan *>(
            Memory::calloc_large(1, sizeof(ReportResolvedPlan), false));
        if (!result_resolved_plan_) {
            log_report_alloc_failed("result_resolved_plan",
                                    sizeof(ReportResolvedPlan));
            fail_result_prepare("result_plan_alloc_failed");
            return false;
        }
    }
    if (!result_resolve_scratch_) {
        result_resolve_scratch_ = static_cast<ReportResolveScratch *>(
            Memory::calloc_large(1, sizeof(ReportResolveScratch), false));
        if (!result_resolve_scratch_) {
            log_report_alloc_failed("result_resolve_scratch",
                                    sizeof(ReportResolveScratch));
            fail_result_prepare("result_scratch_alloc_failed");
            return false;
        }
    }
    return true;
}

bool ReportManager::result_uses_edf_provider() const {
    if (!result_chunks_) return false;
    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        if (result_chunks_[i].provider_ref.provider == ReportProviderId::Edf) {
            return true;
        }
    }
    return false;
}

void ReportManager::release_result_edf_sessions() {
    Memory::free(result_edf_sessions_);
    result_edf_sessions_ = nullptr;
    result_edf_session_count_ = 0;
}

void ReportManager::clear_result_ranges() {
    memset(result_ranges_, 0, sizeof(result_ranges_));
    result_range_count_ = 0;
}

bool ReportManager::set_result_ranges_from_indexed_night(
    const ReportIndexedNight &night) {
    clear_result_ranges();
    auto append_range = [&](int64_t start_ms, int64_t end_ms) {
        if (end_ms <= start_ms ||
            result_range_count_ >= AC_REPORT_SUMMARY_SESSION_MAX) {
            return;
        }
        PlotRange &range = result_ranges_[result_range_count_++];
        range.start_ms = start_ms;
        range.end_ms = end_ms;
    };
    const size_t edf_count =
        std::min(night.data_range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (night.has_edf && edf_count > 0) {
        for (size_t i = 0; i < edf_count; ++i) {
            append_range(night.data_ranges[i].start_ms,
                         night.data_ranges[i].end_ms);
        }
    } else {
        const size_t display_count =
            std::min(night.range_count,
                     static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
        for (size_t i = 0; i < display_count; ++i) {
            append_range(night.ranges[i].start_ms, night.ranges[i].end_ms);
        }
    }
    std::sort(result_ranges_,
              result_ranges_ + result_range_count_,
              [](const PlotRange &a, const PlotRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return result_range_count_ > 0;
}

bool ReportManager::set_result_ranges_from_edf_sessions() {
    if (!result_edf_sessions_ || result_edf_session_count_ == 0) return false;
    clear_result_ranges();
    for (size_t i = 0; i < result_edf_session_count_ &&
                       result_range_count_ < AC_REPORT_SUMMARY_SESSION_MAX;
         ++i) {
        const EdfReportSessionDescriptor &session = result_edf_sessions_[i];
        if (!edf_session_has_report_numeric(session)) continue;
        if (session.earliest_header_start_ms <= 0 ||
            session.latest_header_end_ms <= session.earliest_header_start_ms) {
            continue;
        }
        PlotRange &range = result_ranges_[result_range_count_++];
        range.start_ms = session.earliest_header_start_ms;
        range.end_ms = session.latest_header_end_ms;
    }
    std::sort(result_ranges_,
              result_ranges_ + result_range_count_,
              [](const PlotRange &a,
                 const PlotRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return result_range_count_ > 0;
}

bool ReportManager::result_data_span(int64_t &span_start_ms,
                                     int64_t &span_end_ms) const {
    if (result_range_count_ == 0) {
        return indexed_night_data_span(result_indexed_night_,
                                       span_start_ms,
                                       span_end_ms) ||
               night_data_span(result_night_, span_start_ms, span_end_ms);
    }
    span_start_ms = result_ranges_[0].start_ms;
    span_end_ms = result_ranges_[0].end_ms;
    for (size_t i = 1; i < result_range_count_; ++i) {
        span_start_ms = std::min(span_start_ms, result_ranges_[i].start_ms);
        span_end_ms = std::max(span_end_ms, result_ranges_[i].end_ms);
    }
    return span_end_ms > span_start_ms;
}

void ReportManager::clear_result_prepare() {
    reset_plot_build();
    reset_range_plot_build(true);
    if (result_chunks_ && result_chunk_capacity_) {
        memset(result_chunks_, 0,
               result_chunk_capacity_ * sizeof(ReportResultChunk));
    }
    memset(result_streams_, 0, sizeof(result_streams_));
    result_stream_count_ = 0;
    release_result_edf_sessions();
    result_indexed_night_ = {};
    result_night_ = {};
    result_etag_[0] = '\0';
    clear_result_ranges();
    result_plot_bin_.clear();
    result_skip_plot_cache_ = false;
    result_status_ = {};
}

void ReportManager::fail_result_prepare(const char *message) {
    reset_plot_build();
    result_status_.state = ReportResultState::Error;
    result_status_.error = message ? message : "result_prepare_failed";
    if (result_night_.start_ms != 0 && result_status_.night_start_ms != 0) {
        publish_result_to_slot();
    }
    release_result_edf_sessions();
    Log::logf(CAT_REPORT, LOG_WARN, "Result prepare failed: %s\n",
              result_status_.error.c_str());
}

const char *ReportManager::result_state_name() const {
    return result_state_name_for(result_status_.state);
}

bool ReportManager::add_result_stream(ReportStoreChunkKind kind,
                                      ReportSourceId source,
                                      ReportSignalId signal,
                                      const char *name,
                                      bool required,
                                      bool complete,
                                      size_t &stream_index) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < result_stream_count_; ++i) {
        ReportResultStream &stream = result_streams_[i];
        if (stream.kind == kind && stream.signal == signal &&
            stream.name && strcmp(stream.name, name) == 0) {
            stream_index = i;
            if (required) stream.required = true;
            if (!complete && stream.complete) {
                stream.complete = false;
                if (stream.required) result_status_.missing_streams++;
            }
            if (stream.chunk_count == 0) {
                stream.source = source;
            }
            return true;
        }
    }

    if (result_stream_count_ >= AC_REPORT_RESULT_STREAM_MAX) {
        fail_result_prepare("result_streams_full");
        return false;
    }
    stream_index = result_stream_count_;
    ReportResultStream &stream = result_streams_[result_stream_count_++];
    stream.kind = kind;
    stream.source = source;
    stream.signal = signal;
    stream.name = name;
    stream.required = required;
    stream.complete = complete;
    result_status_.stream_count =
        static_cast<uint32_t>(result_stream_count_);
    if (required && !complete) result_status_.missing_streams++;
    return true;
}

bool ReportManager::collect_result_chunk(void *context,
                                         const ReportProviderChunk &info) {
    ResultChunkContext *ctx = static_cast<ResultChunkContext *>(context);
    if (!ctx || !ctx->manager || !info.name || !info.name[0]) return false;
    ReportManager *manager = ctx->manager;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }
    for (uint32_t i = 0; i < manager->result_status_.chunk_count; ++i) {
        const ReportResultChunk &existing = manager->result_chunks_[i];
        if (existing.kind == info.kind &&
            existing.source == info.source &&
            existing.name && info.name &&
            strcmp(existing.name, info.name) == 0 &&
            existing.start_ms == info.start_ms &&
            existing.end_ms == info.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            info.ref)) {
            return true;
        }
    }

    const bool fixed_stream =
        ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) == 0;
    size_t stream_index = ctx->stream_index;
    if (fixed_stream && stream_index != SIZE_MAX) {
        if (stream_index >= manager->result_stream_count_) {
            manager->fail_result_prepare("bad_result_stream");
            return false;
        }
        const ReportResultStream &stream =
            manager->result_streams_[stream_index];
        if (stream.kind != info.kind ||
            stream.signal != info.signal ||
            !stream.name ||
            strcmp(stream.name, info.name) != 0) {
            manager->fail_result_prepare("result_stream_mismatch");
            return false;
        }
    }
    if (stream_index == SIZE_MAX || !fixed_stream) {
        if (!manager->add_result_stream(info.kind,
                                        info.source,
                                        info.signal,
                                        info.name,
                                        ctx->required,
                                        true,
                                        stream_index)) {
            return false;
        }
    }
    if (!manager->add_provider_result_chunk(info,
                                            ctx->required,
                                            stream_index)) {
        return false;
    }
    if (fixed_stream) ctx->stream_index = stream_index;
    ctx->entries++;
    return true;
}

bool ReportManager::add_provider_result_chunk(
    const ReportProviderChunk &provider_chunk,
    bool required,
    size_t stream_index) {
    (void)required;
    if (stream_index >= result_stream_count_ || stream_index > UINT8_MAX) {
        fail_result_prepare("bad_result_stream");
        return false;
    }
    uint32_t stream_bit = 0;
    if (!report_stream_bit(stream_index, stream_bit)) {
        fail_result_prepare("bad_result_stream");
        return false;
    }
    if (!result_chunks_) {
        fail_result_prepare("result_chunks_missing");
        return false;
    }

    auto account_stream = [&]() {
        ReportResultStream &stream = result_streams_[stream_index];
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        result_status_.record_count += provider_chunk.record_count;
        result_status_.payload_bytes += provider_chunk.payload_len;
    };

    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        ReportResultChunk &existing = result_chunks_[i];
        const bool same_physical =
            result_chunk_same_physical_edf(existing, provider_chunk);
        const bool same_logical =
            existing.kind == provider_chunk.kind &&
            existing.source == provider_chunk.source &&
            existing.name && provider_chunk.name &&
            strcmp(existing.name, provider_chunk.name) == 0 &&
            existing.start_ms == provider_chunk.start_ms &&
            existing.end_ms == provider_chunk.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            provider_chunk.ref);
        if (!same_physical && !same_logical) continue;
        if ((existing.stream_mask & stream_bit) != 0) return true;
        existing.stream_mask |= stream_bit;
        account_stream();
        return true;
    }

    if (result_status_.chunk_count >= result_chunk_capacity_) {
        fail_result_prepare("result_chunks_full");
        return false;
    }

    ReportResultChunk &chunk =
        result_chunks_[result_status_.chunk_count++];
    chunk.provider_ref = provider_chunk.ref;
    chunk.kind = provider_chunk.kind;
    chunk.source = provider_chunk.source;
    chunk.signal = provider_chunk.signal;
    chunk.name = provider_chunk.name;
    chunk.stream_index = static_cast<uint8_t>(stream_index);
    chunk.stream_mask = stream_bit;
    chunk.start_ms = provider_chunk.start_ms;
    chunk.end_ms = provider_chunk.end_ms;
    chunk.payload_schema = provider_chunk.payload_schema;
    chunk.record_count = provider_chunk.record_count;
    chunk.payload_len = provider_chunk.payload_len;

    account_stream();
    return true;
}

// Earliest start / latest end of a source's cached chunks for a night, within
// the night's session span. Returns false when the source has none cached.
bool ReportManager::source_chunk_extent(const ReportSummaryRecord &night,
                                        ReportSourceId source,
                                        const char *name,
                                        int64_t &min_start,
                                        int64_t &max_end) const {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0] || !name || !name[0]) {
        return false;
    }
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;
    ReportProviderChunkExtent extent;
    if (!spool_report_provider().chunk_extent(
            ReportStoreChunkKind::Series,
            *def,
            name,
            static_cast<int64_t>(night.start_ms),
            span_start,
            span_end,
            extent)) {
        return false;
    }
    min_start = extent.min_start_ms;
    max_end = extent.max_end_ms;
    return true;
}

bool ReportManager::add_provider_chunks_to_result_stream(
    const ReportDataProvider &provider,
    ReportStoreChunkKind kind,
    ReportSourceId source,
    ReportSignalId signal,
    const char *name,
    int64_t night_start_ms,
    int64_t start_ms,
    int64_t end_ms,
    bool required,
    bool complete,
    size_t stream_index) {
    if (!ensure_result_chunks()) return false;
    const ReportSourceDef *source_def = report_source_def(source);
    if (!source_def || !source_def->spool_type || !source_def->spool_type[0]) {
        fail_result_prepare("bad_result_source");
        return false;
    }
    const bool sparse_events = kind == ReportStoreChunkKind::Events;
    if (stream_index >= result_stream_count_ || !name || !name[0]) {
        fail_result_prepare("bad_result_stream");
        return false;
    }
    ReportResultStream &stream = result_streams_[stream_index];
    if (stream.kind != kind || stream.signal != signal || !stream.name ||
        strcmp(stream.name, name) != 0) {
        fail_result_prepare("result_stream_mismatch");
        return false;
    }
    if (required) stream.required = true;
    if (!complete && stream.complete) {
        stream.complete = false;
        if (stream.required) result_status_.missing_streams++;
    }
    if (!complete) return true;

    const uint32_t chunks_before = stream.chunk_count;
    ResultChunkContext context;
    context.manager = this;
    context.kind = kind;
    context.source = source;
    context.signal = signal;
    context.name = name;
    context.required = required;
    context.stream_index = stream_index;
    if (!provider.for_each_chunk(kind,
                                 *source_def,
                                 signal,
                                 name,
                                 night_start_ms,
                                 start_ms,
                                 end_ms,
                                 collect_result_chunk,
                                 &context)) {
        if (result_status_.state != ReportResultState::Error) {
            fail_result_prepare("result_chunk_list_failed");
        }
        return false;
    }
    if (!sparse_events &&
        required &&
        result_streams_[stream_index].chunk_count == chunks_before &&
        result_streams_[stream_index].complete) {
        result_streams_[stream_index].complete = false;
        result_status_.missing_streams++;
    }
    return true;
}

bool ReportManager::edf_report_catalog_pending() const {
    if (!edf_catalog_) return false;

    EdfReportCatalogStatus status;
    if (!edf_catalog_->status(status, 0)) return true;
    if (status.state == EdfReportCatalogState::Ready ||
        status.state == EdfReportCatalogState::Error) {
        return false;
    }
    if (status.sessions > 0) {
        return false;
    }
    (void)edf_catalog_->request_refresh();
    return true;
}

bool ReportManager::edf_catalog_session_has_annotation_marker(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!edf_catalog_ || !edf_session_has_report_numeric(session)) {
        return false;
    }
    if (edf_session_has_report_annotation(session)) return true;
    const size_t count = edf_catalog_->session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!edf_catalog_->copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(session, scratch)) {
            return true;
        }
    }
    return false;
}

bool ReportManager::edf_catalog_annotation_has_numeric_session(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (!edf_catalog_ || !edf_session_has_report_annotation(session)) {
        return false;
    }
    if (edf_session_has_report_numeric(session)) return true;
    const size_t count = edf_catalog_->session_count();
    for (size_t i = 0; i < count; ++i) {
        if (!edf_catalog_->copy_session(i, scratch)) continue;
        if (edf_session_annotation_matches_numeric(scratch, session)) {
            return true;
        }
    }
    return false;
}

bool ReportManager::edf_catalog_session_reportable(
    const EdfReportSessionDescriptor &session,
    EdfReportSessionDescriptor &scratch) const {
    if (edf_session_has_report_numeric(session)) {
        return edf_catalog_session_has_annotation_marker(session, scratch);
    }
    return edf_catalog_annotation_has_numeric_session(session, scratch);
}

bool ReportManager::collect_edf_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &session_count,
    bool *pending_out) const {
    session_count = 0;
    if (pending_out) *pending_out = false;
    if (!edf_catalog_ || !sessions || session_capacity == 0 ||
        range_end_ms <= range_start_ms) {
        return false;
    }

    EdfReportCatalogStatus status;
    if (!edf_catalog_->status(status, 0) ||
        (status.state != EdfReportCatalogState::Ready &&
         status.sessions == 0)) {
        if (status.state != EdfReportCatalogState::Error) {
            (void)edf_catalog_->request_refresh();
            if (pending_out) *pending_out = true;
        }
        return false;
    }

    const size_t count = edf_catalog_->session_count();
    char target_sleep_day[9] = {};
    const bool have_target_sleep_day =
        report_summary_sleep_day_yyyymmdd(night,
                                          target_sleep_day,
                                          sizeof(target_sleep_day));
    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        log_report_alloc_failed("edf_session_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    for (size_t i = 0; i < count &&
                       session_count < session_capacity; ++i) {
        EdfReportSessionDescriptor &session = sessions[session_count];
        if (!edf_catalog_->copy_session(i, session)) continue;
        const bool matches_sleep_day =
            have_target_sleep_day &&
            strcmp(session.sleep_day, target_sleep_day) == 0;
        const bool matches_range =
            session.earliest_header_start_ms > 0 &&
            session.latest_header_end_ms > session.earliest_header_start_ms &&
            ranges_overlap(session.earliest_header_start_ms,
                           session.latest_header_end_ms,
                           range_start_ms,
                           range_end_ms);
        if (!matches_sleep_day && !matches_range) {
            continue;
        }
        if (!edf_catalog_session_reportable(session, *marker_scratch)) {
            continue;
        }
        session_count++;
    }
    Memory::free(marker_scratch);

    std::sort(sessions,
              sessions + session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  return a.earliest_header_start_ms <
                         b.earliest_header_start_ms;
              });
    if (!append_edf_sessions_for_selected_days(sessions,
                                               session_capacity,
                                               session_count)) {
        return false;
    }
    if (session_count == 0 &&
        status.state == EdfReportCatalogState::Refreshing) {
        if (pending_out) *pending_out = true;
        return false;
    }
    return session_count > 0;
}

bool ReportManager::append_edf_sessions_for_selected_days(
    EdfReportSessionDescriptor *sessions,
    size_t session_capacity,
    size_t &session_count) const {
    if (!edf_catalog_ || !sessions || session_count == 0 ||
        session_capacity == 0) {
        return true;
    }

    const size_t base_count = session_count;
    const size_t catalog_count = edf_catalog_->session_count();
    EdfReportSessionDescriptor *candidate =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!candidate) {
        log_report_alloc_failed("edf_event_session_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    EdfReportSessionDescriptor *marker_scratch =
        static_cast<EdfReportSessionDescriptor *>(
            Memory::alloc_large(sizeof(EdfReportSessionDescriptor), false));
    if (!marker_scratch) {
        Memory::free(candidate);
        log_report_alloc_failed("edf_event_marker_scratch",
                                sizeof(EdfReportSessionDescriptor));
        return false;
    }
    for (size_t i = 0; i < catalog_count; ++i) {
        if (!edf_catalog_->copy_session(i, *candidate)) continue;

        bool selected_day = false;
        for (size_t base = 0; base < base_count; ++base) {
            if (strcmp(sessions[base].sleep_day, candidate->sleep_day) == 0) {
                selected_day = true;
                break;
            }
        }
        if (!selected_day) continue;
        if (!edf_catalog_session_reportable(*candidate, *marker_scratch)) {
            continue;
        }

        bool duplicate = false;
        for (size_t existing = 0; existing < session_count; ++existing) {
            if (edf_session_same_identity(sessions[existing], *candidate)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        if (session_count >= session_capacity) {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "EDF report session list full capacity=%u\n",
                      static_cast<unsigned>(session_capacity));
            break;
        }
        sessions[session_count++] = *candidate;
    }
    Memory::free(marker_scratch);
    Memory::free(candidate);
    merge_edf_annotation_sessions(sessions, session_count);
    std::sort(sessions,
              sessions + session_count,
              [](const EdfReportSessionDescriptor &a,
                 const EdfReportSessionDescriptor &b) {
                  if (strcmp(a.sleep_day, b.sleep_day) != 0) {
                      return strcmp(a.sleep_day, b.sleep_day) < 0;
                  }
                  return strcmp(a.session_stamp, b.session_stamp) < 0;
              });
    return true;
}

bool ReportManager::edf_report_complete_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) const {
    EdfReportSessionDescriptor *sessions =
        static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
            AC_REPORT_EDF_SESSION_MAX *
            sizeof(EdfReportSessionDescriptor),
            false));
    if (!sessions) {
        log_report_alloc_failed(
            "edf_report_session_snapshot",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        if (pending_out) *pending_out = false;
        return false;
    }

    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night,
                                        range_start_ms,
                                        range_end_ms,
                                        sessions,
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        pending_out)) {
        Memory::free(sessions);
        return false;
    }

    EdfReportRequiredRange required_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t required_range_count =
        collect_required_edf_ranges(night,
                                    range_start_ms,
                                    range_end_ms,
                                    required_ranges,
                                    AC_REPORT_SUMMARY_SESSION_MAX);

    const bool complete = edf_report_plan_covers_report(sessions,
                                                        session_count,
                                                        required_ranges,
                                                        required_range_count);
    Memory::free(sessions);
    return complete;
}

bool ReportManager::edf_report_complete_for_indexed_night(
    const ReportIndexedNight &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) const {
    EdfReportSessionDescriptor *sessions =
        static_cast<EdfReportSessionDescriptor *>(Memory::alloc_large(
            AC_REPORT_EDF_SESSION_MAX *
            sizeof(EdfReportSessionDescriptor),
            false));
    if (!sessions) {
        log_report_alloc_failed(
            "edf_report_session_snapshot",
            AC_REPORT_EDF_SESSION_MAX *
                sizeof(EdfReportSessionDescriptor));
        if (pending_out) *pending_out = false;
        return false;
    }

    size_t session_count = 0;
    if (!collect_edf_sessions_for_night(night.summary,
                                        range_start_ms,
                                        range_end_ms,
                                        sessions,
                                        AC_REPORT_EDF_SESSION_MAX,
                                        session_count,
                                        pending_out)) {
        Memory::free(sessions);
        return false;
    }

    EdfReportRequiredRange required_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    ReportSessionRange source_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t source_range_count =
        collect_indexed_night_data_ranges(night,
                                          source_ranges,
                                          AC_REPORT_SUMMARY_SESSION_MAX);
    const size_t required_range_count =
        collect_required_ranges_from_session_ranges(
            source_ranges,
            source_range_count,
            range_start_ms,
            range_end_ms,
            required_ranges,
            AC_REPORT_SUMMARY_SESSION_MAX);

    const bool complete = edf_report_plan_covers_report(sessions,
                                                        session_count,
                                                        required_ranges,
                                                        required_range_count);
    Memory::free(sessions);
    return complete;
}

bool ReportManager::edf_report_complete_for_night_sessions(
    const ReportSummaryRecord &night) const {
    int64_t span_start_ms = 0;
    int64_t span_end_ms = 0;
    return night_data_span(night, span_start_ms, span_end_ms) &&
           edf_report_complete_for_night(night,
                                         span_start_ms,
                                         span_end_ms);
}

bool ReportManager::find_edf_sessions_for_night(
    const ReportSummaryRecord &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *pending_out) {
    result_edf_session_count_ = 0;
    if (pending_out) *pending_out = false;
    if (!ensure_result_edf_sessions()) return false;
    memset(result_edf_sessions_,
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    return collect_edf_sessions_for_night(night,
                                          range_start_ms,
                                          range_end_ms,
                                          result_edf_sessions_,
                                          AC_REPORT_EDF_SESSION_MAX,
                                          result_edf_session_count_,
                                          pending_out);
}

bool ReportManager::read_result_chunk_payload(
    const ReportResultChunk &chunk,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) {
    ReportProviderChunk provider_chunk;
    provider_chunk_from_result(chunk, provider_chunk);

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().read_chunk(
                provider_chunk,
                static_cast<int64_t>(result_night_.start_ms),
                meta,
                payload);
        case ReportProviderId::Edf:
            return edf_report_provider().read_chunk(provider_chunk,
                                                   result_edf_sessions_,
                                                   result_edf_session_count_,
                                                   meta,
                                                   payload);
        default:
            return false;
    }
}

void ReportManager::provider_chunk_from_result(
    const ReportResultChunk &chunk,
    ReportProviderChunk &out) const {
    out = {};
    out.ref = chunk.provider_ref;
    out.kind = chunk.kind;
    out.source = chunk.source;
    out.signal = chunk.signal;
    out.name = chunk.name;
    out.start_ms = chunk.start_ms;
    out.end_ms = chunk.end_ms;
    out.payload_schema = chunk.payload_schema;
    out.record_count = chunk.record_count;
    out.payload_len = chunk.payload_len;
}

bool ReportManager::result_chunk_has_stream(const ReportResultChunk &chunk,
                                            size_t stream_index) const {
    uint32_t bit = 0;
    if (chunk.stream_mask != 0 && report_stream_bit(stream_index, bit)) {
        return (chunk.stream_mask & bit) != 0;
    }
    return stream_index == chunk.stream_index;
}

bool ReportManager::result_chunk_same_physical_edf(
    const ReportResultChunk &existing,
    const ReportProviderChunk &candidate) const {
    if (existing.kind != candidate.kind ||
        existing.provider_ref.provider != ReportProviderId::Edf ||
        candidate.ref.provider != ReportProviderId::Edf ||
        existing.payload_schema != candidate.payload_schema ||
        existing.start_ms != candidate.start_ms ||
        existing.end_ms != candidate.end_ms) {
        return false;
    }
    if (existing.kind == ReportStoreChunkKind::Series &&
        edf_report_signal_uses_edge_zero_padding(existing.signal) !=
            edf_report_signal_uses_edge_zero_padding(candidate.signal)) {
        return false;
    }

    EdfReportProviderToken a;
    EdfReportProviderToken b;
    if (!edf_report_provider_unpack_token(existing.provider_ref, a) ||
        !edf_report_provider_unpack_token(candidate.ref, b)) {
        return false;
    }
    return a.session_index == b.session_index &&
           a.file_slot == b.file_slot &&
           a.file_kind == b.file_kind &&
           a.data_kind == b.data_kind &&
           a.first_record == b.first_record &&
           a.record_count == b.record_count;
}

bool ReportManager::provider_chunk_from_result_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderChunk &out) const {
    if (!streams || stream_index >= stream_count ||
        !result_chunk_has_stream(chunk, stream_index)) {
        return false;
    }
    const ReportResultStream &stream = streams[stream_index];
    provider_chunk_from_result(chunk, out);
    out.source = stream.source;
    out.signal = stream.signal;
    out.name = stream.name;

    if (chunk.provider_ref.provider != ReportProviderId::Edf ||
        chunk.kind != ReportStoreChunkKind::Series) {
        return true;
    }

    EdfReportProviderToken token;
    if (!edf_report_provider_unpack_token(chunk.provider_ref, token) ||
        token.session_index >= session_count || !sessions) {
        return false;
    }

    EdfReportDataPlanEntry entry;
    if (!edf_report_find_signal_entry_for_chunk(
            sessions[token.session_index],
            stream.signal,
            token.file_kind,
            token.file_slot,
            token.first_record,
            token.record_count,
            chunk.start_ms,
            chunk.end_ms,
            entry)) {
        return false;
    }

    token.primary = entry.primary;
    token.trim_leading_padding = entry.trim_leading_padding;
    token.trim_trailing_padding = entry.trim_trailing_padding;
    copy_cstr(token.signal_label,
              sizeof(token.signal_label),
              entry.signal_label);
    edf_report_provider_pack_token(out.ref, token);
    out.source = entry.source;
    out.signal = entry.signal;
    out.name = entry.name;
    out.start_ms = entry.start_ms;
    out.end_ms = entry.end_ms;
    out.record_count = entry.record_count_estimate;
    out.payload_len = entry.payload_len_estimate;
    return true;
}

bool ReportManager::for_each_result_series_sample(
    const ReportResultChunk &chunk,
    size_t stream_index,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }
    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           result_streams_,
                                           result_stream_count_,
                                           result_edf_sessions_,
                                           result_edf_session_count_,
                                           provider_chunk)) {
        return false;
    }

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().for_each_series_sample(
                provider_chunk,
                static_cast<int64_t>(result_night_.start_ms),
                stats,
                callback,
                context);
        case ReportProviderId::Edf:
            return edf_report_provider().for_each_series_sample(
                provider_chunk,
                result_edf_sessions_,
                result_edf_session_count_,
                stats,
                callback,
                context);
        default:
            return false;
    }
}

bool ReportManager::result_plot_cache_path_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    char *path,
    size_t path_size) const {
    return result_plot_cache_path_for_etag(night.summary.start_ms,
                                           etag,
                                           path,
                                           path_size);
}

bool ReportManager::result_plot_cache_path_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    char *path,
                                                    size_t path_size) const {
    if (!path || !path_size || night_start_ms == 0 || !etag || !etag[0]) {
        return false;
    }
    const int written = snprintf(
        path,
        path_size,
        "%s/%llu-%s.bin",
        REPORT_PLOT_CACHE_DIR,
        static_cast<unsigned long long>(night_start_ms),
        etag);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ReportManager::result_json_cache_path_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    char *path,
    size_t path_size) const {
    return result_json_cache_path_for_etag(night.summary.start_ms,
                                           etag,
                                           path,
                                           path_size);
}

bool ReportManager::result_json_cache_path_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    char *path,
                                                    size_t path_size) const {
    if (!path || !path_size || night_start_ms == 0 || !etag || !etag[0]) {
        return false;
    }
    const int written = snprintf(
        path,
        path_size,
        "%s/%llu-%s.json",
        REPORT_RESULT_JSON_CACHE_DIR,
        static_cast<unsigned long long>(night_start_ms),
        etag);
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ReportManager::result_plot_cache_path(char *path,
                                           size_t path_size) const {
    return result_plot_cache_path_for_night(result_indexed_night_,
                                            result_etag_,
                                            path,
                                            path_size);
}

bool ReportManager::result_plot_cache_exists_for_night(
    const ReportIndexedNight &night,
    const char *etag) const {
    Storage::Guard g;
    if (!Storage::mounted()) return false;
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }
    return Storage::exists(path);
}

bool ReportManager::clear_result_json_cache_for_night(
    const ReportSummaryRecord &night,
    uint32_t &deleted) const {
    deleted = 0;
    if (!night.start_ms) return false;
    Storage::Guard g;
    if (!Storage::mounted()) return false;
    File dir = Storage::open(REPORT_RESULT_JSON_CACHE_DIR, "r");
    if (!dir) return true;
    if (!dir.isDirectory()) {
        dir.close();
        return false;
    }

    bool ok = true;
    while (true) {
        File file = dir.openNextFile();
        if (!file) break;
        const bool is_dir = file.isDirectory();
        const bool match =
            !is_dir && plot_cache_name_for_night(file.name(), night.start_ms);
        char path[REPORT_CACHE_PATH_MAX];
        const bool path_ok = match &&
                             cache_child_path(REPORT_RESULT_JSON_CACHE_DIR,
                                              file.name(),
                                              path,
                                              sizeof(path));
        file.close();
        if (!match) continue;
        if (!path_ok || !Storage::remove(path)) {
            ok = false;
            continue;
        }
        deleted++;
    }
    dir.close();
    return ok;
}

bool ReportManager::load_result_json_cache_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    LargeTextBuffer &out) const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_json_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }
    return load_result_json_cache_path(path, out);
}

bool ReportManager::load_result_json_cache_path(const char *path,
                                                LargeTextBuffer &out) const {
    Storage::Guard g;
    out.clear();
    if (!path || !path[0] || !Storage::mounted() || !Storage::exists(path)) {
        return false;
    }
    File file = Storage::open(path, "r");
    if (!file) return false;
    const size_t size = static_cast<size_t>(file.size());
    if (size < 16 || size > REPORT_RESULT_JSON_CACHE_MAX ||
        !out.reserve(size + 1)) {
        file.close();
        out.clear();
        return false;
    }
    char buffer[512];
    while (file.available()) {
        const int n = file.read(reinterpret_cast<uint8_t *>(buffer),
                                sizeof(buffer));
        if (n < 0 || !out.append(buffer, static_cast<size_t>(n))) {
            file.close();
            out.clear();
            return false;
        }
        if (n == 0) break;
    }
    file.close();
    const char *json = out.c_str();
    if (out.length() != size || json[0] != '{') {
        out.clear();
        return false;
    }
    return true;
}

bool ReportManager::load_result_plot_cache_for_night(
    const ReportIndexedNight &night,
    const char *etag,
    ReportSpoolBuffer &out) const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }
    return load_result_plot_cache_path(path, out);
}

bool ReportManager::load_result_plot_cache_for_etag(uint64_t night_start_ms,
                                                    const char *etag,
                                                    ReportSpoolBuffer &out)
    const {
    char path[REPORT_CACHE_PATH_MAX];
    if (!result_plot_cache_path_for_etag(night_start_ms,
                                         etag,
                                         path,
                                         sizeof(path))) {
        return false;
    }
    return load_result_plot_cache_path(path, out);
}

bool ReportManager::load_result_plot_cache_path(const char *path,
                                                ReportSpoolBuffer &out) const {
    Storage::Guard g;
    out.clear();
    if (!path || !path[0] || !Storage::mounted() || !Storage::exists(path)) {
        return false;
    }
    File file = Storage::open(path, "r");
    if (!file) return false;
    const size_t size = static_cast<size_t>(file.size());
    out.set_max_size(size);
    if (size < 8 || !out.reserve_capacity(size)) {
        file.close();
        out.clear();
        return false;
    }
    uint8_t buffer[512];
    while (file.available()) {
        const int n = file.read(buffer, sizeof(buffer));
        if (n < 0 ||
            !out.append(buffer, static_cast<size_t>(n))) {
            file.close();
            out.clear();
            return false;
        }
        if (n == 0) break;
    }
    file.close();
    const uint8_t *d = out.data();
    const uint32_t magic = d ? (static_cast<uint32_t>(d[0]) |
                                (static_cast<uint32_t>(d[1]) << 8) |
                                (static_cast<uint32_t>(d[2]) << 16) |
                                (static_cast<uint32_t>(d[3]) << 24))
                             : 0u;
    // Reject on a PLOT_BIN_VERSION mismatch too (the cache dir name guards
    // format changes by hand; this guards a version bump that forgot the dir).
    const uint16_t version = d ? (static_cast<uint16_t>(d[4]) |
                                  (static_cast<uint16_t>(d[5]) << 8))
                               : 0u;
    if (out.size() != size || magic != PLOT_BIN_MAGIC ||
        version != PLOT_BIN_VERSION) {
        out.clear();
        return false;
    }
    return true;
}

bool ReportManager::load_result_plot_cache() {
    return load_result_plot_cache_for_night(result_indexed_night_,
                                            result_etag_,
                                            result_plot_bin_);
}

void ReportManager::reset_result_cache_write_locked() {
    if (plot_cache_write_.file) {
        Storage::Guard g;
        plot_cache_write_.file.close();
    }
    plot_cache_write_.file = File();
    plot_cache_write_.active = false;
    plot_cache_write_.phase = ResultCacheWritePhase::Idle;
    plot_cache_write_.night = ReportSummaryRecord{};
    plot_cache_write_.plot_path[0] = 0;
    plot_cache_write_.plot_tmp_path[0] = 0;
    plot_cache_write_.result_path[0] = 0;
    plot_cache_write_.result_tmp_path[0] = 0;
    plot_cache_write_.result_json.reset();
    plot_cache_write_.plot.reset();
    plot_cache_write_.offset = 0;
}

bool ReportManager::enqueue_result_cache_write(
    const ReportIndexedNight &night,
    const char *etag,
    const std::shared_ptr<ReportSpoolBuffer> &result_json,
    const std::shared_ptr<ReportSpoolBuffer> &plot) {
    if (!result_json || result_json->size() == 0 ||
        !plot || plot->size() == 0 || !plot_cache_write_lock_) {
        return false;
    }

    char path[sizeof(plot_cache_write_.plot_path)];
    if (!result_plot_cache_path_for_night(night, etag, path, sizeof(path))) {
        return false;
    }
    char tmp[sizeof(plot_cache_write_.plot_tmp_path)];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(tmp)) {
        return false;
    }
    char result_path[sizeof(plot_cache_write_.result_path)];
    if (!result_json_cache_path_for_night(night,
                                          etag,
                                          result_path,
                                          sizeof(result_path))) {
        return false;
    }
    char result_tmp[sizeof(plot_cache_write_.result_tmp_path)];
    const int result_written =
        snprintf(result_tmp, sizeof(result_tmp), "%s.tmp", result_path);
    if (result_written <= 0 ||
        static_cast<size_t>(result_written) >= sizeof(result_tmp)) {
        return false;
    }

    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    if (plot_cache_write_.active) {
        xSemaphoreGive(plot_cache_write_lock_);
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Plot cache write skipped: writer busy night=%llu\n",
                  static_cast<unsigned long long>(night.summary.start_ms));
        return false;
    }

    reset_result_cache_write_locked();
    plot_cache_write_.active = true;
    plot_cache_write_.phase = ResultCacheWritePhase::ClearOld;
    plot_cache_write_.night = night.summary;
    snprintf(plot_cache_write_.plot_path,
             sizeof(plot_cache_write_.plot_path),
             "%s",
             path);
    snprintf(plot_cache_write_.plot_tmp_path,
             sizeof(plot_cache_write_.plot_tmp_path),
             "%s",
             tmp);
    snprintf(plot_cache_write_.result_path,
             sizeof(plot_cache_write_.result_path),
             "%s",
             result_path);
    snprintf(plot_cache_write_.result_tmp_path,
             sizeof(plot_cache_write_.result_tmp_path),
             "%s",
             result_tmp);
    plot_cache_write_.result_json = result_json;
    plot_cache_write_.plot = plot;
    plot_cache_write_.offset = 0;
    xSemaphoreGive(plot_cache_write_lock_);
    if (BackgroundWorker *worker = background_worker()) {
        worker->wake();
    }
    return true;
}

bool ReportManager::plot_cache_writer_active() const {
    if (!plot_cache_write_lock_) return false;
    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(1)) != pdTRUE) {
        return true;
    }
    const bool active = plot_cache_write_.active;
    xSemaphoreGive(plot_cache_write_lock_);
    return active;
}

bool ReportManager::service_result_cache_writer() {
    if (!plot_cache_write_lock_) return false;
    if (xSemaphoreTake(plot_cache_write_lock_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }
    if (!plot_cache_write_.active) {
        xSemaphoreGive(plot_cache_write_lock_);
        return false;
    }

    bool ok = true;
    switch (plot_cache_write_.phase) {
        case ResultCacheWritePhase::ClearOld: {
            {
                Storage::Guard g;
                ok = Storage::mounted() &&
                     Storage::ensure_dir("/aircannect") &&
                     Storage::ensure_dir("/aircannect/report") &&
                     Storage::ensure_dir(REPORT_CACHE_BASE_DIR) &&
                     Storage::ensure_dir("/aircannect/report/v4/plots") &&
                     Storage::ensure_dir(REPORT_PLOT_CACHE_DIR) &&
                     Storage::ensure_dir("/aircannect/report/v4/results") &&
                     Storage::ensure_dir(REPORT_RESULT_JSON_CACHE_DIR);
            }
            uint32_t deleted_plot = 0;
            uint32_t deleted_result = 0;
            ok = ok && clear_plot_cache_for_night(plot_cache_write_.night,
                                                  deleted_plot);
            ok = ok && clear_result_json_cache_for_night(
                           plot_cache_write_.night,
                           deleted_result);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::OpenPlotTmp
                   : ResultCacheWritePhase::Idle;
            break;
        }
        case ResultCacheWritePhase::OpenPlotTmp: {
            Storage::Guard g;
            Storage::remove(plot_cache_write_.plot_tmp_path);
            plot_cache_write_.file =
                Storage::open(plot_cache_write_.plot_tmp_path, "w");
            ok = static_cast<bool>(plot_cache_write_.file);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::WritePlot
                   : ResultCacheWritePhase::Idle;
            break;
        }
        case ResultCacheWritePhase::WritePlot: {
            if (!plot_cache_write_.plot || !plot_cache_write_.file) {
                ok = false;
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
                break;
            }
            const size_t total = plot_cache_write_.plot->size();
            if (plot_cache_write_.offset >= total) {
                plot_cache_write_.phase =
                    ResultCacheWritePhase::ClosePlotRename;
                break;
            }
            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK,
                         total - plot_cache_write_.offset);
            const uint8_t *data =
                plot_cache_write_.plot->data() + plot_cache_write_.offset;
            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = plot_cache_write_.file.write(data, len);
            }
            ok = wrote == len;
            if (ok) {
                plot_cache_write_.offset += len;
                if (plot_cache_write_.offset >= total) {
                    plot_cache_write_.phase =
                        ResultCacheWritePhase::ClosePlotRename;
                }
            } else {
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }
        case ResultCacheWritePhase::ClosePlotRename: {
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            Storage::remove(plot_cache_write_.plot_path);
            ok = Storage::rename(plot_cache_write_.plot_tmp_path,
                                 plot_cache_write_.plot_path);
            if (!ok) {
                Storage::remove(plot_cache_write_.plot_tmp_path);
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            } else {
                plot_cache_write_.offset = 0;
                plot_cache_write_.phase =
                    ResultCacheWritePhase::OpenResultTmp;
            }
            break;
        }
        case ResultCacheWritePhase::OpenResultTmp: {
            Storage::Guard g;
            Storage::remove(plot_cache_write_.result_tmp_path);
            plot_cache_write_.file =
                Storage::open(plot_cache_write_.result_tmp_path, "w");
            ok = static_cast<bool>(plot_cache_write_.file);
            plot_cache_write_.phase =
                ok ? ResultCacheWritePhase::WriteResult
                   : ResultCacheWritePhase::Idle;
            break;
        }
        case ResultCacheWritePhase::WriteResult: {
            if (!plot_cache_write_.result_json || !plot_cache_write_.file) {
                ok = false;
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
                break;
            }
            const size_t total = plot_cache_write_.result_json->size();
            if (plot_cache_write_.offset >= total) {
                plot_cache_write_.phase =
                    ResultCacheWritePhase::CloseResultRename;
                break;
            }
            const size_t len =
                std::min(REPORT_CACHE_WRITE_CHUNK,
                         total - plot_cache_write_.offset);
            const uint8_t *data =
                plot_cache_write_.result_json->data() +
                plot_cache_write_.offset;
            size_t wrote = 0;
            {
                Storage::Guard g;
                wrote = plot_cache_write_.file.write(data, len);
            }
            ok = wrote == len;
            if (ok) {
                plot_cache_write_.offset += len;
                if (plot_cache_write_.offset >= total) {
                    plot_cache_write_.phase =
                        ResultCacheWritePhase::CloseResultRename;
                }
            } else {
                plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            }
            break;
        }
        case ResultCacheWritePhase::CloseResultRename: {
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            Storage::remove(plot_cache_write_.result_path);
            ok = Storage::rename(plot_cache_write_.result_tmp_path,
                                 plot_cache_write_.result_path);
            if (!ok) Storage::remove(plot_cache_write_.result_tmp_path);
            plot_cache_write_.phase = ResultCacheWritePhase::Idle;
            break;
        }
        case ResultCacheWritePhase::Idle:
        default:
            ok = false;
            break;
    }

    const bool done = plot_cache_write_.phase == ResultCacheWritePhase::Idle;
    const uint64_t night_start_ms = plot_cache_write_.night.start_ms;
    const size_t plot_bytes =
        plot_cache_write_.plot ? plot_cache_write_.plot->size() : 0;
    const size_t result_bytes =
        plot_cache_write_.result_json ? plot_cache_write_.result_json->size()
                                      : 0;
    if (done) {
        if (ok) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result cache write complete night=%llu result=%u "
                      "plot=%u\n",
                      static_cast<unsigned long long>(night_start_ms),
                      static_cast<unsigned>(result_bytes),
                      static_cast<unsigned>(plot_bytes));
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "Result cache write failed night=%llu result=%u "
                      "plot=%u\n",
                      static_cast<unsigned long long>(night_start_ms),
                      static_cast<unsigned>(result_bytes),
                      static_cast<unsigned>(plot_bytes));
            Storage::Guard g;
            if (plot_cache_write_.file) plot_cache_write_.file.close();
            if (plot_cache_write_.plot_tmp_path[0]) {
                Storage::remove(plot_cache_write_.plot_tmp_path);
            }
            if (plot_cache_write_.result_tmp_path[0]) {
                Storage::remove(plot_cache_write_.result_tmp_path);
            }
        }
        reset_result_cache_write_locked();
    }
    xSemaphoreGive(plot_cache_write_lock_);
    return true;
}

void ReportManager::reset_plot_build() {
    plot_build_active_ = false;
    plot_build_idle_prebuild_ = false;
    plot_build_night_start_ms_.store(0);
    plot_build_phase_ = ReportPlotBuildPhase::Idle;
    plot_build_bin_.clear();
    plot_tmp_.clear();
    plot_bin_ok_ = true;
    memset(plot_ranges_, 0, sizeof(plot_ranges_));
    plot_range_count_ = 0;
    plot_start_ms_ = 0;
    plot_end_ms_ = 0;
    plot_bucket_ms_ = 1;
    plot_chunk_index_ = 0;
    memset(plot_chunk_done_, 0, sizeof(plot_chunk_done_));
    plot_seen_events_.clear();
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        plot_series_states_[i].reset();
    }
    plot_build_started_ms_ = 0;
    plot_build_input_chunks_ = 0;
    plot_build_input_bytes_ = 0;
}

void ReportManager::build_empty_plot_bin(ReportSpoolBuffer &out) const {
    out.clear();
    out.set_max_size(32);
    bin_put_u32(out, PLOT_BIN_MAGIC);
    bin_put_u16(out, PLOT_BIN_VERSION);
    bin_put_u16(out, 0);   // flags
    bin_put_i64(out, 0);   // base_ms
    bin_put_u32(out, 0);   // event count; no series follow
}

bool ReportManager::plot_time_in_ranges(int64_t timestamp_ms) const {
    return plot_range_index(timestamp_ms) >= 0;
}

int ReportManager::plot_range_index(int64_t timestamp_ms) const {
    for (size_t i = 0; i < plot_range_count_; ++i) {
        if (timestamp_ms >= plot_ranges_[i].start_ms &&
            timestamp_ms < plot_ranges_[i].end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReportManager::start_result_plot_build() {
    reset_plot_build();
    build_empty_plot_bin(result_plot_bin_);
    if (result_status_.state == ReportResultState::Error ||
        !result_chunks_ || result_status_.chunk_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }

    const size_t range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    if (range_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }

    plot_range_count_ = range_count;
    plot_start_ms_ = result_ranges_[0].start_ms;
    plot_end_ms_ = result_ranges_[0].end_ms;
    for (size_t i = 0; i < range_count; ++i) {
        plot_ranges_[i] = result_ranges_[i];
        plot_start_ms_ = std::min(plot_start_ms_, result_ranges_[i].start_ms);
        plot_end_ms_ = std::max(plot_end_ms_, result_ranges_[i].end_ms);
    }
    if (plot_start_ms_ <= 0 || plot_end_ms_ <= plot_start_ms_) {
        build_empty_plot_bin(result_plot_bin_);
        return publish_result_to_slot();
    }
    if (!result_skip_plot_cache_ && load_result_plot_cache()) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache hit index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        return publish_result_to_slot();
    }
    if (result_skip_plot_cache_) {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result plot cache skipped index=%lu reason=refresh\n",
                  static_cast<unsigned long>(result_status_.therapy_index));
    }

    const int64_t span_ms = plot_end_ms_ - plot_start_ms_;
    plot_bucket_ms_ = std::max<int64_t>(
        1, span_ms / static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS));
    plot_build_bin_.clear();
    plot_build_bin_.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    plot_tmp_.clear();
    plot_tmp_.set_max_size(128 * 1024);
    plot_bin_ok_ = true;
    if (!plot_build_bin_.reserve_capacity(AC_REPORT_PLOT_INITIAL_RESERVE)) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_bin_ok_ &= bin_put_u32(plot_build_bin_, PLOT_BIN_MAGIC);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, PLOT_BIN_VERSION);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, 0);          // flags
    plot_bin_ok_ &= bin_put_i64(plot_build_bin_, plot_start_ms_);  // base_ms
    if (!plot_bin_ok_) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_build_active_ = true;
    plot_build_idle_prebuild_ = active_build_idle_prebuild_;
    plot_build_started_ms_ = millis();
    plot_build_input_chunks_ = 0;
    plot_build_input_bytes_ = 0;
    plot_build_night_start_ms_.store(result_night_.start_ms);
    plot_build_phase_ = ReportPlotBuildPhase::Events;
    return true;
}

bool ReportManager::process_plot_event_chunk(const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!read_result_chunk_payload(chunk, meta, payload)) {
        fail_result_prepare("plot_event_read_failed");
        return false;
    }
    const size_t count = payload.size() / report_event_record_wire_size();
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        const int64_t duration_ms = event.duration_ms > 0
                                        ? static_cast<int64_t>(
                                              event.duration_ms)
                                        : 0;
        const int64_t event_end_ms = event.start_ms + duration_ms;
        const bool in_range = duration_ms > 0
                                  ? plot_time_in_ranges(event.start_ms) ||
                                        plot_time_in_ranges(event_end_ms - 1)
                                  : plot_time_in_ranges(event.start_ms);
        if (!in_range) continue;
        if (report_event_seen(plot_seen_events_, event)) continue;
        if (!remember_report_event(plot_seen_events_, event)) {
            fail_result_prepare("plot_event_dedupe_failed");
            return false;
        }
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.start_ms - plot_start_ms_));
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.duration_ms));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_, static_cast<int32_t>(event.code));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_,
                                    static_cast<int32_t>(event.flags));
    }
    return true;
}

bool ReportManager::open_plot_series_state(size_t stream_index) {
    if (stream_index >= result_stream_count_ ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return false;
    }
    PlotSeriesBuildState &state = plot_series_states_[stream_index];
    if (state.open) return true;
    state.reset();
    state.points.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    state.open = true;
    return plot_bin_ok_;
}

uint8_t ReportManager::flush_plot_bucket_to(ReportSpoolBuffer &out,
                                            PlotBuildBucket &bucket,
                                            int64_t base_ms,
                                            bool &ok) {
    if (!bucket.have) return 0;
    struct PlotPoint {
        int64_t t = 0;
        int32_t value = 0;
    };
    PlotPoint points[4] = {
        {bucket.start_t, bucket.start_value},
        {bucket.min_t, bucket.min_value},
        {bucket.max_t, bucket.max_value},
        {bucket.end_t, bucket.end_value},
    };
    std::sort(points,
              points + 4,
              [](const PlotPoint &a, const PlotPoint &b) {
                  return a.t < b.t;
              });
    bool emitted[4] = {};
    uint8_t count = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;
        ok &= bin_put_i32(out, static_cast<int32_t>(points[i].t - base_ms));
        ok &= bin_put_i32(out, points[i].value);
        count++;
        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }

    bucket.clear();
    return count;
}

uint8_t ReportManager::emit_plot_gap_to(ReportSpoolBuffer &out,
                                        PlotBuildBucket &bucket,
                                        int64_t base_ms,
                                        bool &ok) {
    uint8_t count = flush_plot_bucket_to(out, bucket, base_ms, ok);
    ok &= bin_put_i32(out, PLOT_POINT_GAP_DELTA);
    ok &= bin_put_i32(out, 0);
    return static_cast<uint8_t>(count + 1);
}

void ReportManager::flush_plot_bucket(PlotSeriesBuildState &state) {
    if (!state.bucket.have) return;
    if (state.current_bucket < 0 ||
        state.current_bucket > PLOT_ENVELOPE_GAP_BUCKET - 1) {
        plot_bin_ok_ = false;
        state.bucket.clear();
        return;
    }
    plot_bin_ok_ &=
        bin_put_u32(state.points, static_cast<uint32_t>(state.current_bucket));
    int32_t min_value = state.bucket.min_value;
    int32_t max_value = state.bucket.max_value;
    if (min_value > max_value) std::swap(min_value, max_value);
    plot_bin_ok_ &= bin_put_i32(state.points, min_value);
    plot_bin_ok_ &= bin_put_i32(state.points, max_value);
    state.bucket.clear();
}

bool ReportManager::append_plot_series_value(PlotSeriesBuildState &state,
                                             int64_t timestamp_ms,
                                             int32_t value_milli,
                                             int64_t bucket_ms) {
    if (timestamp_ms < plot_start_ms_) return true;
    if (bucket_ms <= 0) bucket_ms = 1;
    if (state.series_bucket_ms <= 0) {
        state.series_bucket_ms = bucket_ms;
    } else {
        bucket_ms = state.series_bucket_ms;
    }

    int64_t sample_bucket = state.current_bucket;
    if (state.current_bucket < 0 ||
        state.current_bucket_ms != bucket_ms ||
        timestamp_ms < state.current_bucket_start_ms ||
        timestamp_ms >= state.current_bucket_end_ms) {
        sample_bucket = (timestamp_ms - plot_start_ms_) / bucket_ms;
        if (sample_bucket < 0) sample_bucket = 0;
    }
    if (state.current_bucket != sample_bucket ||
        state.current_bucket_ms != bucket_ms) {
        flush_plot_bucket(state);
        state.current_bucket = sample_bucket;
        state.current_bucket_ms = bucket_ms;
        state.current_bucket_start_ms =
            plot_start_ms_ + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    } else if (state.current_bucket_start_ms == 0 ||
               state.current_bucket_end_ms == 0) {
        state.current_bucket_start_ms =
            plot_start_ms_ + sample_bucket * bucket_ms;
        state.current_bucket_end_ms =
            state.current_bucket_start_ms + bucket_ms;
    }

    if (!state.bucket.have) {
        state.bucket.have = true;
        state.bucket.start_t = timestamp_ms;
        state.bucket.end_t = timestamp_ms;
        state.bucket.min_t = timestamp_ms;
        state.bucket.max_t = timestamp_ms;
        state.bucket.start_value = value_milli;
        state.bucket.end_value = value_milli;
        state.bucket.min_value = value_milli;
        state.bucket.max_value = value_milli;
    } else {
        state.bucket.end_t = timestamp_ms;
        state.bucket.end_value = value_milli;
        if (value_milli < state.bucket.min_value) {
            state.bucket.min_value = value_milli;
            state.bucket.min_t = timestamp_ms;
        }
        if (value_milli > state.bucket.max_value) {
            state.bucket.max_value = value_milli;
            state.bucket.max_t = timestamp_ms;
        }
    }
    return plot_bin_ok_;
}

bool ReportManager::process_plot_series_sample_value(
    PlotSeriesBuildState &state,
    const ReportResultChunk &chunk,
    const ReportSeriesSample &sample,
    uint32_t interval_ms) {
    int range_index = -1;
    if (state.last_range_index >= 0 &&
        static_cast<size_t>(state.last_range_index) < plot_range_count_) {
        const PlotRange &last_range =
            plot_ranges_[state.last_range_index];
        if (sample.timestamp_ms >= last_range.start_ms &&
            sample.timestamp_ms < last_range.end_ms) {
            range_index = state.last_range_index;
        }
    }
    if (range_index < 0) {
        range_index = plot_range_index(sample.timestamp_ms);
    }
    if (range_index < 0) {
        return true;
    }
    if (state.have_last_sample &&
        (range_index != state.last_range_index ||
         sample.timestamp_ms >
             state.last_sample_ms + plot_gap_threshold_ms(interval_ms))) {
        if (!append_plot_series_gap(state)) return false;
    }
    const int64_t bucket_ms =
        plot_bucket_ms_for_signal(chunk.signal,
                                  chunk.source,
                                  plot_bucket_ms_,
                                  interval_ms,
                                  false);
    int32_t value_milli = sample.value_milli;
    if ((chunk.signal == ReportSignalId::Flow &&
         chunk.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (chunk.signal == ReportSignalId::Leak &&
         chunk.source == ReportSourceId::Leak0p5Hz)) {
        const int64_t scaled = static_cast<int64_t>(value_milli) * 60LL;
        value_milli = scaled > INT32_MAX
                          ? INT32_MAX
                          : (scaled < INT32_MIN
                                 ? INT32_MIN
                                 : static_cast<int32_t>(scaled));
    }
    if (!append_plot_series_value(state,
                                  sample.timestamp_ms,
                                  value_milli,
                                  bucket_ms)) {
        return false;
    }
    state.bucket.range_index = range_index;
    state.have_last_sample = true;
    state.last_sample_ms = sample.timestamp_ms;
    state.last_range_index = range_index;
    return true;
}

bool ReportManager::append_plot_series_point(PlotSeriesBuildState &state,
                                             int64_t timestamp_ms,
                                             int32_t value_milli,
                                             int64_t bucket_ms) {
    if (!append_plot_series_value(state,
                                  timestamp_ms,
                                  value_milli,
                                  bucket_ms)) {
        return false;
    }
    state.have_last_sample = true;
    state.last_sample_ms = timestamp_ms;
    return plot_bin_ok_;
}

bool ReportManager::append_plot_series_gap(PlotSeriesBuildState &state) {
    flush_plot_bucket(state);
    plot_bin_ok_ &= bin_put_u32(state.points, PLOT_ENVELOPE_GAP_BUCKET);
    plot_bin_ok_ &= bin_put_i32(state.points, 0);
    plot_bin_ok_ &= bin_put_i32(state.points, 0);
    state.have_last_sample = false;
    state.last_sample_ms = 0;
    state.last_range_index = -1;
    state.current_bucket = -1;
    state.current_bucket_start_ms = 0;
    state.current_bucket_end_ms = 0;
    state.current_bucket_ms = 0;
    state.bucket.clear();
    return plot_bin_ok_;
}

bool ReportManager::process_plot_series_chunk(size_t chunk_index) {
    if (chunk_index >= result_status_.chunk_count ||
        chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail_result_prepare("plot_bad_chunk");
        return false;
    }
    const ReportResultChunk &chunk = result_chunks_[chunk_index];
    if (chunk.stream_index >= result_stream_count_ ||
        chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
        !open_plot_series_state(chunk.stream_index)) {
        fail_result_prepare("plot_series_open_failed");
        return false;
    }
    struct PlotSeriesContext {
        ReportManager *manager = nullptr;
        const ReportResultChunk *chunk = nullptr;
        PlotSeriesBuildState *state = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        uint32_t interval_ms = 0;
    };
    PlotSeriesContext ctx;
    ctx.manager = this;
    ctx.chunk = &chunk;
    ctx.state = &plot_series_states_[chunk.stream_index];
    ctx.interval_ms =
        infer_chunk_interval_ms(chunk.record_count, chunk.start_ms, chunk.end_ms);
    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;
    const bool ok = for_each_result_series_sample(
        chunk,
        chunk.stream_index,
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            PlotSeriesContext *ctx =
                static_cast<PlotSeriesContext *>(context);
            ReportManager *manager = ctx ? ctx->manager : nullptr;
            const ReportResultChunk *chunk = ctx ? ctx->chunk : nullptr;
            PlotSeriesBuildState *state = ctx ? ctx->state : nullptr;
            if (!manager || !chunk || !state) return false;
            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;
            return manager->process_plot_series_sample_value(*state,
                                                             *chunk,
                                                             sample,
                                                             interval_ms);
        },
        &ctx);
    if (!ok) {
        fail_result_prepare("plot_series_decode_failed");
        return false;
    }
    (void)read_stats;
    return true;
}

bool ReportManager::process_plot_edf_series_batch(size_t seed_chunk_index,
                                                  bool &processed) {
    processed = false;
    if (!result_chunks_ || seed_chunk_index >= result_status_.chunk_count ||
        seed_chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail_result_prepare("plot_bad_chunk");
        return false;
    }
    const ReportResultChunk &seed = result_chunks_[seed_chunk_index];
    if (seed.kind != ReportStoreChunkKind::Series ||
        seed.provider_ref.provider != ReportProviderId::Edf ||
        plot_chunk_done_[seed_chunk_index]) {
        return true;
    }

    const size_t max_chunks = std::min(
        static_cast<size_t>(result_status_.chunk_count),
        static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));
    if (max_chunks == 0) return true;
    const size_t candidate_capacity =
        max_chunks *
        std::min(result_stream_count_,
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    if (candidate_capacity == 0) return true;
    ReportProviderChunk *candidates =
        static_cast<ReportProviderChunk *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportProviderChunk), false));
    ReportResultChunk *logical_chunks =
        static_cast<ReportResultChunk *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportResultChunk), false));
    size_t *chunk_indices = static_cast<size_t *>(Memory::calloc_large(
        candidate_capacity, sizeof(size_t), false));
    uint8_t *stream_indices = static_cast<uint8_t *>(Memory::calloc_large(
        candidate_capacity, sizeof(uint8_t), false));
    bool *selected = static_cast<bool *>(Memory::calloc_large(
        candidate_capacity, sizeof(bool), false));
    bool *physical_counted = static_cast<bool *>(Memory::calloc_large(
        max_chunks, sizeof(bool), false));
    ReportProviderSeriesReadStats *stats =
        static_cast<ReportProviderSeriesReadStats *>(Memory::calloc_large(
            candidate_capacity, sizeof(ReportProviderSeriesReadStats), false));
    EdfReportSeriesPlotConfig *plot_configs =
        static_cast<EdfReportSeriesPlotConfig *>(Memory::calloc_large(
            candidate_capacity, sizeof(EdfReportSeriesPlotConfig), false));
    if (!candidates || !logical_chunks || !chunk_indices ||
        !stream_indices || !selected || !physical_counted || !stats ||
        !plot_configs) {
        if (candidates) Memory::free(candidates);
        if (logical_chunks) Memory::free(logical_chunks);
        if (chunk_indices) Memory::free(chunk_indices);
        if (stream_indices) Memory::free(stream_indices);
        if (selected) Memory::free(selected);
        if (physical_counted) Memory::free(physical_counted);
        if (stats) Memory::free(stats);
        if (plot_configs) Memory::free(plot_configs);
        fail_result_prepare("plot_alloc_failed");
        return false;
    }

    EdfReportPlotRange fast_ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    const size_t fast_range_count =
        std::min(plot_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < fast_range_count; ++i) {
        fast_ranges[i].start_ms = plot_ranges_[i].start_ms;
        fast_ranges[i].end_ms = plot_ranges_[i].end_ms;
    }

    size_t candidate_count = 0;
    auto add_candidate = [&](size_t index, size_t stream_index) {
        const ReportResultChunk &chunk = result_chunks_[index];
        if (plot_chunk_done_[index] ||
            chunk.kind != ReportStoreChunkKind::Series ||
            chunk.provider_ref.provider != ReportProviderId::Edf ||
            stream_index >= result_stream_count_ ||
            stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
            !result_chunk_has_stream(chunk, stream_index) ||
            candidate_count >= candidate_capacity) {
            return;
        }
        if (!provider_chunk_from_result_stream(chunk,
                                               stream_index,
                                               result_streams_,
                                               result_stream_count_,
                                               result_edf_sessions_,
                                               result_edf_session_count_,
                                               candidates[candidate_count])) {
            return;
        }
        ReportResultChunk &logical = logical_chunks[candidate_count];
        logical = chunk;
        logical.provider_ref = candidates[candidate_count].ref;
        logical.source = candidates[candidate_count].source;
        logical.signal = candidates[candidate_count].signal;
        logical.name = candidates[candidate_count].name;
        logical.stream_index = static_cast<uint8_t>(stream_index);
        logical.stream_mask = 0;
        logical.start_ms = candidates[candidate_count].start_ms;
        logical.end_ms = candidates[candidate_count].end_ms;
        logical.record_count = candidates[candidate_count].record_count;
        logical.payload_len = candidates[candidate_count].payload_len;
        logical.payload_schema = candidates[candidate_count].payload_schema;
        const uint32_t interval_ms = infer_chunk_interval_ms(
            logical.record_count, logical.start_ms, logical.end_ms);
        plot_configs[candidate_count].ranges = fast_ranges;
        plot_configs[candidate_count].range_count = fast_range_count;
        plot_configs[candidate_count].plot_start_ms = plot_start_ms_;
        plot_configs[candidate_count].bucket_ms =
            static_cast<uint32_t>(std::min<int64_t>(
                UINT32_MAX,
                plot_bucket_ms_for_signal(logical.signal,
                                          logical.source,
                                          plot_bucket_ms_,
                                          interval_ms,
                                          false)));
        plot_configs[candidate_count].gap_threshold_ms =
            static_cast<uint32_t>(std::min<int64_t>(
                UINT32_MAX, plot_gap_threshold_ms(interval_ms)));
        plot_configs[candidate_count].value_multiplier =
            plot_value_multiplier(logical.signal, logical.source);
        chunk_indices[candidate_count] = index;
        stream_indices[candidate_count] = static_cast<uint8_t>(stream_index);
        ++candidate_count;
    };
    for (size_t stream_index = 0; stream_index < result_stream_count_ &&
                                  stream_index < AC_REPORT_RESULT_STREAM_MAX;
         ++stream_index) {
        add_candidate(seed_chunk_index, stream_index);
    }
    for (size_t i = 0; i < max_chunks; ++i) {
        if (i == seed_chunk_index) continue;
        for (size_t stream_index = 0;
             stream_index < result_stream_count_ &&
             stream_index < AC_REPORT_RESULT_STREAM_MAX;
             ++stream_index) {
            add_candidate(i, stream_index);
        }
    }
    if (candidate_count == 0) {
        Memory::free(plot_configs);
        Memory::free(stats);
        Memory::free(physical_counted);
        Memory::free(selected);
        Memory::free(stream_indices);
        Memory::free(chunk_indices);
        Memory::free(logical_chunks);
        Memory::free(candidates);
        return true;
    }

    struct EdfBatchContext {
        ReportManager *manager = nullptr;
        const size_t *chunk_indices = nullptr;
        const uint8_t *stream_indices = nullptr;
        const ReportResultChunk *logical_chunks = nullptr;
        const EdfReportSeriesPlotConfig *plot_configs = nullptr;
        size_t chunk_count = 0;
        uint32_t points = 0;
    };
    EdfBatchContext ctx;
    ctx.manager = this;
    ctx.chunk_indices = chunk_indices;
    ctx.stream_indices = stream_indices;
    ctx.logical_chunks = logical_chunks;
    ctx.plot_configs = plot_configs;
    ctx.chunk_count = candidate_count;
    bool ok = false;
    if (!edf_report_signal_uses_edge_zero_padding(seed.signal)) {
        ok = edf_report_provider().for_each_compatible_series_plot_batch(
            candidates,
            candidate_count,
            plot_configs,
            selected,
            result_edf_sessions_,
            result_edf_session_count_,
            stats,
            [](void *context,
               size_t candidate_index,
               const EdfReportSeriesPlotPoint &point) -> bool {
                EdfBatchContext *ctx =
                    static_cast<EdfBatchContext *>(context);
                ReportManager *manager = ctx ? ctx->manager : nullptr;
                if (!manager || !ctx->chunk_indices ||
                    !ctx->stream_indices || !ctx->logical_chunks ||
                    !ctx->plot_configs ||
                    candidate_index >= ctx->chunk_count) {
                    return false;
                }
                const size_t chunk_index =
                    ctx->chunk_indices[candidate_index];
                if (chunk_index >= manager->result_status_.chunk_count ||
                    chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
                    return false;
                }
                const size_t stream_index =
                    ctx->stream_indices[candidate_index];
                if (stream_index >= manager->result_stream_count_ ||
                    stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
                    !manager->open_plot_series_state(stream_index)) {
                    return false;
                }
                PlotSeriesBuildState &state =
                    manager->plot_series_states_[stream_index];
                if (point.gap) {
                    ctx->points++;
                    return manager->append_plot_series_gap(state);
                }
                const EdfReportSeriesPlotConfig &config =
                    ctx->plot_configs[candidate_index];
                const int64_t decimated_gap_threshold_ms =
                    std::max<int64_t>(
                        static_cast<int64_t>(config.gap_threshold_ms),
                        static_cast<int64_t>(config.bucket_ms) * 2LL);
                if (state.have_last_sample &&
                    point.timestamp_ms >
                        state.last_sample_ms + decimated_gap_threshold_ms) {
                    if (!manager->append_plot_series_gap(state)) return false;
                }
                ctx->points++;
                return manager->append_plot_series_point(state,
                                                         point.timestamp_ms,
                                                         point.value_milli,
                                                         config.bucket_ms);
            },
            &ctx);
    }

    if (ok) {
        for (size_t i = 0; i < candidate_count; ++i) {
            if (!selected[i]) continue;
            const size_t chunk_index = chunk_indices[i];
            plot_chunk_done_[chunk_index] = true;
            if (chunk_index < max_chunks && !physical_counted[chunk_index]) {
                physical_counted[chunk_index] = true;
                plot_build_input_chunks_++;
                plot_build_input_bytes_ +=
                    result_chunks_[chunk_index].payload_len;
            }
            processed = true;
        }
    } else if (ctx.points == 0 && plot_bin_ok_) {
        ok = edf_report_provider().for_each_compatible_series_sample_batch(
            candidates,
            candidate_count,
            selected,
            result_edf_sessions_,
            result_edf_session_count_,
            stats,
            [](void *context,
               size_t candidate_index,
               const ReportSeriesSample &sample) -> bool {
                EdfBatchContext *ctx = static_cast<EdfBatchContext *>(context);
                ReportManager *manager = ctx ? ctx->manager : nullptr;
                if (!manager || !ctx->chunk_indices ||
                    candidate_index >= ctx->chunk_count) {
                    return false;
                }
                const size_t chunk_index = ctx->chunk_indices[candidate_index];
                if (chunk_index >= manager->result_status_.chunk_count ||
                    chunk_index >= AC_REPORT_RESULT_CHUNK_MAX) {
                    return false;
                }
                const size_t stream_index =
                    ctx->stream_indices[candidate_index];
                const ReportResultChunk &logical =
                    ctx->logical_chunks[candidate_index];
                if (stream_index >= manager->result_stream_count_ ||
                    stream_index >= AC_REPORT_RESULT_STREAM_MAX ||
                    !manager->open_plot_series_state(stream_index)) {
                    return false;
                }
                const uint32_t interval_ms = infer_chunk_interval_ms(
                    logical.record_count, logical.start_ms, logical.end_ms);
                return manager->process_plot_series_sample_value(
                    manager->plot_series_states_[stream_index],
                    logical,
                    sample,
                    interval_ms);
            },
            &ctx);

        if (ok) {
            for (size_t i = 0; i < candidate_count; ++i) {
                if (!selected[i]) continue;
                const size_t chunk_index = chunk_indices[i];
                plot_chunk_done_[chunk_index] = true;
                if (chunk_index < max_chunks &&
                    !physical_counted[chunk_index]) {
                    physical_counted[chunk_index] = true;
                    plot_build_input_chunks_++;
                    plot_build_input_bytes_ +=
                        result_chunks_[chunk_index].payload_len;
                }
                processed = true;
            }
        }
    }

    Memory::free(plot_configs);
    Memory::free(stats);
    Memory::free(physical_counted);
    Memory::free(selected);
    Memory::free(stream_indices);
    Memory::free(chunk_indices);
    Memory::free(logical_chunks);
    Memory::free(candidates);
    if (!ok || !processed || !plot_bin_ok_) {
        fail_result_prepare(ok && processed ? "plot_overflow"
                                            : "plot_series_decode_failed");
        return false;
    }
    return true;
}

bool ReportManager::finish_plot_series(size_t stream_index) {
    if (stream_index >= result_stream_count_ ||
        stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
        return true;
    }
    const ReportResultStream &stream = result_streams_[stream_index];
    PlotSeriesBuildState &state = plot_series_states_[stream_index];
    if (stream.kind != ReportStoreChunkKind::Series || !state.open) {
        state.reset();
        return true;
    }
    flush_plot_bucket(state);
    if (state.points.size()) {
        const int64_t bucket_ms =
            state.series_bucket_ms > 0 ? state.series_bucket_ms
                                       : plot_bucket_ms_;
        if (!append_plot_series_envelope_runs(plot_build_bin_,
                                              stream.name,
                                              state.points,
                                              bucket_ms,
                                              plot_bin_ok_)) {
            fail_result_prepare("plot_overflow");
            return false;
        }
    }
    state.reset();
    return true;
}

bool ReportManager::finish_result_plot_build() {
    if (!plot_bin_ok_ || plot_build_bin_.size() == 0) {
        fail_result_prepare("plot_overflow");
        return false;
    }
    const size_t len = plot_build_bin_.size();
    result_plot_bin_.clear();
    result_plot_bin_.set_max_size(len);
    if (!result_plot_bin_.reserve_capacity(len) ||
        !result_plot_bin_.append(plot_build_bin_.data(), len)) {
        fail_result_prepare("plot_publish_failed");
        return false;
    }
    result_status_.state = settled_result_state(result_status_.missing_required);
    result_status_.error.clear();
    if (!publish_result_to_slot(true)) {
        reset_plot_build();
        release_result_edf_sessions();
        return false;
    }
    const uint32_t elapsed_ms =
        plot_build_started_ms_ ? static_cast<uint32_t>(millis() -
                                                       plot_build_started_ms_)
                               : 0;
    const uint32_t input_chunks = plot_build_input_chunks_;
    const uint32_t input_bytes = plot_build_input_bytes_;
    reset_plot_build();
    release_result_edf_sessions();
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result plot ready index=%lu chunks=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(result_status_.therapy_index),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(input_chunks),
              static_cast<unsigned long>(input_bytes),
              static_cast<unsigned long>(result_plot_bin_.size()),
              static_cast<unsigned long>(elapsed_ms));
    return true;
}

void ReportManager::reset_range_plot_build(bool clear_ready) {
    range_build_active_ = false;
    range_build_phase_ = ReportPlotBuildPhase::Idle;
    range_build_index_ = 0;
    range_build_from_ = 0;
    range_build_to_ = 0;
    range_night_start_ms_ = 0;
    range_chunk_count_ = 0;
    range_stream_count_ = 0;
    range_edf_session_count_ = 0;
    range_range_count_ = 0;
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        range_ranges_[i] = PlotRange{};
    }
    range_build_bytes_.reset();
    range_tmp_.clear();
    range_seen_events_.clear();
    range_event_count_ = 0;
    range_chunk_index_ = 0;
    range_stream_index_ = 0;
    range_series_open_ = false;
    range_series_points_ = 0;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    range_last_range_index_ = -1;
    range_bucket_ms_ = 1;
    range_current_bucket_ = -1;
    range_bucket_.clear();
    range_build_ok_ = true;
    range_build_started_ms_ = 0;
    range_build_input_chunks_ = 0;
    range_build_input_bytes_ = 0;

    if (!clear_ready) return;
    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
        xSemaphoreGive(result_slots_lock_);
    } else {
        range_req_active_ = false;
        range_req_night_start_ms_ = 0;
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }
}

bool ReportManager::ensure_range_build_buffers() {
    if (!range_indexed_night_) {
        range_indexed_night_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
        if (!range_indexed_night_) {
            log_report_alloc_failed("range_indexed_night",
                                    sizeof(ReportIndexedNight));
            return false;
        }
    }
    if (!range_chunks_) {
        range_chunks_ = static_cast<ReportResultChunk *>(
            Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                                 sizeof(ReportResultChunk),
                                 false));
        if (!range_chunks_) {
            log_report_alloc_failed(
                "range_chunks",
                AC_REPORT_RESULT_CHUNK_MAX * sizeof(ReportResultChunk));
            return false;
        }
    }
    if (!range_edf_sessions_) {
        range_edf_sessions_ = static_cast<EdfReportSessionDescriptor *>(
            Memory::calloc_large(AC_REPORT_EDF_SESSION_MAX,
                                 sizeof(EdfReportSessionDescriptor),
                                 false));
        if (!range_edf_sessions_) {
            log_report_alloc_failed(
                "range_edf_sessions",
                AC_REPORT_EDF_SESSION_MAX *
                    sizeof(EdfReportSessionDescriptor));
            return false;
        }
    }
    return true;
}

bool ReportManager::read_range_chunk_payload(
    const ReportResultChunk &chunk,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload) {
    ReportProviderChunk provider_chunk;
    provider_chunk.ref = chunk.provider_ref;
    provider_chunk.kind = chunk.kind;
    provider_chunk.source = chunk.source;
    provider_chunk.signal = chunk.signal;
    provider_chunk.name = chunk.name;
    provider_chunk.start_ms = chunk.start_ms;
    provider_chunk.end_ms = chunk.end_ms;
    provider_chunk.payload_schema = chunk.payload_schema;
    provider_chunk.record_count = chunk.record_count;
    provider_chunk.payload_len = chunk.payload_len;

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().read_chunk(
                provider_chunk,
                static_cast<int64_t>(range_night_start_ms_),
                meta,
                payload);
        case ReportProviderId::Edf:
            return edf_report_provider().read_chunk(provider_chunk,
                                                   range_edf_sessions_,
                                                   range_edf_session_count_,
                                                   meta,
                                                   payload);
        default:
            return false;
    }
}

bool ReportManager::for_each_range_series_sample(
    const ReportResultChunk &chunk,
    size_t stream_index,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context) {
    stats = {};
    if (!callback || chunk.kind != ReportStoreChunkKind::Series) {
        return false;
    }
    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           range_streams_,
                                           range_stream_count_,
                                           range_edf_sessions_,
                                           range_edf_session_count_,
                                           provider_chunk)) {
        return false;
    }

    switch (chunk.provider_ref.provider) {
        case ReportProviderId::Spool:
            return spool_report_provider().for_each_series_sample(
                provider_chunk,
                static_cast<int64_t>(range_night_start_ms_),
                stats,
                callback,
                context);
        case ReportProviderId::Edf:
            return edf_report_provider().for_each_series_sample(
                provider_chunk,
                range_edf_sessions_,
                range_edf_session_count_,
                stats,
                callback,
                context);
        default:
            return false;
    }
}

bool ReportManager::start_range_plot_build(uint64_t night_start_ms,
                                           size_t therapy_index_hint,
                                           int64_t from_ms,
                                           int64_t to_ms,
                                           bool &waiting_for_result) {
    waiting_for_result = false;
    reset_range_plot_build(false);
    if (to_ms <= from_ms) return false;
    if (!ensure_range_build_buffers()) return false;

    size_t therapy_index = therapy_index_hint;
    memset(range_indexed_night_, 0, sizeof(*range_indexed_night_));
    if (!indexed_night_by_start(night_start_ms,
                                *range_indexed_night_,
                                &therapy_index)) {
        return false;
    }
    ReportIndexedNight &indexed_night = *range_indexed_night_;

    range_edf_session_count_ = 0;
    bool edf_pending = false;
    memset(range_edf_sessions_,
           0,
           AC_REPORT_EDF_SESSION_MAX *
               sizeof(EdfReportSessionDescriptor));
    bool have_edf =
        collect_edf_sessions_for_night(indexed_night.summary,
                                       from_ms,
                                       to_ms,
                                       range_edf_sessions_,
                                       AC_REPORT_EDF_SESSION_MAX,
                                       range_edf_session_count_,
                                       &edf_pending);
    if (edf_pending) {
        range_edf_session_count_ = 0;
        waiting_for_result = true;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Range plot waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<long long>(from_ms),
                  static_cast<long long>(to_ms));
        return false;
    }
    ScopedReportResolveContext resolve("range_resolver", false);
    if (!resolve) return false;

    EdfReportDataProvider edf_provider(have_edf ? range_edf_sessions_
                                                : nullptr,
                                       have_edf ? range_edf_session_count_
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  resolve.scratch());
    ReportResolvedPlan &plan = resolve.plan();
    if (!resolver.build_plan(indexed_night, from_ms, to_ms, plan)) {
        return false;
    }
    if (!materialize_range_plan(indexed_night, plan)) {
        return false;
    }

    range_build_index_ = therapy_index;
    range_build_from_ = from_ms;
    range_build_to_ = to_ms;
    range_bucket_ms_ = std::max<int64_t>(
        1,
        (to_ms - from_ms) /
            static_cast<int64_t>(AC_REPORT_RANGE_PLOT_BUCKETS));
    range_build_bytes_ = std::make_shared<ReportSpoolBuffer>();
    if (!range_build_bytes_) {
        fail_range_plot_build("range_alloc_failed");
        return false;
    }

    ReportSpoolBuffer &out = *range_build_bytes_;
    out.set_max_size(AC_REPORT_RANGE_PLOT_MAX_BYTES);
    range_tmp_.set_max_size(768 * 1024);
    range_seen_events_.set_max_size(16 * 1024);

    range_build_ok_ =
        out.reserve_capacity(64 * 1024) &&
        range_seen_events_.reserve_capacity(2 * 1024) &&
        bin_put_u32(out, PLOT_BIN_MAGIC) &&
        bin_put_u16(out, PLOT_BIN_VERSION) &&
        bin_put_u16(out, 0) &&
        bin_put_i64(out, from_ms);
    if (!range_build_ok_) {
        fail_range_plot_build("range_alloc_failed");
        return false;
    }

    range_build_active_ = true;
    range_build_started_ms_ = millis();
    range_build_input_chunks_ = 0;
    range_build_input_bytes_ = 0;
    range_build_phase_ = ReportPlotBuildPhase::Events;
    return true;
}

bool ReportManager::process_range_event_chunk(
    const ReportResultChunk &chunk) {
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!read_range_chunk_payload(chunk, meta, payload)) {
        fail_range_plot_build("range_event_read_failed");
        return false;
    }
    const size_t wire = report_event_record_wire_size();
    const size_t count = wire ? payload.size() / wire : 0;
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        const int64_t duration_ms = event.duration_ms > 0
                                        ? static_cast<int64_t>(
                                              event.duration_ms)
                                        : 0;
        const int64_t event_end_ms = event.start_ms + duration_ms;
        const bool in_range = duration_ms > 0
                                  ? event.start_ms < range_build_to_ &&
                                        event_end_ms > range_build_from_
                                  : event.start_ms >= range_build_from_ &&
                                        event.start_ms < range_build_to_;
        if (!in_range) {
            continue;
        }
        bool in_session_range = false;
        for (size_t i = 0; i < range_range_count_; ++i) {
            if (duration_ms > 0) {
                if (event.start_ms < range_ranges_[i].end_ms &&
                    event_end_ms > range_ranges_[i].start_ms) {
                    in_session_range = true;
                    break;
                }
            } else if (event.start_ms >= range_ranges_[i].start_ms &&
                       event.start_ms < range_ranges_[i].end_ms) {
                in_session_range = true;
                break;
            }
        }
        if (!in_session_range) continue;
        if (report_event_seen(range_seen_events_, event)) continue;
        if (!remember_report_event(range_seen_events_, event)) {
            fail_range_plot_build("range_event_dedupe_failed");
            return false;
        }
        range_build_ok_ &=
            bin_put_i32(range_tmp_,
                        static_cast<int32_t>(event.start_ms -
                                             range_build_from_));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.duration_ms));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.code));
        range_build_ok_ &=
            bin_put_i32(range_tmp_, static_cast<int32_t>(event.flags));
        if (!range_build_ok_) {
            fail_range_plot_build("range_overflow");
            return false;
        }
        ++range_event_count_;
    }
    return true;
}

bool ReportManager::open_range_series(const ReportResultStream &stream) {
    const size_t name_len = stream.name ? strlen(stream.name) : 0;
    if (!range_build_bytes_ || name_len > UINT16_MAX) return false;
    range_tmp_.clear();
    range_series_points_ = 0;
    range_current_bucket_ = -1;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    range_last_range_index_ = -1;
    range_bucket_.clear();
    range_series_open_ = true;
    return range_build_ok_;
}

int ReportManager::range_plot_range_index(int64_t timestamp_ms) const {
    for (size_t i = 0; i < range_range_count_; ++i) {
        if (timestamp_ms >= range_ranges_[i].start_ms &&
            timestamp_ms < range_ranges_[i].end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ReportManager::result_chunk_matches_stream(
    const ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream &stream) const {
    if (!result_chunk_has_stream(chunk, stream_index)) return false;
    if (chunk.stream_mask != 0) {
        return chunk.kind == stream.kind;
    }
    return chunk.kind == stream.kind && chunk.signal == stream.signal &&
           chunk.name && stream.name && strcmp(chunk.name, stream.name) == 0;
}

bool ReportManager::collect_range_chunk(void *context,
                                        const ReportProviderChunk &info) {
    RangeChunkContext *ctx = static_cast<RangeChunkContext *>(context);
    if (!ctx || !ctx->manager || !info.name || !info.name[0]) return false;
    if (ctx->name && ctx->name[0] && strcmp(ctx->name, info.name) != 0) {
        return true;
    }
    return ctx->manager->add_range_provider_chunk(info, ctx->stream_index);
}

bool ReportManager::add_range_provider_chunk(
    const ReportProviderChunk &provider_chunk,
    size_t stream_index) {
    if (stream_index >= range_stream_count_ || stream_index > UINT8_MAX) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    uint32_t stream_bit = 0;
    if (!report_stream_bit(stream_index, stream_bit)) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    if (!range_chunks_) {
        fail_range_plot_build("range_chunks_missing");
        return false;
    }

    ReportResultStream &stream = range_streams_[stream_index];
    if (stream.kind != provider_chunk.kind ||
        stream.signal != provider_chunk.signal ||
        !stream.name ||
        strcmp(stream.name, provider_chunk.name) != 0) {
        fail_range_plot_build("range_stream_mismatch");
        return false;
    }
    auto account_stream = [&]() {
        stream.chunk_count++;
        stream.record_count += provider_chunk.record_count;
        stream.payload_bytes += provider_chunk.payload_len;
        stream.has_edf_segment =
            stream.has_edf_segment ||
            provider_chunk.ref.provider == ReportProviderId::Edf;
        stream.has_spool_segment =
            stream.has_spool_segment ||
            provider_chunk.ref.provider == ReportProviderId::Spool;
    };

    for (size_t i = 0; i < range_chunk_count_; ++i) {
        ReportResultChunk &existing = range_chunks_[i];
        const bool same_physical =
            result_chunk_same_physical_edf(existing, provider_chunk);
        const bool same_logical =
            existing.kind == provider_chunk.kind &&
            existing.source == provider_chunk.source &&
            existing.name && provider_chunk.name &&
            strcmp(existing.name, provider_chunk.name) == 0 &&
            existing.start_ms == provider_chunk.start_ms &&
            existing.end_ms == provider_chunk.end_ms &&
            report_provider_chunk_ref_equal(existing.provider_ref,
                                            provider_chunk.ref);
        if (!same_physical && !same_logical) continue;
        if ((existing.stream_mask & stream_bit) != 0) return true;
        existing.stream_mask |= stream_bit;
        account_stream();
        return true;
    }

    if (range_chunk_count_ >= AC_REPORT_RESULT_CHUNK_MAX) {
        fail_range_plot_build("range_chunks_full");
        return false;
    }

    ReportResultChunk &chunk = range_chunks_[range_chunk_count_++];
    chunk.provider_ref = provider_chunk.ref;
    chunk.kind = provider_chunk.kind;
    chunk.source = provider_chunk.source;
    chunk.signal = provider_chunk.signal;
    chunk.name = provider_chunk.name;
    chunk.stream_index = static_cast<uint8_t>(stream_index);
    chunk.stream_mask = stream_bit;
    chunk.start_ms = provider_chunk.start_ms;
    chunk.end_ms = provider_chunk.end_ms;
    chunk.payload_schema = provider_chunk.payload_schema;
    chunk.record_count = provider_chunk.record_count;
    chunk.payload_len = provider_chunk.payload_len;

    account_stream();
    return true;
}

bool ReportManager::materialize_range_plan(const ReportIndexedNight &night,
                                           const ReportResolvedPlan &plan) {
    range_night_start_ms_ = night.summary.start_ms;
    range_chunk_count_ = 0;
    for (size_t i = 0; i < AC_REPORT_RESULT_CHUNK_MAX; ++i) {
        range_chunks_[i] = ReportResultChunk{};
    }
    range_stream_count_ =
        std::min(plan.stream_count,
                 static_cast<size_t>(AC_REPORT_RESULT_STREAM_MAX));
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        range_streams_[i] = ReportResultStream{};
    }
    for (size_t i = 0; i < range_stream_count_; ++i) {
        const ReportResolvedStream &resolved = plan.streams[i];
        ReportResultStream &stream = range_streams_[i];
        stream.kind = resolved.kind;
        stream.source = resolved.selected_source;
        stream.signal = resolved.signal;
        stream.name = resolved.name;
        stream.required = resolved.required;
        stream.complete = resolved.complete;
        stream.has_edf_segment = resolved.has_edf_segment;
        stream.has_spool_segment = resolved.has_spool_segment;
    }
    range_range_count_ =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        if (i < range_range_count_) {
            range_ranges_[i].start_ms = plan.ranges[i].start_ms;
            range_ranges_[i].end_ms = plan.ranges[i].end_ms;
        } else {
            range_ranges_[i] = PlotRange{};
        }
    }

    EdfReportDataProvider edf_provider(range_edf_sessions_,
                                       range_edf_session_count_);
    for (size_t i = 0; i < plan.segment_count; ++i) {
        const ReportResolvedSegment &segment = plan.segments[i];
        if (segment.stream_index >= range_stream_count_) {
            fail_range_plot_build("range_bad_segment");
            return false;
        }
        if (!segment.complete ||
            segment.provider == ReportResolvedProvider::None) {
            continue;
        }
        const ReportSourceDef *source_def = report_source_def(segment.source);
        if (!source_def || !source_def->spool_type ||
            !source_def->spool_type[0]) {
            fail_range_plot_build("range_bad_source");
            return false;
        }
        const ReportDataProvider *provider = nullptr;
        if (segment.provider == ReportResolvedProvider::Edf) {
            provider = &edf_provider;
        } else if (segment.provider == ReportResolvedProvider::Spool) {
            provider = &spool_report_provider();
        }
        if (!provider) {
            fail_range_plot_build("range_bad_provider");
            return false;
        }
        RangeChunkContext context;
        context.manager = this;
        context.stream_index = segment.stream_index;
        context.name = segment.name;
        if (!provider->for_each_chunk(segment.kind,
                                      *source_def,
                                      segment.signal,
                                      segment.name,
                                      static_cast<int64_t>(
                                          night.summary.start_ms),
                                      segment.start_ms,
                                      segment.end_ms,
                                      collect_range_chunk,
                                      &context)) {
            fail_range_plot_build("range_chunk_list_failed");
            return false;
        }
    }
    return true;
}

bool ReportManager::process_range_series_sample_value(
    const ReportSeriesSample &sample,
    ReportSignalId signal,
    ReportSourceId source,
    uint32_t interval_ms,
    int32_t scale,
    bool &capped,
    bool &overflow) {
    if (sample.timestamp_ms < range_build_from_ ||
        sample.timestamp_ms >= range_build_to_) {
        return true;
    }
    const int sample_range_index = range_plot_range_index(sample.timestamp_ms);
    if (sample_range_index < 0) return true;
    if (range_have_last_sample_ &&
        (sample_range_index != range_last_range_index_ ||
         sample.timestamp_ms >
             range_last_sample_ms_ + plot_gap_threshold_ms(interval_ms))) {
        range_series_points_ += emit_plot_gap_to(range_tmp_,
                                                 range_bucket_,
                                                 range_build_from_,
                                                 range_build_ok_);
        range_current_bucket_ = -1;
        if (!range_build_ok_) {
            overflow = true;
            return false;
        }
        if (range_series_points_ >= AC_REPORT_RANGE_MAX_POINTS) {
            range_chunk_index_ = static_cast<uint32_t>(range_chunk_count_);
            capped = true;
            return false;
        }
    }
    const int64_t bucket_ms =
        plot_bucket_ms_for_signal(signal,
                                  source,
                                  range_bucket_ms_,
                                  interval_ms,
                                  true);
    int64_t sample_bucket =
        (sample.timestamp_ms - range_build_from_) / bucket_ms;
    if (sample_bucket < 0) sample_bucket = 0;
    if (range_current_bucket_ != sample_bucket) {
        range_series_points_ += flush_plot_bucket_to(range_tmp_,
                                                     range_bucket_,
                                                     range_build_from_,
                                                     range_build_ok_);
        if (!range_build_ok_) {
            overflow = true;
            return false;
        }
        range_current_bucket_ = sample_bucket;
        if (range_series_points_ >= AC_REPORT_RANGE_MAX_POINTS) {
            range_chunk_index_ = static_cast<uint32_t>(range_chunk_count_);
            capped = true;
            return false;
        }
    }
    int64_t value = static_cast<int64_t>(sample.value_milli) * scale;
    if (value > INT32_MAX) value = INT32_MAX;
    else if (value < INT32_MIN) value = INT32_MIN;
    const int32_t value_i32 = static_cast<int32_t>(value);
    if (!range_bucket_.have) {
        range_bucket_.have = true;
        range_bucket_.start_t = sample.timestamp_ms;
        range_bucket_.end_t = sample.timestamp_ms;
        range_bucket_.min_t = sample.timestamp_ms;
        range_bucket_.max_t = sample.timestamp_ms;
        range_bucket_.start_value = value_i32;
        range_bucket_.end_value = value_i32;
        range_bucket_.min_value = value_i32;
        range_bucket_.max_value = value_i32;
    } else {
        range_bucket_.end_t = sample.timestamp_ms;
        range_bucket_.end_value = value_i32;
        if (value_i32 < range_bucket_.min_value) {
            range_bucket_.min_value = value_i32;
            range_bucket_.min_t = sample.timestamp_ms;
        }
        if (value_i32 > range_bucket_.max_value) {
            range_bucket_.max_value = value_i32;
            range_bucket_.max_t = sample.timestamp_ms;
        }
    }
    range_have_last_sample_ = true;
    range_last_sample_ms_ = sample.timestamp_ms;
    range_last_range_index_ = sample_range_index;
    return true;
}

bool ReportManager::process_range_series_chunk(
    const ReportResultChunk &chunk) {
    return process_range_series_chunk(chunk, chunk.stream_index);
}

bool ReportManager::process_range_series_chunk(
    const ReportResultChunk &chunk,
    size_t stream_index) {
    if (stream_index >= range_stream_count_) {
        fail_range_plot_build("range_bad_stream");
        return false;
    }
    ReportProviderChunk provider_chunk;
    if (!provider_chunk_from_result_stream(chunk,
                                           stream_index,
                                           range_streams_,
                                           range_stream_count_,
                                           range_edf_sessions_,
                                           range_edf_session_count_,
                                           provider_chunk)) {
        fail_range_plot_build("range_chunk_map_failed");
        return false;
    }
    const int32_t scale =
        (provider_chunk.source == ReportSourceId::RespiratoryFlow6p25Hz ||
         provider_chunk.source == ReportSourceId::Leak0p5Hz)
            ? 60
            : 1;

    struct RangeSeriesContext {
        ReportManager *manager = nullptr;
        const ReportProviderSeriesReadStats *read_stats = nullptr;
        ReportSignalId signal = ReportSignalId::Flow;
        ReportSourceId source = ReportSourceId::Summary;
        uint32_t interval_ms = 0;
        int32_t scale = 1;
        bool capped = false;
        bool overflow = false;
    };
    RangeSeriesContext ctx;
    ctx.manager = this;
    ctx.signal = provider_chunk.signal;
    ctx.source = provider_chunk.source;
    ctx.scale = scale;
    ctx.interval_ms =
        infer_chunk_interval_ms(provider_chunk.record_count,
                                provider_chunk.start_ms,
                                provider_chunk.end_ms);
    ReportProviderSeriesReadStats read_stats;
    ctx.read_stats = &read_stats;
    const bool ok = for_each_range_series_sample(
        chunk,
        stream_index,
        read_stats,
        [](void *context, const ReportSeriesSample &sample) -> bool {
            RangeSeriesContext *ctx =
                static_cast<RangeSeriesContext *>(context);
            ReportManager *manager = ctx ? ctx->manager : nullptr;
            if (!manager) return false;
            const uint32_t interval_ms =
                (ctx->read_stats && ctx->read_stats->interval_ms)
                    ? ctx->read_stats->interval_ms
                    : ctx->interval_ms;
            return manager->process_range_series_sample_value(sample,
                                                              ctx->signal,
                                                              ctx->source,
                                                              interval_ms,
                                                              ctx->scale,
                                                              ctx->capped,
                                                              ctx->overflow);
        },
        &ctx);
    if (!ok && !ctx.capped) {
        fail_range_plot_build(ctx.overflow ? "range_overflow"
                                           : "range_series_decode_failed");
        return false;
    }
    (void)read_stats;
    return true;
}

bool ReportManager::finish_range_series() {
    if (!range_build_bytes_ || !range_series_open_) return false;
    range_series_points_ += flush_plot_bucket_to(range_tmp_,
                                                 range_bucket_,
                                                 range_build_from_,
                                                 range_build_ok_);
    if (range_series_points_ > 0) {
        const ReportResultStream &stream =
            range_streams_[range_stream_index_];
        if (!append_plot_series_compact(*range_build_bytes_,
                                        stream.name,
                                        range_tmp_,
                                        range_build_ok_)) {
            fail_range_plot_build("range_overflow");
            return false;
        }
    }
    range_tmp_.clear();
    range_series_open_ = false;
    range_series_points_ = 0;
    range_current_bucket_ = -1;
    range_have_last_sample_ = false;
    range_last_sample_ms_ = 0;
    if (!range_build_ok_) {
        fail_range_plot_build("range_overflow");
        return false;
    }
    return true;
}

void ReportManager::finish_range_plot_build() {
    if (!range_build_bytes_) {
        fail_range_plot_build("range_bad_state");
        return;
    }
    const PlotBlobScan scan = scan_plot_blob(*range_build_bytes_);
    if (!scan.valid) {
        fail_range_plot_build("range_invalid_blob");
        return;
    }

    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == range_build_index_ &&
            range_req_night_start_ms_ == range_night_start_ms_ &&
            range_req_from_ == range_build_from_ &&
            range_req_to_ == range_build_to_) {
            range_plot_bytes_ = range_build_bytes_;
            range_plot_index_ = range_build_index_;
            range_plot_night_start_ms_ = range_night_start_ms_;
            range_plot_from_ = range_build_from_;
            range_plot_to_ = range_build_to_;
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Range plot ready index=%lu points=%lu input_chunks=%lu "
              "input_bytes=%lu bytes=%lu elapsed_ms=%lu\n",
              static_cast<unsigned long>(range_build_index_),
              static_cast<unsigned long>(scan.points),
              static_cast<unsigned long>(range_build_input_chunks_),
              static_cast<unsigned long>(range_build_input_bytes_),
              static_cast<unsigned long>(range_build_bytes_->size()),
              static_cast<unsigned long>(
                  range_build_started_ms_
                      ? static_cast<uint32_t>(millis() -
                                              range_build_started_ms_)
                      : 0));
    reset_range_plot_build(false);
}

void ReportManager::fail_range_plot_build(const char *message) {
    Log::logf(CAT_REPORT,
              LOG_WARN,
              "Range plot failed index=%lu error=%s\n",
              static_cast<unsigned long>(range_build_index_),
              message ? message : "range_failed");
    if (result_slots_lock_) {
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == range_build_index_ &&
            range_req_night_start_ms_ == range_night_start_ms_ &&
            range_req_from_ == range_build_from_ &&
            range_req_to_ == range_build_to_) {
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
    reset_range_plot_build(false);
}

PlotBlobScan scan_plot_blob(const ReportSpoolBuffer &b) {
    PlotBlobScan out;
    const uint8_t *d = reinterpret_cast<const uint8_t *>(b.data());
    const size_t n = b.size();
    if (!d || n < 20) return out;
    if (read_u32_le(d) != PLOT_BIN_MAGIC ||
        read_u16_le(d + 4) != PLOT_BIN_VERSION) {
        return out;
    }
    const uint32_t ev = read_u32_le(d + 16);
    out.events = ev;
    size_t off = 20 + static_cast<size_t>(ev) * 16;
    if (off > n) return out;
    while (off < n) {
        if (off + 2 > n) return out;
        const uint16_t name_len = read_u16_le(d + off);
        off += 2;
        if (off + name_len + 4 > n) return out;
        off += name_len;
        const uint8_t mode = d[off++];
        off += 1;  // flags
        off += 2;  // reserved
        if (mode == PLOT_SERIES_MODE_COMPACT) {
            if (off + 16 > n) return out;
            off += 4;  // series_base_delta_ms
            const uint32_t time_unit_ms = read_u32_le(d + off);
            off += 4;
            const uint32_t value_scale_milli = read_u32_le(d + off);
            off += 4;
            const uint32_t pc = read_u32_le(d + off);
            off += 4;
            if (time_unit_ms == 0 || value_scale_milli == 0) return out;
            if (off + static_cast<size_t>(pc) * 4 > n) return out;
            off += static_cast<size_t>(pc) * 4;
            out.points += pc;
        } else if (mode == PLOT_SERIES_MODE_ENVELOPE_RUNS) {
            if (off + 16 > n) return out;
            off += 4;  // axis_base_delta_ms
            const uint32_t bucket_ms = read_u32_le(d + off);
            off += 4;
            const uint32_t value_scale_milli = read_u32_le(d + off);
            off += 4;
            const uint32_t run_count = read_u32_le(d + off);
            off += 4;
            if (bucket_ms == 0 || value_scale_milli == 0) return out;
            for (uint32_t i = 0; i < run_count; ++i) {
                if (off + 6 > n) return out;
                off += 4;  // start_bucket
                const uint16_t bucket_count = read_u16_le(d + off);
                off += 2;
                if (off + static_cast<size_t>(bucket_count) * 4 > n) {
                    return out;
                }
                off += static_cast<size_t>(bucket_count) * 4;
                out.points += static_cast<uint32_t>(bucket_count) * 2u;
            }
        } else {
            return out;
        }
    }
    out.valid = off == n;
    return out;
}

ReportManager::PlotRead ReportManager::read_plot_range(
    size_t therapy_index, const char *version,
    char *etag_out, size_t etag_out_size,
    int64_t from_ms, int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (etag_out && etag_out_size) etag_out[0] = '\0';
    if (to_ms <= from_ms) return PlotRead::NotFound;
    if (!ensure_result_slots()) return PlotRead::Unavailable;

    ScopedIndexedNight indexed_night("read_plot_range_index");
    if (!indexed_night) return PlotRead::Unavailable;
    size_t resolved_therapy_index = therapy_index;
    uint64_t version_night_start_ms = 0;
    const bool have_version_start =
        parse_report_night_start_from_etag(version, version_night_start_ms);
    const bool found_night = have_version_start
        ? indexed_night_by_start(version_night_start_ms,
                                 indexed_night.get(),
                                 &resolved_therapy_index)
        : indexed_night_by_therapy_index(therapy_index, indexed_night.get());
    if (!found_night) return PlotRead::NotFound;

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return PlotRead::Busy;
    format_night_etag_unlocked(indexed_night->summary,
                               indexed_night->source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return PlotRead::Stale;
    }
    if (indexed_night->edf_catalog_pending) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return PlotRead::Building;
    }

    const uint64_t night_start_ms = indexed_night->summary.start_ms;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    if (range_plot_bytes_ && range_plot_index_ == resolved_therapy_index &&
        range_plot_night_start_ms_ == night_start_ms &&
        range_plot_from_ == from_ms && range_plot_to_ == to_ms) {
        const PlotBlobScan scan = scan_plot_blob(*range_plot_bytes_);
        if (!scan.valid) {
            range_plot_bytes_.reset();
            range_plot_index_ = 0;
            range_plot_night_start_ms_ = 0;
            range_plot_from_ = 0;
            range_plot_to_ = 0;
            xSemaphoreGive(result_slots_lock_);
            return PlotRead::Error;
        }
        if (scan.events == 0 && scan.points == 0) {
            xSemaphoreGive(result_slots_lock_);
            return PlotRead::Empty;
        }
        out = range_plot_bytes_;
        xSemaphoreGive(result_slots_lock_);
        return PlotRead::Ready;
    }
    if (range_plot_bytes_) {
        range_plot_bytes_.reset();
        range_plot_index_ = 0;
        range_plot_night_start_ms_ = 0;
        range_plot_from_ = 0;
        range_plot_to_ = 0;
    }
    range_req_active_ = true;
    range_req_index_ = resolved_therapy_index;
    range_req_night_start_ms_ = night_start_ms;
    range_req_from_ = from_ms;
    range_req_to_ = to_ms;
    xSemaphoreGive(result_slots_lock_);
    return PlotRead::Building;
}

void ReportManager::service_range_plot(bool realtime_active) {
    if (!result_slots_lock_) return;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    const bool active = range_req_active_;
    const size_t index = range_req_index_;
    const uint64_t night_start_ms = range_req_night_start_ms_;
    const int64_t from_ms = range_req_from_;
    const int64_t to_ms = range_req_to_;
    xSemaphoreGive(result_slots_lock_);
    if (!active) return;
    if (realtime_active) return;
    if (summary_fetch_active_) return;
    if (plot_build_active_) {
        const uint32_t elapsed_ms =
            plot_build_started_ms_
                ? static_cast<uint32_t>(millis() - plot_build_started_ms_)
                : 0;
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Range plot preempted overview index=%lu "
                  "night=%llu elapsed_ms=%lu\n",
                  static_cast<unsigned long>(index),
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(elapsed_ms));
        reset_plot_build();
        release_result_edf_sessions();
    }
    // Yield an idle prefetch so the range builds now; a real foreground fetch is
    // not yielded, so wait for that.
    if (cache_fetch_active_) {
        prefetch_yield_to_foreground();
        if (cache_fetch_active_) return;
    }

    if (range_build_active_) {
        if (range_build_index_ == index &&
            range_night_start_ms_ == night_start_ms &&
            range_build_from_ == from_ms &&
            range_build_to_ == to_ms) {
            return;
        }
        reset_range_plot_build(false);
    }
    bool waiting_for_result = false;
    if (!start_range_plot_build(night_start_ms,
                                index,
                                from_ms,
                                to_ms,
                                waiting_for_result)) {
        if (waiting_for_result) return;
        xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
        if (range_req_active_ && range_req_index_ == index &&
            range_req_night_start_ms_ == night_start_ms &&
            range_req_from_ == from_ms && range_req_to_ == to_ms) {
            range_req_active_ = false;
        }
        xSemaphoreGive(result_slots_lock_);
    }
}

void ReportManager::poll_range_plot_build() {
    if (!range_build_active_) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_RANGE_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_RANGE_PLOT_POLL_BUDGET_MS;
    };

    while (range_build_active_ && !budget_spent()) {
        // Event chunks
        if (range_build_phase_ == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (range_chunk_index_ < range_chunk_count_) {
                const ReportResultChunk &chunk =
                    range_chunks_[range_chunk_index_++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (chunk.end_ms <= range_build_from_ ||
                    chunk.start_ms >= range_build_to_) {
                    continue;
                }

                if (!process_range_event_chunk(chunk)) return;

                reads++;
                range_build_input_chunks_++;
                range_build_input_bytes_ += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            if (!range_build_bytes_) {
                fail_range_plot_build("range_bad_state");
                return;
            }

            range_build_ok_ &=
                bin_put_u32(*range_build_bytes_, range_event_count_);

            if (range_tmp_.size()) {
                range_build_ok_ &=
                    range_build_bytes_->append(range_tmp_.data(),
                                               range_tmp_.size());
            }

            range_tmp_.clear();

            if (!range_build_ok_) {
                fail_range_plot_build("range_overflow");
                return;
            }

            range_build_phase_ = ReportPlotBuildPhase::Series;
            range_chunk_index_ = 0;
            range_stream_index_ = 0;

            continue;
        }

        // Series chunks
        if (range_build_phase_ == ReportPlotBuildPhase::Series) {
            if (range_stream_index_ >= range_stream_count_) {
                finish_range_plot_build();
                return;
            }

            const ReportResultStream &stream =
                range_streams_[range_stream_index_];

            if (stream.kind != ReportStoreChunkKind::Series ||
                !stream.name || !stream.name[0] ||
                stream.chunk_count == 0) {
                range_stream_index_++;
                range_chunk_index_ = 0;

                continue;
            }

            if (!range_series_open_ && !open_range_series(stream)) {
                fail_range_plot_build("range_series_open_failed");
                return;
            }

            bool processed = false;

            while (range_chunk_index_ < range_chunk_count_) {
                const ReportResultChunk &chunk =
                    range_chunks_[range_chunk_index_++];

                if (!result_chunk_matches_stream(chunk,
                                                 range_stream_index_,
                                                 stream)) {
                    continue;
                }

                if (chunk.end_ms <= range_build_from_ ||
                    chunk.start_ms >= range_build_to_) {
                    continue;
                }

                if (!process_range_series_chunk(chunk,
                                                range_stream_index_)) {
                    return;
                }

                reads++;
                range_build_input_chunks_++;
                range_build_input_bytes_ += chunk.payload_len;
                processed = true;

                break;
            }

            if (processed) continue;

            if (!finish_range_series()) return;

            range_stream_index_++;
            range_chunk_index_ = 0;

            continue;
        }

        fail_range_plot_build("range_bad_state");
        return;
    }
}

void ReportManager::poll_result_plot_build() {
    if (!plot_build_active_) return;

    size_t reads = 0;
    const uint32_t started_ms = millis();

    auto budget_spent = [&]() -> bool {
        if (reads == 0) return false;
        if (reads >= AC_REPORT_PLOT_POLL_CHUNK_CAP) return true;

        return static_cast<uint32_t>(millis() - started_ms) >=
               AC_REPORT_PLOT_POLL_BUDGET_MS;
    };

    while (plot_build_active_ && !budget_spent()) {
        // Event chunks
        if (plot_build_phase_ == ReportPlotBuildPhase::Events) {
            bool processed = false;

            while (plot_chunk_index_ < result_status_.chunk_count) {
                const ReportResultChunk &chunk =
                    result_chunks_[plot_chunk_index_++];

                if (chunk.kind != ReportStoreChunkKind::Events) continue;

                if (!process_plot_event_chunk(chunk)) return;

                processed = true;
                reads++;
                plot_build_input_chunks_++;
                plot_build_input_bytes_ += chunk.payload_len;

                break;
            }

            if (processed) continue;

            // Event phase footer
            const uint32_t event_count =
                static_cast<uint32_t>(plot_tmp_.size() / 16);

            plot_bin_ok_ &= bin_put_u32(plot_build_bin_, event_count);

            if (plot_tmp_.size()) {
                plot_bin_ok_ &=
                    plot_build_bin_.append(plot_tmp_.data(), plot_tmp_.size());
            }

            plot_tmp_.clear();
            plot_build_phase_ = ReportPlotBuildPhase::Series;
            plot_chunk_index_ = 0;
            memset(plot_chunk_done_, 0, sizeof(plot_chunk_done_));

            continue;
        }

        // Series chunks
        if (plot_build_phase_ == ReportPlotBuildPhase::Series) {
            bool processed = false;

            const size_t max_chunks = std::min(
                static_cast<size_t>(result_status_.chunk_count),
                static_cast<size_t>(AC_REPORT_RESULT_CHUNK_MAX));

            while (plot_chunk_index_ < max_chunks) {
                const size_t chunk_index = plot_chunk_index_++;
                if (plot_chunk_done_[chunk_index]) continue;

                const ReportResultChunk &chunk = result_chunks_[chunk_index];

                if (chunk.kind != ReportStoreChunkKind::Series) {
                    plot_chunk_done_[chunk_index] = true;
                    continue;
                }

                if (chunk.stream_index >= result_stream_count_ ||
                    chunk.stream_index >= AC_REPORT_RESULT_STREAM_MAX) {
                    plot_chunk_done_[chunk_index] = true;
                    continue;
                }

                if (chunk.provider_ref.provider == ReportProviderId::Edf) {
                    if (!process_plot_edf_series_batch(chunk_index,
                                                       processed)) {
                        return;
                    }

                    if (processed) {
                        reads++;
                        break;
                    }

                    continue;
                }

                if (!process_plot_series_chunk(chunk_index)) return;

                plot_chunk_done_[chunk_index] = true;
                plot_build_input_chunks_++;
                plot_build_input_bytes_ += chunk.payload_len;
                processed = true;
                reads++;

                break;
            }

            if (processed) continue;

            for (size_t i = 0; i < result_stream_count_ &&
                               i < AC_REPORT_RESULT_STREAM_MAX;
                 ++i) {
                if (!finish_plot_series(i)) return;
            }

            if (!plot_bin_ok_) {
                fail_result_prepare("plot_overflow");
                return;
            }

            if (plot_chunk_index_ >= max_chunks) {
                finish_result_plot_build();
                return;
            }

            continue;
        }

        fail_result_prepare("plot_bad_state");
        return;
    }
}

bool ReportManager::prepare_result_by_therapy_index(size_t therapy_index,
                                                    bool refresh_cache) {
    const ResultPrepareOutcome outcome =
        prepare_result_by_therapy_index_internal(therapy_index,
                                                 refresh_cache);
    return outcome == ResultPrepareOutcome::Prepared ||
           outcome == ResultPrepareOutcome::Deferred;
}

bool ReportManager::request_result_prepare_by_therapy_index(
    size_t therapy_index,
    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_index");
    if (!indexed_night ||
        !indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        return false;
    }

    if (refresh_cache) {
        invalidate_materialized(indexed_night->summary.start_ms, false);
    }
    const BuildQueueResult queued =
        enqueue_build(indexed_night->summary.start_ms,
                      therapy_index,
                      refresh_cache);
    return queued == BuildQueueResult::Queued ||
           queued == BuildQueueResult::AlreadyQueued;
}

bool ReportManager::request_result_prepare_by_start(uint64_t night_start_ms,
                                                    bool refresh_cache) {
    ScopedIndexedNight indexed_night("request_result_prepare_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !indexed_night_by_start(night_start_ms,
                                indexed_night.get(),
                                &therapy_index)) {
        return false;
    }

    if (refresh_cache) {
        invalidate_materialized(indexed_night->summary.start_ms, false);
    }
    const BuildQueueResult queued =
        enqueue_build(indexed_night->summary.start_ms,
                      therapy_index,
                      refresh_cache);
    return queued == BuildQueueResult::Queued ||
           queued == BuildQueueResult::AlreadyQueued;
}

bool ReportManager::defer_result_prepare_for_summary(size_t therapy_index,
                                                     bool refresh_cache) {
    if (!summary_fetch_active_) return false;
    pending_result_prepare_ = true;
    pending_result_refresh_cache_ = refresh_cache;
    pending_result_therapy_index_ = therapy_index;
    clear_result_prepare();
    result_status_.state = ReportResultState::Preparing;
    result_status_.therapy_index = therapy_index;
    result_status_.error = "summary_fetching";
    return true;
}

bool ReportManager::load_result_night(size_t therapy_index,
                                      ReportSummaryRecord &night) {
    ScopedIndexedNight indexed_night("load_result_night_index");
    if (indexed_night &&
        indexed_night_by_therapy_index(therapy_index, indexed_night.get())) {
        night = indexed_night->summary;
        return true;
    }
    clear_result_prepare();
    fail_result_prepare("night_not_found");
    return false;
}

bool ReportManager::publish_existing_result_if_current(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *current_etag,
    bool refresh_cache) {
    // Idempotent re-prepare: same indexed night and same Summary+EDF content
    // version already prepared with a plot -> keep it and republish. The ETag
    // check is required because another EDF session can be appended to the same
    // noon-noon night without changing the Summary start/end identity.
    if (!refresh_cache &&
        current_etag && current_etag[0] &&
        strcmp(result_etag_, current_etag) == 0 &&
        (result_status_.state == ReportResultState::Ready ||
         result_status_.state == ReportResultState::Partial) &&
        result_status_.therapy_index == therapy_index &&
        result_status_.chunk_count > 0 &&
        result_status_.night_start_ms == night.summary.start_ms &&
        result_plot_bin_.size() > 0) {
        return publish_result_to_slot();
    }
    return false;
}

void ReportManager::begin_result_prepare_for_night(
    size_t therapy_index,
    const ReportIndexedNight &night,
    const char *current_etag) {
    clear_result_prepare();
    result_status_.state = ReportResultState::Preparing;
    result_status_.therapy_index = therapy_index;

    result_indexed_night_ = night;
    result_night_ = night.summary;
    snprintf(result_etag_, sizeof(result_etag_), "%s",
             current_etag ? current_etag : "");
    set_result_ranges_from_indexed_night(night);
    result_status_.night_start_ms = night.summary.start_ms;
    result_status_.night_end_ms = night.summary.end_ms;
    result_status_.duration_min = night.summary.duration_min;
    if (night.summary.has_ahi) {
        result_status_.ahi = night.summary.ahi;
        result_status_.oa_index =
            night.summary.has_oa_index ? night.summary.oa_index : 0.0f;
        result_status_.ca_index =
            night.summary.has_ca_index ? night.summary.ca_index : 0.0f;
        result_status_.ua_index =
            night.summary.has_ua_index ? night.summary.ua_index : 0.0f;
        result_status_.hypopnea_index =
            night.summary.has_hypopnea_index
                ? night.summary.hypopnea_index
                : 0.0f;
        result_status_.arousal_index =
            night.summary.has_rera_index ? night.summary.rera_index : 0.0f;
        result_status_.event_metrics_valid = true;
    }
}

bool ReportManager::refresh_result_cache_if_needed(
    const ReportIndexedNight &night,
    size_t therapy_index,
    bool refresh_cache,
    bool &deferred) {
    deferred = false;
    if (!refresh_cache || result_status_.missing_required == 0) return true;

    const bool latest_tail_refresh = therapy_index == 0;
    prefetch_yield_to_foreground();
    if (!cache_fetch_active_) {
        if (!build_cache_plan(night, false, latest_tail_refresh)) {
            return false;
        }
        if (cache_source_count_ > 0) {
            if (!start_next_cache_source()) {
                return false;
            }
        } else {
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
        }
    }

    const bool cache_refresh_in_flight =
        cache_fetch_active_ &&
        cache_night_.start_ms == night.summary.start_ms;
    if (cache_refresh_in_flight) {
        pending_result_prepare_ = true;
        pending_result_refresh_cache_ = false;
        pending_result_therapy_index_ = therapy_index;
        result_status_.state = ReportResultState::Preparing;
        result_status_.error = "cache_fetching";
        deferred = true;
    }
    return true;
}

bool ReportManager::begin_materialization(const ReportIndexedNight &night,
                                          const ReportResolvedPlan &plan) {
    clear_result_ranges();
    result_range_count_ =
        std::min(plan.range_count,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    for (size_t i = 0; i < result_range_count_; ++i) {
        result_ranges_[i].start_ms = plan.ranges[i].start_ms;
        result_ranges_[i].end_ms = plan.ranges[i].end_ms;
    }

    uint32_t duration_min = 0;
    for (size_t i = 0; i < result_range_count_; ++i) {
        duration_min += report_ceil_duration_min(result_ranges_[i].start_ms,
                                                 result_ranges_[i].end_ms);
    }
    if (duration_min > 0) {
        result_status_.duration_min = duration_min;
    } else if (night.has_summary && night.summary.duration_min > 0) {
        result_status_.duration_min = night.summary.duration_min;
    }
    return true;
}

bool ReportManager::add_materialized_stream(
    const ReportResolvedStream &stream,
    size_t &result_stream_index) {
    if (!add_result_stream(stream.kind,
                           stream.selected_source,
                           stream.signal,
                           stream.name,
                           stream.required,
                           stream.complete,
                           result_stream_index)) {
        return false;
    }
    if (result_stream_index < result_stream_count_) {
        ReportResultStream &result_stream =
            result_streams_[result_stream_index];
        result_stream.has_edf_segment =
            result_stream.has_edf_segment || stream.has_edf_segment;
        result_stream.has_spool_segment =
            result_stream.has_spool_segment || stream.has_spool_segment;
    }
    return true;
}

bool ReportManager::add_materialized_segment(
    const ReportResolvedSegment &segment,
    size_t result_stream_index) {
    if (segment.provider == ReportResolvedProvider::None) return true;

    if (segment.provider == ReportResolvedProvider::Edf) {
        EdfReportDataProvider provider(result_edf_sessions_,
                                       result_edf_session_count_);
        return add_provider_chunks_to_result_stream(
            provider,
            segment.kind,
            segment.source,
            segment.signal,
            segment.name,
            static_cast<int64_t>(result_night_.start_ms),
            segment.start_ms,
            segment.end_ms,
            segment.required,
            segment.complete,
            result_stream_index);
    }

    return add_provider_chunks_to_result_stream(
        spool_report_provider(),
        segment.kind,
        segment.source,
        segment.signal,
        segment.name,
        static_cast<int64_t>(result_night_.start_ms),
        segment.start_ms,
        segment.end_ms,
        segment.required,
        segment.complete,
        result_stream_index);
}

void ReportManager::finish_materialization(const ReportResolvedPlan &plan) {
    result_status_.events_available = plan.events_available;
    result_status_.missing_required = result_status_.missing_streams;
}

bool ReportManager::resolve_and_materialize_result_for_night(
    const ReportIndexedNight &night,
    int64_t range_start_ms,
    int64_t range_end_ms,
    bool *edf_pending_out) {
    if (edf_pending_out) *edf_pending_out = false;
    bool edf_pending = false;
    bool have_edf =
        find_edf_sessions_for_night(night.summary,
                                    range_start_ms,
                                    range_end_ms,
                                    &edf_pending);
    if (edf_pending) {
        if (edf_pending_out) *edf_pending_out = true;
        result_status_.state = ReportResultState::Preparing;
        result_status_.error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result waiting for EDF catalog "
                  "night=%llu from=%lld to=%lld\n",
                  static_cast<unsigned long long>(night.summary.start_ms),
                  static_cast<long long>(range_start_ms),
                  static_cast<long long>(range_end_ms));
        return true;
    }
    if (!ensure_result_resolve_buffers()) return false;

    EdfReportDataProvider edf_provider(have_edf ? result_edf_sessions_
                                                : nullptr,
                                       have_edf ? result_edf_session_count_
                                                : 0);
    ReportSourceResolver resolver(edf_provider,
                                  spool_report_provider(),
                                  *result_resolve_scratch_);
    ReportResolvedPlan &plan = *result_resolved_plan_;
    if (!resolver.build_plan(night, range_start_ms, range_end_ms, plan)) {
        fail_result_prepare("source_resolve_failed");
        return false;
    }

    ReportMaterializer materializer;
    if (!materializer.materialize(night, plan, *this)) {
        fail_result_prepare("source_materialize_failed");
        return false;
    }
    return true;
}

bool ReportManager::activate_cache_plan_for_night(
    const ReportSummaryRecord &night) {
    cache_status_.source_count = static_cast<uint32_t>(cache_source_count_);
    if (cache_source_count_ > 0) {
        invalidate_materialized(night.start_ms, false);
    }
    discard_cache_coalesce_buffers();
    begin_cache_write_fetch();
    cache_fetch_active_ = true;
    cache_status_.active = true;
    return true;
}

bool ReportManager::count_result_events_from_chunks() {
    const size_t range_count =
        std::min(result_range_count_,
                 static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
    result_status_.oa_count = 0;
    result_status_.ca_count = 0;
    result_status_.ua_count = 0;
    result_status_.hypopnea_count = 0;
    result_status_.arousal_count = 0;
    ReportSpoolBuffer counted_events;
    counted_events.set_max_size(64 * 1024);
    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (chunk.kind != ReportStoreChunkKind::Events) continue;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
        if (!read_result_chunk_payload(chunk, meta, payload)) {
            fail_result_prepare("event_chunk_read_failed");
            return false;
        }
        const size_t count =
            payload.size() / report_event_record_wire_size();
        for (size_t index = 0; index < count; ++index) {
            ReportEventRecord event;
            if (!report_read_event_record(payload.data(),
                                          payload.size(),
                                          index,
                                          event)) {
                continue;
            }
            bool overlaps_result_range = false;
            const int64_t duration_ms = event.duration_ms > 0
                                            ? static_cast<int64_t>(
                                                  event.duration_ms)
                                            : 0;
            const int64_t event_end_ms = event.start_ms + duration_ms;
            for (size_t range_index = 0; range_index < range_count;
                 ++range_index) {
                const PlotRange &range = result_ranges_[range_index];
                if (duration_ms > 0) {
                    if (report_ranges_overlap(event.start_ms,
                                              event_end_ms,
                                              range.start_ms,
                                              range.end_ms)) {
                        overlaps_result_range = true;
                        break;
                    }
                } else if (event.start_ms >= range.start_ms &&
                           event.start_ms <= range.end_ms) {
                    overlaps_result_range = true;
                    break;
                }
            }
            if (!overlaps_result_range) {
                continue;
            }
            if (report_event_seen(counted_events, event)) continue;
            if (!remember_report_event(counted_events, event)) {
                fail_result_prepare("event_dedupe_failed");
                return false;
            }
            switch (event.code) {
                case report_event_code_value(ReportEventCode::Hypopnea):
                    result_status_.hypopnea_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::CentralApnea):
                    result_status_.ca_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::ObstructiveApnea):
                    result_status_.oa_count++;
                    break;
                case report_event_code_value(
                    ReportEventCode::UnclassifiedApnea):
                    result_status_.ua_count++;
                    break;
                case report_event_code_value(ReportEventCode::Arousal):
                    result_status_.arousal_count++;
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

void ReportManager::apply_result_event_indices_from_counts() {
    result_status_.event_metrics_valid = false;
    if (result_status_.duration_min <= 0) return;

    const float hours =
        static_cast<float>(result_status_.duration_min) / 60.0f;
    if (hours <= 0.0f) return;

    result_status_.oa_index =
        static_cast<float>(result_status_.oa_count) / hours;
    result_status_.ca_index =
        static_cast<float>(result_status_.ca_count) / hours;
    result_status_.ua_index =
        static_cast<float>(result_status_.ua_count) / hours;
    result_status_.hypopnea_index =
        static_cast<float>(result_status_.hypopnea_count) / hours;
    result_status_.arousal_index =
        static_cast<float>(result_status_.arousal_count) / hours;
    result_status_.ahi =
        result_status_.oa_index +
        result_status_.ca_index +
        result_status_.ua_index +
        result_status_.hypopnea_index;
    // Trust chunk-derived indices only when events are covered; otherwise the
    // counts are zero-by-absence, so omit the AHI.
    result_status_.event_metrics_valid = result_status_.events_available;
}

bool ReportManager::finalize_result_prepare(size_t therapy_index) {
    if (result_status_.state == ReportResultState::Error) return false;
    // Build a best-effort plot from whatever is cached: aged-out signals leave
    // missing_streams>0 and not-yet-swept sources leave missing_required>0, but
    // both are reported for the UI to mark - they do not block rendering. Only
    // a night with nothing cached at all (background hasn't reached it) has no
    // plot to show.
    if (result_status_.chunk_count == 0) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "not_cached";
        if (!publish_result_to_slot()) return false;
    } else {
        std::sort(result_chunks_,
                  result_chunks_ + result_status_.chunk_count,
                  [](const ReportResultChunk &a,
                     const ReportResultChunk &b) {
                      if (a.stream_index != b.stream_index) {
                          return a.stream_index < b.stream_index;
                      }
                      if (a.kind != b.kind) return a.kind < b.kind;
                      if (a.start_ms != b.start_ms) {
                          return a.start_ms < b.start_ms;
                      }
                      if (a.end_ms != b.end_ms) return a.end_ms < b.end_ms;
                      if (a.source != b.source) return a.source < b.source;
                      const int name_cmp = strcmp(a.name ? a.name : "",
                                                  b.name ? b.name : "");
                      if (name_cmp != 0) return name_cmp < 0;
                      return a.payload_len < b.payload_len;
                  });
        if (!count_result_events_from_chunks()) return false;
        apply_result_event_indices_from_counts();
        result_status_.state =
            settled_result_state(result_status_.missing_required);
        result_status_.error.clear();
        if (!publish_result_to_slot()) return false;
        if (!start_result_plot_build()) return false;
        if (plot_build_active_) {
            Log::logf(CAT_REPORT,
                      LOG_DEBUG,
                      "Result prepared index=%lu state=%s chunks=%lu "
                      "records=%lu bytes=%lu plot=building\n",
                      static_cast<unsigned long>(therapy_index),
                      result_state_name(),
                      static_cast<unsigned long>(result_status_.chunk_count),
                      static_cast<unsigned long>(result_status_.record_count),
                      static_cast<unsigned long>(result_status_.payload_bytes));
            return true;
        }
    }
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "Result prepared index=%lu state=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long>(therapy_index),
              result_state_name(),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_status_.record_count),
              static_cast<unsigned long>(result_status_.payload_bytes));
    return true;
}

ReportManager::ResultPrepareOutcome
ReportManager::prepare_result_by_therapy_index_internal(
    size_t therapy_index,
    bool refresh_cache) {
    if (defer_result_prepare_for_summary(therapy_index, refresh_cache)) {
        return ResultPrepareOutcome::Deferred;
    }
    if (!ensure_result_chunks()) return ResultPrepareOutcome::Failed;

    ReportSummaryRecord night;
    if (!load_result_night(therapy_index, night)) {
        return ResultPrepareOutcome::Failed;
    }
    return prepare_result_by_night_start_internal(night.start_ms,
                                                 therapy_index,
                                                 refresh_cache);
}

ReportManager::ResultPrepareOutcome
ReportManager::prepare_result_by_night_start_internal(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh_cache) {
    Log::logf(CAT_REPORT,
              LOG_DEBUG,
              "Result prepare start night=%llu index=%lu refresh=%u\n",
              static_cast<unsigned long long>(night_start_ms),
              static_cast<unsigned long>(therapy_index),
              refresh_cache ? 1u : 0u);
    if (!ensure_result_chunks()) {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "Result prepare failed before night load reason=chunk_alloc "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Failed;
    }

    if (!prepare_indexed_night_) {
        prepare_indexed_night_ = static_cast<ReportIndexedNight *>(
            Memory::calloc_large(1, sizeof(ReportIndexedNight), false));
        if (!prepare_indexed_night_) {
            log_report_alloc_failed("prepare_indexed_night",
                                    sizeof(ReportIndexedNight));
            return ResultPrepareOutcome::Retry;
        }
    }
    size_t current_therapy_index = therapy_index;
    memset(prepare_indexed_night_, 0, sizeof(*prepare_indexed_night_));
    if (!indexed_night_by_start(night_start_ms,
                                *prepare_indexed_night_,
                                &current_therapy_index)) {
        clear_result_prepare();
        fail_result_prepare("night_not_found");
        return ResultPrepareOutcome::Failed;
    }
    const ReportIndexedNight &indexed_night = *prepare_indexed_night_;
    therapy_index = current_therapy_index;
    const ReportSummaryRecord &night = indexed_night.summary;
    if (indexed_night.edf_catalog_pending) {
        result_status_.state = ReportResultState::Preparing;
        result_status_.therapy_index = therapy_index;
        result_status_.night_start_ms = night.start_ms;
        result_status_.night_end_ms = night.end_ms;
        result_status_.duration_min = night.duration_min;
        result_status_.error = "edf_catalog_refreshing";
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "Result prepare deferred for EDF catalog "
                  "night=%llu index=%lu\n",
                  static_cast<unsigned long long>(night_start_ms),
                  static_cast<unsigned long>(therapy_index));
        return ResultPrepareOutcome::Deferred;
    }

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        result_status_.error = "summary_lock_busy";
        return ResultPrepareOutcome::Retry;
    }
    format_night_etag_unlocked(night,
                               indexed_night.source_signature,
                               current_etag,
                               sizeof(current_etag));
    give_summary_lock();
    if (!current_etag[0]) {
        result_status_.error = "empty_etag";
        return ResultPrepareOutcome::Retry;
    }

    if (publish_existing_result_if_current(therapy_index,
                                           indexed_night,
                                           current_etag,
                                           refresh_cache)) {
        return ResultPrepareOutcome::Prepared;
    }

    begin_result_prepare_for_night(therapy_index, indexed_night, current_etag);
    result_skip_plot_cache_ = refresh_cache;

    // Use the session data span, not night.end_ms (a 24h day bucket far past the
    // therapy data) coverage is only written/checked over the session span,
    // so the result chunk range and its coverage check must match.
    ReportSessionRange night_range;
    if (!result_data_span(night_range.start_ms, night_range.end_ms)) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "no_sessions";
        return publish_result_to_slot() ? ResultPrepareOutcome::Prepared
                                        : ResultPrepareOutcome::Retry;
    }

    bool deferred = false;
    if (!resolve_and_materialize_result_for_night(indexed_night,
                                                  night_range.start_ms,
                                                  night_range.end_ms,
                                                  &deferred)) {
        release_result_edf_sessions();
        return result_status_.state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) return ResultPrepareOutcome::Deferred;
    const bool uses_edf = result_uses_edf_provider();
    if (!uses_edf) {
        release_result_edf_sessions();
    }

    deferred = false;
    if (!refresh_result_cache_if_needed(indexed_night,
                                        therapy_index,
                                        refresh_cache,
                                        deferred)) {
        release_result_edf_sessions();
        return result_status_.state == ReportResultState::Error
                   ? ResultPrepareOutcome::Failed
                   : ResultPrepareOutcome::Retry;
    }
    if (deferred) {
        release_result_edf_sessions();
        return ResultPrepareOutcome::Deferred;
    }

    const bool ok = finalize_result_prepare(therapy_index);
    if (!plot_build_active_) {
        release_result_edf_sessions();
    }
    if (ok) return ResultPrepareOutcome::Prepared;
    return result_status_.state == ReportResultState::Error
               ? ResultPrepareOutcome::Failed
               : ResultPrepareOutcome::Retry;
}

bool ReportManager::cache_source_supported(ReportSourceId source) const {
    switch (source) {
        case ReportSourceId::RespiratoryEvents:
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            return true;
        default:
            return false;
    }
}

bool ReportManager::build_cache_plan(const ReportIndexedNight &indexed_night,
                                     bool force,
                                     bool latest_tail_refresh) {
    const ReportSummaryRecord &night = indexed_night.summary;
    cache_night_ = night;
    cache_source_count_ = 0;
    cache_source_index_ = 0;
    cache_status_ = {};
    cache_status_.night_start_ms = night.start_ms;
    cache_status_.night_end_ms = night.end_ms;
    cache_status_.active = true;

    auto add_plan_source = [&](ReportSourceId source,
                               int64_t from_ms) -> bool {
        if (!cache_source_supported(source)) return true;
        for (size_t i = 0; i < cache_source_count_; ++i) {
            ReportCacheSourcePlan &existing = cache_plan_[i];
            if (existing.source != source) continue;
            if (from_ms < existing.from_ms) existing.from_ms = from_ms;
            return true;
        }
        if (cache_source_count_ >= AC_REPORT_CACHE_SOURCE_MAX) {
            fail_cache_fetch("cache_plan_full");
            return false;
        }
        ReportCacheSourcePlan &plan = cache_plan_[cache_source_count_++];
        plan.source = source;
        plan.from_ms = from_ms;
        return true;
    };

    int64_t latest_tail_start_ms = static_cast<int64_t>(night.start_ms);
    int64_t latest_tail_end_ms = static_cast<int64_t>(night.end_ms);
    if (latest_tail_refresh) {
        bool found_latest_range = false;
        auto maybe_use_latest_range = [&](const ReportSessionRange &range) {
            if (range.end_ms <= range.start_ms) return;
            if (!found_latest_range ||
                range.start_ms > latest_tail_start_ms) {
                latest_tail_start_ms = range.start_ms;
                latest_tail_end_ms = range.end_ms;
                found_latest_range = true;
            }
        };
        const size_t edf_count =
            std::min(indexed_night.data_range_count,
                     static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
        if (indexed_night.has_edf && edf_count > 0) {
            for (size_t i = 0; i < edf_count; ++i) {
                maybe_use_latest_range(indexed_night.data_ranges[i]);
            }
            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                const ReportSessionRange &display = indexed_night.ranges[i];
                bool overlaps_edf = false;
                for (size_t k = 0; k < edf_count; ++k) {
                    if (ranges_overlap(display.start_ms,
                                       display.end_ms,
                                       indexed_night.data_ranges[k].start_ms,
                                       indexed_night.data_ranges[k].end_ms)) {
                        overlaps_edf = true;
                        break;
                    }
                }
                if (!overlaps_edf) maybe_use_latest_range(display);
            }
        } else {
            const size_t display_count =
                std::min(indexed_night.range_count,
                         static_cast<size_t>(AC_REPORT_SUMMARY_SESSION_MAX));
            for (size_t i = 0; i < display_count; ++i) {
                maybe_use_latest_range(indexed_night.ranges[i]);
            }
        }
    }

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);

    if (force) {
        for (size_t i = 0; i < source_count; ++i) {
            const ReportSourceDef &source = sources[i];
            if (source.id == ReportSourceId::Summary) continue;
            if (!add_plan_source(source.id,
                                 static_cast<int64_t>(night.start_ms))) {
                return false;
            }
        }
    } else {
        int64_t span_start_ms = 0;
        int64_t span_end_ms = 0;
        if (latest_tail_refresh) {
            span_start_ms = latest_tail_start_ms;
            span_end_ms = latest_tail_end_ms;
        } else if (!indexed_night_data_span(indexed_night,
                                            span_start_ms,
                                            span_end_ms)) {
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
            return true;
        }

        ScopedReportResolveContext resolve("cache_plan_resolver");
        if (!resolve) {
            fail_cache_fetch("cache_plan_alloc_failed");
            return false;
        }

        bool edf_pending = false;
        size_t session_count = 0;
        const bool have_edf =
            collect_edf_sessions_for_night(night,
                                           span_start_ms,
                                           span_end_ms,
                                           resolve.sessions(),
                                           AC_REPORT_EDF_SESSION_MAX,
                                           session_count,
                                           &edf_pending);
        if (edf_pending) {
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error.clear();
            return true;
        }

        EdfReportDataProvider edf_provider(have_edf ? resolve.sessions()
                                                    : nullptr,
                                           have_edf ? session_count : 0);
        ReportSourceResolver resolver(edf_provider,
                                      spool_report_provider(),
                                      resolve.scratch());
        ReportResolvedPlan &resolved = resolve.plan();
        if (!resolver.build_plan(indexed_night,
                                 span_start_ms,
                                 span_end_ms,
                                 resolved)) {
            fail_cache_fetch("cache_source_resolve_failed");
            return false;
        }

        for (size_t i = 0; i < resolved.segment_count; ++i) {
            const ReportResolvedSegment &segment = resolved.segments[i];
            if (segment.provider != ReportResolvedProvider::Spool ||
                segment.complete ||
                !segment.required ||
                segment.end_ms <= segment.start_ms) {
                continue;
            }
            const ReportSourceDef *source = report_source_def(segment.source);
            if (!source || !cache_source_supported(source->id)) continue;
            int64_t from_ms = segment.start_ms;
            if (latest_tail_refresh) {
                int64_t cached_end_ms = 0;
                if (source_latest_cached_end_for_night(*source,
                                                       night,
                                                       cached_end_ms) &&
                    cached_end_ms > latest_tail_start_ms) {
                    from_ms =
                        cached_end_ms - AC_REPORT_LATEST_TAIL_OVERLAP_MS;
                    if (from_ms < latest_tail_start_ms) {
                        from_ms = latest_tail_start_ms;
                    }
                    if (from_ms > segment.start_ms) {
                        from_ms = segment.start_ms;
                    }
                }
            }
            if (!add_plan_source(source->id, from_ms)) return false;
        }
    }

    if (cache_source_count_ == 0) {
        cache_status_.active = false;
        cache_status_.revision++;
        cache_status_.error.clear();
        return true;
    }
    return activate_cache_plan_for_night(night);
}

bool ReportManager::start_next_cache_source() {
    if (!cache_fetch_active_) return false;
    if (cache_source_index_ >= cache_source_count_) {
        finish_cache_fetch();
        return true;
    }

    const ReportCacheSourcePlan &plan = cache_plan_[cache_source_index_];
    const ReportSourceId source = plan.source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    std::string from_dt;
    if (!format_utc_ms_iso(static_cast<uint64_t>(plan.from_ms), from_dt)) {
        fail_cache_fetch("bad_cache_from_time");
        return false;
    }

    SpoolClientRequest request;
    request.spool_type = def->spool_type;
    request.from_dt = from_dt;
    request.max_size = AC_REPORT_CACHE_SPOOL_ROUND_BYTES;
    request.fragment_max = AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES;
    request.max_notifications = AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL;
    request.max_rounds = 128;
    request.pace_on_backpressure = true;
    request.stream_rounds = true;
    if (!spool_.begin(request)) {
        fail_cache_fetch("cache_spool_start_failed");
        return false;
    }

    if (!reset_cache_source_coverage_marks()) {
        fail_cache_fetch("coverage_extent_alloc_failed");
        return false;
    }
    cache_status_.active_source = source;
    cache_status_.source_index = static_cast<uint32_t>(cache_source_index_);
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_DEBUG,
              "Cache source queued source=%s from=%s night=%llu\n",
              def->spool_type,
              from_dt.c_str(),
              static_cast<unsigned long long>(cache_night_.start_ms));
    return true;
}

bool ReportManager::store_cache_round(ReportSpoolResult &result) {
    const ReportSourceId source = cache_plan_[cache_source_index_].source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    ChunkWriteContext context;
    context.manager = this;
    context.source = source;
    char error[64] = {};
    context.error = error;
    context.error_len = sizeof(error);
    bool parsed = false;
    switch (source) {
        case ReportSourceId::UsageEvents:
        case ReportSourceId::RespiratoryEvents:
            parsed = report_parse_event_spool(result,
                                              source,
                                              write_parsed_chunk,
                                              &context,
                                              error,
                                              sizeof(error));
            break;
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            parsed = report_parse_series_spool(result,
                                               source,
                                               write_parsed_chunk,
                                               &context,
                                               error,
                                               sizeof(error));
            break;
        default:
            snprintf(error, sizeof(error), "%s", "unsupported_cache_source");
            parsed = false;
            break;
    }
    if (!parsed) {
        // An empty source spool is not a fetch failure: a session can hold zero
        // events, or a sampled source can have aged out
        if (strcmp(error, "spool_empty") == 0) return true;
        fail_cache_fetch(error[0] ? error : "cache_parse_failed");
        return false;
    }

    return true;
}

bool ReportManager::reset_cache_source_coverage_marks() {
    if (!ensure_cache_source_night_extents()) return false;
    memset(cache_source_night_extent_ms_,
           0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(int64_t));
    return true;
}

void ReportManager::note_cache_chunk_coverage(const ReportParsedChunk &chunk) {
    // Track every night the chunk touches, series AND events, recording how far
    // (max end_ms) real data reached so write_cache_source_coverage can bound
    // its coverage claims to the actually-delivered extent.
    if (chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        return;
    }

    if (!cache_source_night_extent_ms_) return;
    if (!take_summary_lock(portMAX_DELAY)) return;
    for (size_t record_index = 0;
         records_ &&
         record_index < record_count_ &&
         record_index < AC_REPORT_SUMMARY_RECORD_MAX;
         ++record_index) {
        const ReportSummaryRecord &record = records_[record_index];
        if (!record.valid || !record.duration_min) continue;

        if (report_ranges_overlap(chunk.start_ms,
                                  chunk.end_ms,
                                  static_cast<int64_t>(record.start_ms),
                                  static_cast<int64_t>(record.end_ms))) {
            if (chunk.end_ms > cache_source_night_extent_ms_[record_index]) {
                cache_source_night_extent_ms_[record_index] = chunk.end_ms;
            }
        }
    }
    give_summary_lock();
}

bool ReportManager::write_cache_source_coverage(ReportSourceId source,
                                                int64_t from_ms) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    // Build coverage for every night this sweep delivered, then persist them all
    // in ONE load+coalesce+rewrite (write_coverage_batch). Writing per night
    // re-read+rewrote the whole coverage file O(nights) times on the spool path,
    // starving the CAN RX (dropped frames -> framing CRC). This runs only after
    // a source's spool completed the [from_ms, now] sweep. Per night:
    // - start: a tail refresh (from_ms past the session start) claims only from
    //   where it fetched, never back-claiming the earlier span; a full sweep
    //   (from_ms <= span_start) claims from the span start.
    // - end: the full session span. A sampled source that delivered nothing is
    //   skipped on a partial sweep but settled covered on a full sweep (the
    //   device no longer retains it -- aged out -- so it stops re-fetching).
    //   Events are sparse, so a covered span can legitimately hold zero events.
    const bool sampled = source_is_sampled(*def);
    static ReportStoreCoverageRecord *cov_batch = nullptr;
    if (!cov_batch) {
        cov_batch = static_cast<ReportStoreCoverageRecord *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX,
            sizeof(ReportStoreCoverageRecord),
            false));
    }
    if (!cov_batch) {
        log_report_alloc_failed(
            "coverage_batch",
            AC_REPORT_SUMMARY_RECORD_MAX *
                sizeof(ReportStoreCoverageRecord));
        fail_cache_fetch("coverage_alloc_failed");
        return false;
    }

    ReportSummaryRecord *summary_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, summary_batch)) {
        fail_cache_fetch("coverage_summary_alloc_failed");
        return false;
    }

    size_t summary_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        fail_cache_fetch("coverage_summary_busy");
        return false;
    }
    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        summary_batch[summary_count++] = records_[i];
    }
    give_summary_lock();

    size_t batch_count = 0;
    for (size_t i = 0; i < summary_count; ++i) {
        const ReportSummaryRecord &record = summary_batch[i];
        if (!record.valid || !record.duration_min) continue;
        int64_t span_start = 0;
        int64_t span_end = 0;
        if (!night_data_span(record, span_start, span_end)) continue;
        if (span_end <= from_ms) continue;
        const int64_t extent = cache_source_night_extent_ms_[i];
        if (sampled && extent <= span_start && from_ms > span_start) continue;
        if (batch_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;

        ReportStoreCoverageRecord &coverage = cov_batch[batch_count];
        coverage = {};
        coverage.start_ms = from_ms > span_start ? from_ms : span_start;
        coverage.end_ms = span_end;
        coverage.parser_schema = def->parser_schema;
        coverage.state = ReportStoreCoverageState::Complete;
        coverage.origin = ReportStoreChunkOrigin::Spool;
        ++batch_count;
    }
    if (batch_count == 0) {
        give_summary_scratch();
        fail_cache_fetch("coverage_empty");
        return false;
    }
    give_summary_scratch();
    if (!ReportStore::write_coverage_batch(def->spool_type, cov_batch,
                                           batch_count)) {
        fail_cache_fetch("coverage_write_failed");
        return false;
    }
    if (!source_complete_for_range(cache_night_, *def, from_ms)) {
        fail_cache_fetch("coverage_incomplete");
        return false;
    }
    int64_t cached_end_ms = 0;
    if (source_is_sparse_event(*def) &&
        !source_latest_cached_end_for_night(*def, cache_night_, cached_end_ms)) {
        note_sparse_event_confirmed_empty(cache_night_, *def);
    }
    return true;
}

bool ReportManager::finalize_cache_source_if_ready() {
    if (fail_cache_fetch_if_write_failed()) return false;
    const CacheFlushResult flush = flush_all_cache_coalesce_buffers();
    if (flush == CacheFlushResult::Blocked) return true;
    if (flush == CacheFlushResult::Failed) {
        fail_cache_fetch("cache_flush_failed");
        return false;
    }
    if (cache_writes_pending_for_active_fetch()) return true;

    if (!write_cache_source_coverage(cache_finalizing_plan_.source,
                                     cache_finalizing_plan_.from_ms)) {
        return false;
    }
    cache_source_finalizing_ = false;
    cache_finalizing_plan_ = {};
    cache_source_index_++;
    return start_next_cache_source();
}

bool ReportManager::fail_cache_fetch_if_write_failed() {
    std::string write_error;
    if (cache_write_failed_for_active_fetch(write_error)) {
        fail_cache_fetch(write_error.empty() ? "cache_write_failed"
                                             : write_error.c_str());
        return true;
    }
    return false;
}

bool ReportManager::drain_cache_spool_rounds() {
    ReportSpoolResult round;
    while (spool_.take_completed_round(round)) {
        if (!store_cache_round(round)) return false;
        round.clear();
        cache_status_.spool = spool_.status();
        if (cache_write_backpressure_active()) return false;
    }
    return true;
}

void ReportManager::finish_cache_spool_if_terminal() {
    if (spool_.complete()) {
        ReportSpoolResult final_result;
        spool_.move_result_to(final_result);
        if (final_result.truncated) {
            fail_cache_fetch("source_truncated");
            return;
        }
        // Flush before claiming coverage, so coverage never marks unpersisted data.
        cache_finalizing_plan_ = cache_plan_[cache_source_index_];
        cache_source_finalizing_ = true;
        (void)finalize_cache_source_if_ready();
    } else if (spool_.failed()) {
        fail_cache_fetch(spool_.status().error.c_str());
    }
}

void ReportManager::poll_cache_fetch(RpcArbiter &arbiter) {
    if (!cache_fetch_active_) return;

    if (cache_source_finalizing_) {
        (void)finalize_cache_source_if_ready();
        return;
    }
    if (fail_cache_fetch_if_write_failed()) return;

    spool_.poll(arbiter);
    log_spool_can_pressure(arbiter);
    cache_status_.spool = spool_.status();
    if (cache_write_backpressure_active()) return;

    if (!drain_cache_spool_rounds()) return;
    finish_cache_spool_if_terminal();
}

void ReportManager::finish_cache_fetch() {
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error.clear();
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_INFO,
              "Cache fetch complete night=%llu chunks=%lu\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              static_cast<unsigned long>(cache_status_.chunks_written));
    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index, false);
    }
}

void ReportManager::fail_cache_fetch(const char *message) {
    discard_cache_coalesce_buffers();
    abort_cache_write_fetch();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = message ? message : "cache_fetch_failed";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_REPORT, LOG_WARN, "Cache fetch failed: %s\n",
              cache_status_.error.c_str());
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(cache_status_.error.c_str());
    }
}

}  // namespace aircannect
