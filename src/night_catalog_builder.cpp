#include "night_catalog_builder.h"

#include <algorithm>
#include <cmath>
#include <string.h>

#include "checked_size.h"
#include "incremental_sort.h"
#include "large_object.h"
#include "large_scratch_array.h"
#include "report_records.h"
#include "report_fallback_artifact.h"
#include "report_fallback_payload_layout.h"

namespace aircannect {
namespace {

constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);
constexpr int64_t SUMMARY_SESSION_EDGE_TOLERANCE_MS = 2LL * 60000LL;

enum class SessionOrigin : uint8_t {
    Edf,
    Fallback,
    Summary,
};

struct BuildNight {
    SleepDayId sleep_day;
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    ReportDailyMetrics str_metrics;
    ReportDailyMetrics summary_metrics;
    uint64_t summary_identity = 0;
    int32_t timezone_offset_minutes = 0;
    size_t owner = 0;
    bool boundary_set = false;
    bool has_edf = false;
    bool active_capture = false;
    bool has_str = false;
    bool has_summary = false;
    bool has_fallback = false;
    bool local_history = false;
    bool fallback_joins_summary = false;
    bool summary_metrics_valid = false;
    bool timezone_offset_valid = false;
    bool summary_expired = false;
};

struct BuildSession {
    size_t owner = 0;
    SessionOrigin origin = SessionOrigin::Edf;
    NightCatalogTimeRange range;
};

struct BuildFile {
    size_t owner = 0;
    NightCatalogSourceFileInput source;
    NightCatalogTimeRange session_range;
    bool has_session = false;
};

struct BuildFallback {
    size_t owner = 0;
    NightCatalogFallbackInput source;
    int32_t time_adjust_ms = 0;
};

bool add_count(size_t &total, size_t amount) {
    return CheckedSize::add_to(total, amount);
}

bool valid_boundary(int64_t start_ms, int64_t end_ms) {
    return start_ms > 0 && end_ms > start_ms;
}

bool same_range(const NightCatalogTimeRange &lhs,
                const NightCatalogTimeRange &rhs) {
    return lhs.start_ms == rhs.start_ms && lhs.end_ms == rhs.end_ms;
}

bool subtract_time(int64_t lhs, int64_t rhs, int64_t &difference) {
    if ((rhs > 0 && lhs < INT64_MIN + rhs) ||
        (rhs < 0 && lhs > INT64_MAX + rhs)) {
        return false;
    }

    difference = lhs - rhs;
    return true;
}

bool adjust_time(int64_t value, int64_t adjustment, int64_t &adjusted) {
    if ((adjustment > 0 && value > INT64_MAX - adjustment) ||
        (adjustment < 0 && value < INT64_MIN - adjustment)) {
        return false;
    }

    adjusted = value + adjustment;
    return true;
}

bool adjust_range(const NightCatalogTimeRange &range,
                  int64_t adjustment,
                  NightCatalogTimeRange &adjusted) {
    return range.valid() &&
           adjust_time(range.start_ms, adjustment, adjusted.start_ms) &&
           adjust_time(range.end_ms, adjustment, adjusted.end_ms) &&
           adjusted.valid();
}

bool valid_timezone_offset(bool valid, int32_t minutes) {
    return !valid || (minutes >= -24 * 60 && minutes <= 24 * 60);
}

uint32_t metric_bit(NightCatalogMetric metric) {
    return 1u << static_cast<uint8_t>(metric);
}

void set_metric(NightCatalogMetrics &out,
                NightCatalogMetric metric,
                float value,
                NightCatalogMetricSource source,
                bool fill_only,
                bool &used) {
    const uint32_t bit = metric_bit(metric);
    if (fill_only && (out.valid_mask & bit) != 0) return;

    switch (metric) {
        case NightCatalogMetric::Ahi: out.ahi = value; break;
        case NightCatalogMetric::ObstructiveApneaIndex:
            out.obstructive_apnea_index = value;
            break;
        case NightCatalogMetric::CentralApneaIndex:
            out.central_apnea_index = value;
            break;
        case NightCatalogMetric::UnknownApneaIndex:
            out.unknown_apnea_index = value;
            break;
        case NightCatalogMetric::HypopneaIndex:
            out.hypopnea_index = value;
            break;
        case NightCatalogMetric::ArousalIndex:
            out.arousal_index = value;
            break;
        case NightCatalogMetric::MaskPressure50:
            out.mask_pressure_50_cm_h2o = value;
            break;
        case NightCatalogMetric::Leak50:
            out.leak_50_l_min = value;
            break;
        case NightCatalogMetric::MaskPressure95:
            out.mask_pressure_95_cm_h2o = value;
            break;
        case NightCatalogMetric::Leak95:
            out.leak_95_l_min = value;
            break;
        case NightCatalogMetric::MinuteVentilation50:
            out.minute_ventilation_50_l_min = value;
            break;
        case NightCatalogMetric::MinuteVentilation95:
            out.minute_ventilation_95_l_min = value;
            break;
        case NightCatalogMetric::RespiratoryRate50:
            out.respiratory_rate_50_bpm = value;
            break;
        case NightCatalogMetric::RespiratoryRate95:
            out.respiratory_rate_95_bpm = value;
            break;
        case NightCatalogMetric::TidalVolume50:
            out.tidal_volume_50_l = value;
            break;
        case NightCatalogMetric::TidalVolume95:
            out.tidal_volume_95_l = value;
            break;
        case NightCatalogMetric::Spo2Median:
            out.spo2_median_percent = value;
            break;
        case NightCatalogMetric::DurationMinutes:
        case NightCatalogMetric::Spo2ThresholdMinutes:
        case NightCatalogMetric::CsrMinutes:
        case NightCatalogMetric::Count:
            return;
    }

    out.valid_mask |= bit;
    if (source == NightCatalogMetricSource::Str) {
        out.str_mask |= bit;
        out.summary_mask &= ~bit;
    } else if (source == NightCatalogMetricSource::Summary) {
        out.summary_mask |= bit;
        out.str_mask &= ~bit;
    }
    used = true;
}

void set_uint_metric(NightCatalogMetrics &out,
                     NightCatalogMetric metric,
                     uint32_t value,
                     NightCatalogMetricSource source,
                     bool fill_only,
                     bool &used) {
    const uint32_t bit = metric_bit(metric);
    if (fill_only && (out.valid_mask & bit) != 0) return;

    switch (metric) {
        case NightCatalogMetric::DurationMinutes:
            out.duration_min = value;
            break;
        case NightCatalogMetric::Spo2ThresholdMinutes:
            out.spo2_threshold_minutes = value;
            break;
        case NightCatalogMetric::CsrMinutes:
            out.csr_minutes = value;
            break;
        default:
            return;
    }

    out.valid_mask |= bit;
    if (source == NightCatalogMetricSource::Str) {
        out.str_mask |= bit;
        out.summary_mask &= ~bit;
    } else if (source == NightCatalogMetricSource::Summary) {
        out.summary_mask |= bit;
        out.str_mask &= ~bit;
    }
    used = true;
}

bool apply_metrics(NightCatalogMetrics &out,
                   const ReportDailyMetrics &input,
                   NightCatalogMetricSource source,
                   bool fill_only,
                   bool include_duration) {
    bool used = false;
    if (input.has_ahi) {
        set_metric(out, NightCatalogMetric::Ahi, input.ahi,
                   source, fill_only, used);
    }
    if (input.has_oa_index) {
        set_metric(out, NightCatalogMetric::ObstructiveApneaIndex,
                   input.oa_index, source, fill_only, used);
    }
    if (input.has_ca_index) {
        set_metric(out, NightCatalogMetric::CentralApneaIndex,
                   input.ca_index, source, fill_only, used);
    }
    if (input.has_ua_index) {
        set_metric(out, NightCatalogMetric::UnknownApneaIndex,
                   input.ua_index, source, fill_only, used);
    }
    if (input.has_hypopnea_index) {
        set_metric(out, NightCatalogMetric::HypopneaIndex,
                   input.hypopnea_index, source, fill_only, used);
    }
    if (input.has_arousal_index) {
        set_metric(out, NightCatalogMetric::ArousalIndex,
                   input.arousal_index, source, fill_only, used);
    }
    if (input.has_mask_pressure_50) {
        set_metric(out, NightCatalogMetric::MaskPressure50,
                   input.mask_pressure_50_cm_h2o, source, fill_only, used);
    }
    if (input.has_leak_50) {
        set_metric(out, NightCatalogMetric::Leak50,
                   input.leak_50_l_min, source, fill_only, used);
    }
    if (input.has_mask_pressure_95) {
        set_metric(out, NightCatalogMetric::MaskPressure95,
                   input.mask_pressure_95_cm_h2o,
                   source, fill_only, used);
    }
    if (input.has_leak_95) {
        set_metric(out, NightCatalogMetric::Leak95,
                   input.leak_95_l_min, source, fill_only, used);
    }
    if (input.has_minute_ventilation_50) {
        set_metric(out, NightCatalogMetric::MinuteVentilation50,
                   input.minute_ventilation_50_l_min,
                   source, fill_only, used);
    }
    if (input.has_minute_ventilation_95) {
        set_metric(out, NightCatalogMetric::MinuteVentilation95,
                   input.minute_ventilation_95_l_min,
                   source, fill_only, used);
    }
    if (input.has_respiratory_rate_50) {
        set_metric(out, NightCatalogMetric::RespiratoryRate50,
                   input.respiratory_rate_50_bpm,
                   source, fill_only, used);
    }
    if (input.has_respiratory_rate_95) {
        set_metric(out, NightCatalogMetric::RespiratoryRate95,
                   input.respiratory_rate_95_bpm,
                   source, fill_only, used);
    }
    if (input.has_tidal_volume_50) {
        set_metric(out, NightCatalogMetric::TidalVolume50,
                   input.tidal_volume_50_l,
                   source, fill_only, used);
    }
    if (input.has_tidal_volume_95) {
        set_metric(out, NightCatalogMetric::TidalVolume95,
                   input.tidal_volume_95_l,
                   source, fill_only, used);
    }
    if (input.has_spo2_50) {
        set_metric(out, NightCatalogMetric::Spo2Median,
                   input.spo2_50_percent,
                   source, fill_only, used);
    }
    if (input.has_spo2_threshold_minutes) {
        set_uint_metric(out, NightCatalogMetric::Spo2ThresholdMinutes,
                        input.spo2_threshold_minutes,
                        source, fill_only, used);
    }
    if (input.has_csr_minutes) {
        set_uint_metric(out, NightCatalogMetric::CsrMinutes,
                        input.csr_minutes,
                        source, fill_only, used);
    }
    if (include_duration && input.has_duration_min) {
        set_uint_metric(out, NightCatalogMetric::DurationMinutes,
                        input.duration_min, source, fill_only, used);
    }
    return used;
}

uint64_t hash_byte(uint64_t hash, uint8_t value) {
    hash ^= value;
    return hash * FNV_PRIME;
}

uint64_t hash_u64(uint64_t hash, uint64_t value) {
    for (uint8_t i = 0; i < 8; ++i) {
        hash = hash_byte(hash, static_cast<uint8_t>(value & 0xffu));
        value >>= 8;
    }
    return hash;
}

uint64_t hash_u32(uint64_t hash, uint32_t value) {
    for (uint8_t i = 0; i < 4; ++i) {
        hash = hash_byte(hash, static_cast<uint8_t>(value & 0xffu));
        value >>= 8;
    }
    return hash;
}

uint64_t hash_i64(uint64_t hash, int64_t value) {
    return hash_u64(hash, static_cast<uint64_t>(value));
}

uint64_t hash_float(uint64_t hash, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float must be 32 bits");
    memcpy(&bits, &value, sizeof(bits));
    return hash_u32(hash, bits);
}

uint64_t hash_text(uint64_t hash, const char *text, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        hash = hash_byte(hash, static_cast<uint8_t>(text[i]));
    }
    return hash_byte(hash, 0);
}

uint64_t source_file_identity(const NightCatalogSourceFileInput &file) {
    if (file.identity != 0) {
        return file.provenance_identity != 0
            ? hash_u64(file.identity, file.provenance_identity)
            : file.identity;
    }

    const char *path = file.path ? file.path : "";
    uint64_t hash = FNV_OFFSET;
    hash = hash_byte(hash, static_cast<uint8_t>(file.kind));
    hash = hash_text(hash, path, strlen(path));
    hash = hash_u64(hash, file.file_size);
    hash = hash_i64(hash, file.last_write_ms);
    hash = hash_u64(hash, file.data_offset);
    hash = hash_u64(hash, file.data_size);
    hash = hash_i64(hash, file.record_start_ms);
    hash = hash_u32(hash, file.header_size);
    hash = hash_u32(hash, file.record_size);
    hash = hash_u32(hash, file.record_duration_ms);
    hash = hash_u32(hash, file.complete_records);
    hash = hash_i64(hash, file.coverage.range.start_ms);
    hash = hash_i64(hash, file.coverage.range.end_ms);
    hash = hash_u32(hash, file.coverage.primary_signal_mask);
    hash = hash_u32(hash, file.coverage.fallback_signal_mask);
    hash = hash_u32(hash, static_cast<uint32_t>(file.signal_layout_count));
    for (size_t i = 0; i < file.signal_layout_count; ++i) {
        const EdfReportSignalLayout &layout = file.signal_layouts[i];
        hash = hash_byte(hash, static_cast<uint8_t>(layout.signal));
        hash = hash_byte(hash, static_cast<uint8_t>(layout.source));
        hash = hash_byte(hash, layout.primary ? 1 : 0);
        hash = hash_u32(hash, layout.samples_per_record);
        hash = hash_u32(hash, layout.byte_offset_in_record);
        hash = hash_u32(hash, layout.sample_interval_ms);
        hash = hash_u32(hash, static_cast<uint16_t>(layout.scale.digital_min));
        hash = hash_u32(hash, static_cast<uint16_t>(layout.scale.digital_max));
        hash = hash_float(hash, layout.scale.scale);
        hash = hash_float(hash, layout.scale.offset);
    }
    if (file.provenance_identity != 0) {
        hash = hash_u64(hash, file.provenance_identity);
    }
    return hash == 0 ? 1 : hash;
}

bool signal_layouts_valid(const NightCatalogSourceFileInput &file) {
    if (file.signal_layout_count > 0 && !file.signal_layouts) return false;

    uint32_t seen_primary = 0;
    uint32_t seen_fallback = 0;
    for (size_t i = 0; i < file.signal_layout_count; ++i) {
        const EdfReportSignalLayout &layout = file.signal_layouts[i];
        const uint32_t bit = report_signal_bit(layout.signal);
        const uint64_t signal_end =
            static_cast<uint64_t>(layout.byte_offset_in_record) +
            static_cast<uint64_t>(layout.samples_per_record) * 2u;
        uint32_t &seen = layout.primary ? seen_primary : seen_fallback;
        if (bit == 0 ||
            static_cast<uint8_t>(layout.source) >
                static_cast<uint8_t>(ReportSourceId::TriggerCycleEvent) ||
            layout.samples_per_record == 0 ||
            layout.sample_interval_ms == 0 || signal_end > file.record_size ||
            layout.scale.digital_max <= layout.scale.digital_min ||
            !std::isfinite(layout.scale.scale) ||
            !std::isfinite(layout.scale.offset) || layout.scale.scale <= 0.0f ||
            (seen & bit) != 0) {
            return false;
        }
        seen |= bit;
    }

    return seen_primary == file.coverage.primary_signal_mask &&
           seen_fallback == file.coverage.fallback_signal_mask;
}

BuildNight *find_or_add_night(LargeScratchArray<BuildNight> &nights,
                              SleepDayId sleep_day) {
    if (!sleep_day.valid()) return nullptr;

    for (size_t i = 0; i < nights.size(); ++i) {
        if (nights.data()[i].sleep_day == sleep_day) {
            return &nights.data()[i];
        }
    }

    BuildNight *night = nights.append();
    if (!night) return nullptr;
    night->sleep_day = sleep_day;
    night->owner = nights.size() - 1;
    return night;
}

BuildNight *find_night(LargeScratchArray<BuildNight> &nights,
                       SleepDayId sleep_day) {
    if (!sleep_day.valid()) return nullptr;

    for (size_t i = 0; i < nights.size(); ++i) {
        if (nights.data()[i].sleep_day == sleep_day) {
            return &nights.data()[i];
        }
    }
    return nullptr;
}

bool timestamps_within(int64_t lhs, int64_t rhs, int64_t tolerance_ms) {
    if (lhs >= rhs) return lhs - rhs <= tolerance_ms;
    return rhs - lhs <= tolerance_ms;
}

bool summary_session_matches_raw_edf(
    const NightCatalogBuildInput &input,
    SleepDayId raw_sleep_day,
    const NightCatalogTimeRange &session) {
    for (size_t i = 0; i < input.edf_session_count; ++i) {
        const NightCatalogEdfSessionInput &edf = input.edf_sessions[i];
        if (!edf.has_clock_provenance ||
            edf.raw_sleep_day != raw_sleep_day) {
            continue;
        }

        const NightCatalogTimeRange raw_window =
            edf.raw_therapy_window.valid()
                ? edf.raw_therapy_window
                : edf.raw_segment_window;
        if (raw_window.valid() &&
            timestamps_within(session.start_ms,
                              raw_window.start_ms,
                              SUMMARY_SESSION_EDGE_TOLERANCE_MS) &&
            timestamps_within(session.end_ms,
                              raw_window.end_ms,
                              SUMMARY_SESSION_EDGE_TOLERANCE_MS)) {
            return true;
        }
    }
    return false;
}

bool set_primary_boundary(BuildNight &night,
                          int64_t start_ms,
                          int64_t end_ms) {
    if (!valid_boundary(start_ms, end_ms)) return false;
    if (!night.boundary_set) {
        night.day_start_ms = start_ms;
        night.day_end_ms = end_ms;
        night.boundary_set = true;
        return true;
    }
    return night.day_start_ms == start_ms && night.day_end_ms == end_ms;
}

bool append_session(LargeScratchArray<BuildSession> &sessions,
                    size_t owner,
                    SessionOrigin origin,
                    const NightCatalogTimeRange &range) {
    if (!range.valid()) return false;

    BuildSession *session = sessions.append();
    if (!session) return false;
    session->owner = owner;
    session->origin = origin;
    session->range = range;
    return true;
}

bool append_file(LargeScratchArray<BuildFile> &files,
                 size_t owner,
                 const NightCatalogSourceFileInput &source,
                 const NightCatalogTimeRange *session_range) {
    if (!source.path || !source.path[0] || !signal_layouts_valid(source)) {
        return false;
    }
    const bool point_annotation =
        (source.kind == NightCatalogFileKind::Eve ||
         source.kind == NightCatalogFileKind::Csl) &&
        source.coverage.range.start_ms > 0 &&
        source.coverage.range.end_ms >= source.coverage.range.start_ms;
    if (source.kind != NightCatalogFileKind::Str &&
        !source.coverage.range.valid() && !point_annotation) {
        return false;
    }

    BuildFile *file = files.append();
    if (!file) return false;
    file->owner = owner;
    file->source = source;
    if (session_range) {
        file->session_range = *session_range;
        file->has_session = true;
    }
    file->source.identity = source_file_identity(source);
    return true;
}

bool fallback_payload_valid(const NightCatalogFallbackInput &source) {
    for (size_t i = 0; i < source.section_count; ++i) {
        const NightCatalogFallbackSectionInput &section = source.sections[i];
        if (!report_fallback_section_valid(section,
                                           source.day_start_ms,
                                           source.day_end_ms,
                                           source.metadata_bytes,
                                           source.file_size)) {
            return false;
        }
    }

    return report_fallback_payload_layout_valid(source.metadata_bytes,
                                                source.file_size,
                                                source.sections,
                                                source.section_count);
}

const NightCatalogSummaryInput *find_summary(
    const NightCatalogBuildInput &input,
    SleepDayId sleep_day) {
    for (size_t i = 0; i < input.summary_record_count; ++i) {
        const NightCatalogSummaryInput &summary = input.summary_records[i];
        if (summary.sleep_day == sleep_day) return &summary;
    }

    return nullptr;
}

bool summary_has_session(const NightCatalogSummaryInput &summary,
                         const NightCatalogTimeRange &session) {
    for (size_t i = 0; i < summary.session_count; ++i) {
        if (same_range(summary.sessions[i], session)) return true;
    }

    return false;
}

bool fallback_input_valid(const NightCatalogFallbackInput &source) {
    if (!source.sleep_day.valid() || !source.path || !source.path[0] ||
        !valid_boundary(source.day_start_ms, source.day_end_ms) ||
        !valid_timezone_offset(source.source_timezone_offset_valid,
                               source.source_timezone_offset_minutes) ||
        !valid_timezone_offset(source.resolved_timezone_offset_valid,
                               source.resolved_timezone_offset_minutes) ||
        source.identity == 0 || source.metadata_bytes == 0 ||
        source.metadata_bytes > source.file_size ||
        source.session_count == 0 || !source.sessions ||
        source.section_count == 0 || !source.sections ||
        source.section_count > UINT16_MAX) {
        return false;
    }

    NightCatalogTimeRange previous_session;
    for (size_t i = 0; i < source.session_count; ++i) {
        const NightCatalogTimeRange &session = source.sessions[i];
        if (!session.valid() || session.start_ms < source.day_start_ms ||
            session.end_ms > source.day_end_ms ||
            (i > 0 && session.start_ms < previous_session.end_ms)) {
            return false;
        }
        previous_session = session;
    }

    return fallback_payload_valid(source);
}

bool resolve_fallback_adjustment(
    const NightCatalogFallbackInput &fallback,
    const NightCatalogSummaryInput &summary,
    int32_t &adjustment_ms) {
    adjustment_ms = 0;

    int64_t start_adjustment = 0;
    if (fallback.source_timezone_offset_valid &&
        summary.timezone_offset_valid) {
        start_adjustment =
            static_cast<int64_t>(fallback.source_timezone_offset_minutes -
                                 summary.timezone_offset_minutes) *
            60000LL;
    } else {
        int64_t end_adjustment = 0;
        if (!subtract_time(summary.day_start_ms,
                           fallback.day_start_ms,
                           start_adjustment) ||
            !subtract_time(summary.day_end_ms,
                           fallback.day_end_ms,
                           end_adjustment) ||
            start_adjustment != end_adjustment) {
            return false;
        }
    }
    if (start_adjustment < INT32_MIN || start_adjustment > INT32_MAX) {
        return false;
    }

    int64_t adjusted_day_start = 0;
    int64_t adjusted_day_end = 0;
    if (!adjust_time(fallback.day_start_ms,
                     start_adjustment,
                     adjusted_day_start) ||
        !adjust_time(fallback.day_end_ms,
                     start_adjustment,
                     adjusted_day_end) ||
        adjusted_day_start != summary.day_start_ms ||
        adjusted_day_end != summary.day_end_ms) {
        return false;
    }

    for (size_t i = 0; i < fallback.session_count; ++i) {
        NightCatalogTimeRange adjusted;
        if (!adjust_range(fallback.sessions[i],
                          start_adjustment,
                          adjusted) ||
            adjusted.start_ms < summary.day_start_ms ||
            adjusted.end_ms > summary.day_end_ms) {
            return false;
        }
    }
    for (size_t i = 0; i < fallback.section_count; ++i) {
        NightCatalogTimeRange adjusted;
        if (!adjust_range(fallback.sections[i].coverage,
                          start_adjustment,
                          adjusted) ||
            adjusted.start_ms < summary.day_start_ms ||
            adjusted.end_ms > summary.day_end_ms) {
            return false;
        }
    }

    adjustment_ms = static_cast<int32_t>(start_adjustment);
    return true;
}

bool ingest_fallback(const NightCatalogBuildInput &input,
                     LargeScratchArray<BuildNight> &nights,
                     LargeScratchArray<BuildSession> &sessions,
                     LargeScratchArray<BuildFallback> &fallbacks,
                     size_t &invalid_fallback_records, size_t index) {
    const NightCatalogFallbackInput &source = input.fallback_records[index];

    BuildNight *night = find_night(nights, source.sleep_day);
    if (night && night->has_edf && !source.retain_with_edf) return true;

    if (!fallback_input_valid(source) || (night && night->has_fallback)) {
        ++invalid_fallback_records;
        return true;
    }

    const NightCatalogSummaryInput *summary = find_summary(input, source.sleep_day);
    int32_t adjustment_ms = source.time_adjust_ms;
    // EDF already owns this night's clock and sessions. Keep the saved
    // fallback transform even when Summary is present during a full scan.
    const bool use_summary_axis =
        !source.coordinates_are_resolved && !(night && night->has_edf) && summary &&
        resolve_fallback_adjustment(source, *summary, adjustment_ms);

    int64_t fallback_day_start_ms = source.day_start_ms;
    int64_t fallback_day_end_ms = source.day_end_ms;
    if (!source.coordinates_are_resolved && !use_summary_axis &&
        (!adjust_time(source.day_start_ms, adjustment_ms, fallback_day_start_ms) ||
         !adjust_time(source.day_end_ms, adjustment_ms, fallback_day_end_ms) ||
         !valid_boundary(fallback_day_start_ms, fallback_day_end_ms))) {
        ++invalid_fallback_records;
        return true;
    }

    if (!night) night = find_or_add_night(nights, source.sleep_day);
    if (!night) return false;

    if (use_summary_axis) {
        if (!night->has_summary || night->day_start_ms != summary->day_start_ms ||
            night->day_end_ms != summary->day_end_ms) {
            return false;
        }
        night->fallback_joins_summary = true;
    } else if (night->has_edf) {
        if (night->day_start_ms != fallback_day_start_ms ||
            night->day_end_ms != fallback_day_end_ms) {
            return false;
        }
    } else {
        night->day_start_ms = fallback_day_start_ms;
        night->day_end_ms = fallback_day_end_ms;
        night->boundary_set = true;
        night->fallback_joins_summary = false;
        night->has_summary = false;
        night->summary_identity = 0;
        night->summary_metrics_valid = false;

        if (source.resolved_timezone_offset_valid) {
            night->timezone_offset_minutes = source.resolved_timezone_offset_minutes;
            night->timezone_offset_valid = true;
        } else if (source.source_timezone_offset_valid && adjustment_ms % 60000 == 0) {
            const int64_t resolved_minutes =
                static_cast<int64_t>(source.source_timezone_offset_minutes) -
                adjustment_ms / 60000;
            if (resolved_minutes >= -24 * 60 && resolved_minutes <= 24 * 60) {
                night->timezone_offset_minutes = static_cast<int32_t>(resolved_minutes);
                night->timezone_offset_valid = true;
            } else {
                night->timezone_offset_minutes = 0;
                night->timezone_offset_valid = false;
            }
        } else {
            night->timezone_offset_minutes = 0;
            night->timezone_offset_valid = false;
        }
    }

    for (size_t session_index = 0; session_index < source.session_count;
         ++session_index) {
        NightCatalogTimeRange session = source.sessions[session_index];
        if (!source.coordinates_are_resolved && adjustment_ms != 0) {
            NightCatalogTimeRange adjusted;
            if (!adjust_range(session, adjustment_ms, adjusted)) {
                return false;
            }
            session = adjusted;
        }
        if (!append_session(sessions, night->owner, SessionOrigin::Fallback, session)) {
            return false;
        }

        if (use_summary_axis && night->summary_metrics_valid &&
            !summary_has_session(*summary, session)) {
            night->summary_metrics_valid = false;
        }
    }

    BuildFallback *fallback = fallbacks.append();
    if (!fallback) return false;
    fallback->owner = night->owner;
    fallback->source = source;
    fallback->time_adjust_ms = adjustment_ms;
    night->has_fallback = true;
    night->local_history = source.local_history;
    return true;
}

bool ingest_edf(const NightCatalogBuildInput &input,
                LargeScratchArray<BuildNight> &nights,
                LargeScratchArray<BuildSession> &sessions,
                LargeScratchArray<BuildFile> &files, size_t index) {
    const NightCatalogEdfSessionInput &source = input.edf_sessions[index];
    if (!source.display_window.valid() || (source.file_count > 0 && !source.files)) {
        return false;
    }

    BuildNight *night = find_or_add_night(nights, source.sleep_day);
    if (!night ||
        !set_primary_boundary(*night, source.day_start_ms, source.day_end_ms) ||
        !append_session(sessions, night->owner, SessionOrigin::Edf,
                        source.display_window)) {
        return false;
    }

    night->has_edf = true;
    night->active_capture = night->active_capture || source.active_capture;
    for (size_t file_index = 0; file_index < source.file_count; ++file_index) {
        if (!append_file(files, night->owner, source.files[file_index],
                         &source.display_window)) {
            return false;
        }
    }
    return true;
}

bool ingest_str(const NightCatalogBuildInput &input,
                LargeScratchArray<BuildNight> &nights,
                LargeScratchArray<BuildFile> &files, size_t index) {
    const NightCatalogStrInput &source = input.str_records[index];
    if (!source.record.sleep_day.valid() || !source.path || !source.path[0] ||
        source.record_size == 0 || source.record.source_identity == 0) {
        return false;
    }

    BuildNight *night = find_night(nights, source.record.sleep_day);
    if (!night || !night->has_edf) return true;
    if (night->has_str) return false;

    NightCatalogSourceFileInput file;
    file.kind = NightCatalogFileKind::Str;
    file.path = source.path;
    file.coverage.range = {night->day_start_ms, night->day_end_ms};
    file.file_size = source.file_size;
    file.last_write_ms = source.last_write_ms;
    file.data_offset = source.record_offset;
    file.data_size = source.record_size;
    file.identity = source.record.source_identity;
    file.record_size = source.record_size;
    file.complete_records = 1;
    if (!append_file(files, night->owner, file, nullptr)) return false;

    night->str_metrics = source.record.metrics;
    night->has_str = true;
    return true;
}

bool ingest_summary(const NightCatalogBuildInput &input,
                    LargeScratchArray<BuildNight> &nights,
                    LargeScratchArray<BuildSession> &sessions, size_t index) {
    const NightCatalogSummaryInput &source = input.summary_records[index];
    if (!source.sleep_day.valid() || source.identity == 0 ||
        !valid_timezone_offset(source.timezone_offset_valid,
                               source.timezone_offset_minutes) ||
        !valid_boundary(source.day_start_ms, source.day_end_ms) ||
        (source.session_count > 0 && !source.sessions)) {
        return false;
    }

    BuildNight *night = find_night(nights, source.sleep_day);
    const bool edf_owned = night && night->has_edf;
    LargeScratchArray<bool> matched_sessions;
    if (!edf_owned && !matched_sessions.allocate(source.session_count)) return false;

    size_t matched_session_count = 0;
    NightCatalogTimeRange previous_session;
    for (size_t session_index = 0; session_index < source.session_count;
         ++session_index) {
        const NightCatalogTimeRange &session = source.sessions[session_index];
        if (session.start_ms < source.day_start_ms ||
            session.end_ms > source.day_end_ms ||
            (session_index > 0 && session.start_ms < previous_session.end_ms)) {
            return false;
        }
        if (!edf_owned) {
            const bool matched = summary_session_matches_raw_edf(
                input, source.sleep_day, session);
            *matched_sessions.append() = matched;
            if (matched) ++matched_session_count;
        }
        previous_session = session;
    }

    if (edf_owned ||
        (source.session_count > 0 && matched_session_count == source.session_count)) {
        return true;
    }
    if (!night) night = find_or_add_night(nights, source.sleep_day);
    if (!night || night->has_summary ||
        !set_primary_boundary(*night, source.day_start_ms, source.day_end_ms)) {
        return false;
    }

    for (size_t session_index = 0; session_index < source.session_count;
         ++session_index) {
        if (matched_sessions.data()[session_index]) {
            continue;
        }
        if (!append_session(sessions, night->owner, SessionOrigin::Summary,
                            source.sessions[session_index])) {
            return false;
        }
    }

    night->summary_metrics = source.metrics;
    night->summary_identity = source.identity;
    night->timezone_offset_minutes = source.timezone_offset_minutes;
    night->timezone_offset_valid = source.timezone_offset_valid;
    night->has_summary = true;
    night->summary_metrics_valid = matched_session_count == 0;
    night->summary_expired = source.expired;
    return true;
}

bool selected_session(const BuildNight &night,
                      const BuildSession &session) {
    if (session.owner != night.owner) return false;
    if (night.has_edf) return session.origin == SessionOrigin::Edf;
    if (!night.has_fallback) return session.origin == SessionOrigin::Summary;

    return session.origin == SessionOrigin::Fallback ||
           (night.fallback_joins_summary &&
            session.origin == SessionOrigin::Summary);
}

template <typename T>
size_t first_owned_index(const LargeScratchArray<T> &entries, size_t owner) {
    if (entries.size() == 0) return 0;

    return static_cast<size_t>(std::lower_bound(
        entries.data(), entries.data() + entries.size(), owner,
        [](const T &entry, size_t value) { return entry.owner < value; }
    ) - entries.data());
}

size_t count_unique_sessions(const BuildNight &night,
                             const LargeScratchArray<BuildSession> &sessions) {
    size_t count = 0;
    NightCatalogTimeRange previous;
    bool have_previous = false;
    for (size_t i = first_owned_index(sessions, night.owner);
         i < sessions.size(); ++i) {
        const BuildSession &session = sessions.data()[i];
        if (session.owner != night.owner) break;
        if (!selected_session(night, session)) continue;
        if (have_previous && same_range(previous, session.range)) continue;
        previous = session.range;
        have_previous = true;
        ++count;
    }
    return count;
}

size_t count_files(const BuildNight &night,
                   const LargeScratchArray<BuildFile> &files,
                   size_t &path_bytes) {
    size_t count = 0;
    for (size_t i = first_owned_index(files, night.owner);
         i < files.size(); ++i) {
        const BuildFile &file = files.data()[i];
        if (file.owner != night.owner) break;

        const size_t len = strlen(file.source.path);
        if (len > UINT16_MAX || path_bytes > UINT32_MAX - len - 1) {
            return SIZE_MAX;
        }
        path_bytes += len + 1;
        ++count;
    }
    return count;
}

size_t count_fallback_files(const BuildNight &night,
                            const LargeScratchArray<BuildFallback> &fallbacks,
                            size_t &section_count,
                            size_t &path_bytes) {
    size_t count = 0;
    for (size_t i = first_owned_index(fallbacks, night.owner);
         i < fallbacks.size(); ++i) {
        const BuildFallback &fallback = fallbacks.data()[i];
        if (fallback.owner != night.owner) break;

        const size_t len = strlen(fallback.source.path);
        if (len > UINT16_MAX || path_bytes > UINT32_MAX - len - 1 ||
            !add_count(section_count, fallback.source.section_count)) {
            return SIZE_MAX;
        }
        path_bytes += len + 1;
        ++count;
    }
    return count;
}

uint16_t find_session_index(const NightCatalog &catalog,
                            const NightCatalogRecord &record,
                            const NightCatalogTimeRange &range) {
    size_t count = 0;
    const NightCatalogTimeRange *sessions = catalog.sessions(record, count);
    for (size_t i = 0; i < count; ++i) {
        if (same_range(sessions[i], range) && i <= UINT16_MAX) {
            return static_cast<uint16_t>(i);
        }
    }
    return NIGHT_CATALOG_NO_SESSION;
}

uint64_t calculate_revision(const NightCatalog &catalog,
                            const NightCatalogRecord &record) {
    uint64_t hash = FNV_OFFSET;
    hash = hash_u32(hash, NIGHT_CATALOG_SOURCE_REVISION_POLICY);
    hash = hash_u32(hash,
                    static_cast<uint32_t>(record.sleep_day.epoch_days()));
    hash = hash_i64(hash, record.day_start_ms);
    hash = hash_i64(hash, record.day_end_ms);
    hash = hash_byte(hash, record.timezone_offset_valid ? 1 : 0);
    if (record.timezone_offset_valid) {
        hash = hash_i64(hash, record.timezone_offset_minutes);
    }
    hash = hash_byte(
        hash,
        record.source_flags & ~NIGHT_CATALOG_SOURCE_SUMMARY_EXPIRED);
    if ((record.source_flags & NIGHT_CATALOG_SOURCE_EDF) != 0) {
        hash = hash_u32(hash, NIGHT_CATALOG_EDF_DERIVATION_POLICY);
    }
    hash = hash_u32(hash, record.metrics.valid_mask);
    hash = hash_u32(hash, record.metrics.str_mask);
    hash = hash_u32(hash, record.metrics.summary_mask);
    hash = hash_float(hash, record.metrics.ahi);
    hash = hash_float(hash, record.metrics.obstructive_apnea_index);
    hash = hash_float(hash, record.metrics.central_apnea_index);
    hash = hash_float(hash, record.metrics.unknown_apnea_index);
    hash = hash_float(hash, record.metrics.hypopnea_index);
    hash = hash_float(hash, record.metrics.arousal_index);
    hash = hash_float(hash, record.metrics.mask_pressure_50_cm_h2o);
    hash = hash_float(hash, record.metrics.leak_50_l_min);
    hash = hash_u32(hash, record.metrics.duration_min);
    hash = hash_float(hash, record.metrics.mask_pressure_95_cm_h2o);
    hash = hash_float(hash, record.metrics.leak_95_l_min);
    hash = hash_float(hash, record.metrics.minute_ventilation_50_l_min);
    hash = hash_float(hash, record.metrics.minute_ventilation_95_l_min);
    hash = hash_float(hash, record.metrics.respiratory_rate_50_bpm);
    hash = hash_float(hash, record.metrics.respiratory_rate_95_bpm);
    hash = hash_float(hash, record.metrics.tidal_volume_50_l);
    hash = hash_float(hash, record.metrics.tidal_volume_95_l);
    hash = hash_float(hash, record.metrics.spo2_median_percent);
    hash = hash_u32(hash, record.metrics.spo2_threshold_minutes);
    hash = hash_u32(hash, record.metrics.csr_minutes);

    if ((record.source_flags & NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) != 0) {
        hash = hash_u64(hash, record.summary_identity);
    }

    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(record, session_count);
    hash = hash_u32(hash, static_cast<uint32_t>(session_count));
    for (size_t i = 0; i < session_count; ++i) {
        hash = hash_i64(hash, sessions[i].start_ms);
        hash = hash_i64(hash, sessions[i].end_ms);
    }

    size_t mask_count = 0;
    const NightCatalogTimeRange *masks =
        catalog.mask_windows(record, mask_count);
    hash = hash_u32(hash, static_cast<uint32_t>(mask_count));
    for (size_t i = 0; i < mask_count; ++i) {
        hash = hash_i64(hash, masks[i].start_ms);
        hash = hash_i64(hash, masks[i].end_ms);
    }

    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(record, file_count);
    hash = hash_u32(hash, static_cast<uint32_t>(file_count));
    for (size_t i = 0; i < file_count; ++i) {
        const NightCatalogSourceFile &file = files[i];
        hash = hash_byte(hash, static_cast<uint8_t>(file.kind));
        hash = hash_u64(hash, file.identity);
        hash = hash_u64(hash, file.file_size);
        hash = hash_i64(hash, file.last_write_ms);
        hash = hash_u64(hash, file.data_offset);
        hash = hash_u64(hash, file.data_size);
        hash = hash_i64(hash, file.record_start_ms);
        hash = hash_u32(hash, file.header_size);
        hash = hash_u32(hash, file.record_size);
        hash = hash_u32(hash, file.record_duration_ms);
        hash = hash_u32(hash, file.complete_records);

        const char *path = catalog.path(file);
        hash = hash_text(hash, path ? path : "", file.path_length);

        size_t coverage_count = 0;
        const NightCatalogSourceCoverage *coverage =
            catalog.coverage(file, coverage_count);
        hash = hash_u32(hash, static_cast<uint32_t>(coverage_count));
        for (size_t coverage_index = 0;
             coverage_index < coverage_count;
             ++coverage_index) {
            hash = hash_i64(hash, coverage[coverage_index].range.start_ms);
            hash = hash_i64(hash, coverage[coverage_index].range.end_ms);
            hash = hash_u32(hash,
                            coverage[coverage_index].primary_signal_mask);
            hash = hash_u32(hash,
                            coverage[coverage_index].fallback_signal_mask);
        }

        size_t signal_layout_count = 0;
        const EdfReportSignalLayout *signal_layouts =
            catalog.signal_layouts(file, signal_layout_count);
        hash = hash_u32(hash, static_cast<uint32_t>(signal_layout_count));
        for (size_t layout_index = 0;
             layout_index < signal_layout_count;
             ++layout_index) {
            const EdfReportSignalLayout &layout = signal_layouts[layout_index];
            hash = hash_byte(hash, static_cast<uint8_t>(layout.signal));
            hash = hash_byte(hash, static_cast<uint8_t>(layout.source));
            hash = hash_byte(hash, layout.primary ? 1 : 0);
            hash = hash_u32(hash, layout.samples_per_record);
            hash = hash_u32(hash, layout.byte_offset_in_record);
            hash = hash_u32(hash, layout.sample_interval_ms);
            hash = hash_u32(
                hash, static_cast<uint16_t>(layout.scale.digital_min));
            hash = hash_u32(
                hash, static_cast<uint16_t>(layout.scale.digital_max));
            hash = hash_float(hash, layout.scale.scale);
            hash = hash_float(hash, layout.scale.offset);
        }
    }

    size_t fallback_file_count = 0;
    const NightCatalogFallbackFile *fallback_files =
        catalog.fallback_files(record, fallback_file_count);
    hash = hash_u32(hash, static_cast<uint32_t>(fallback_file_count));
    for (size_t i = 0; i < fallback_file_count; ++i) {
        const NightCatalogFallbackFile &file = fallback_files[i];
        hash = hash_u64(hash, file.identity);
        hash = hash_i64(hash, file.time_adjust_ms);

        size_t section_count = 0;
        const NightCatalogFallbackSection *sections =
            catalog.fallback_sections(file, section_count);
        hash = hash_u32(hash, static_cast<uint32_t>(section_count));
        for (size_t section_index = 0;
             section_index < section_count;
             ++section_index) {
            const NightCatalogFallbackSection &section =
                sections[section_index];
            hash = hash_byte(hash, static_cast<uint8_t>(section.kind));
            hash = hash_byte(hash, static_cast<uint8_t>(section.source));
            hash = hash_byte(hash, static_cast<uint8_t>(section.signal));
            hash = hash_byte(hash, section.event_mask);
            hash = hash_u32(hash, section.payload_schema);
            hash = hash_u32(hash, section.record_count);
            hash = hash_u32(hash, section.sample_interval_ms);
            hash = hash_i64(hash, section.coverage.start_ms);
            hash = hash_i64(hash, section.coverage.end_ms);
            hash = hash_u64(hash, section.data_offset);
            hash = hash_u32(hash, section.data_size);
            hash = hash_u32(hash, section.data_crc32);
        }
    }

    return hash == 0 ? 1 : hash;
}

}  // namespace

struct NightCatalogBuilder::Runtime {
    enum class Phase {
        CountEdf,
        CountSummary,
        CountFallback,
        AllocateNights,
        AllocateSessions,
        AllocateFiles,
        AllocateFallbacks,
        Edf,
        Str,
        Summary,
        Fallback,
        SortNights,
        SortSessions,
        SortFiles,
        SortFallbacks,
        CountOutput,
        AllocateOutput,
        WriteOutput,
        Finish,
        Done
    };

    explicit Runtime(const NightCatalogBuildInput &value,
                     NightCatalogBuildStatus &result)
        : input(value), status(result) {}

    bool fail(NightCatalogBuildFailure failure, const char *detail) {
        status.failure = failure;
        status.detail = detail;
        phase = Phase::Done;
        catalog.reset();
        return false;
    }

    void advance(Phase next) {
        phase = next;
        cursor = 0;
        sort.reset();
    }

    bool count_night() {
        BuildNight &night = nights.data()[cursor];
        if (!night.boundary_set) {
            return fail(NightCatalogBuildFailure::InvariantViolation,
                        "night_catalog_boundary_missing");
        }

        const size_t session_count = count_unique_sessions(night, sessions);
        const size_t file_count = count_files(night, files, final_paths);
        const size_t fallback_file_count = count_fallback_files(
            night,
            fallbacks,
            final_fallback_sections,
            final_paths);
        if (file_count == SIZE_MAX || session_count > UINT16_MAX ||
            fallback_file_count == SIZE_MAX ||
            file_count > UINT16_MAX ||
            fallback_file_count > UINT16_MAX ||
            !add_count(final_sessions, session_count) ||
            !add_count(final_files, file_count) ||
            !add_count(final_fallback_files, fallback_file_count)) {
            return fail(NightCatalogBuildFailure::InvariantViolation,
                        "night_catalog_output_count_invalid");
        }

        for (size_t file_index = first_owned_index(files, night.owner);
             file_index < files.size(); ++file_index) {
            const BuildFile &file = files.data()[file_index];
            if (file.owner != night.owner) break;
            if (file.source.signal_layout_count > UINT16_MAX ||
                !add_count(final_signal_layouts,
                           file.source.signal_layout_count)) {
                return fail(NightCatalogBuildFailure::InvariantViolation,
                            "night_catalog_signal_count_invalid");
            }
        }

        return true;
    }

    bool allocate_output() {
        if (nights.size() > UINT32_MAX || final_sessions > UINT32_MAX ||
            final_files > UINT32_MAX || final_signal_layouts > UINT32_MAX ||
            final_fallback_files > UINT32_MAX || final_fallback_sections > UINT32_MAX ||
            final_paths > UINT32_MAX) {
            return fail(NightCatalogBuildFailure::InvariantViolation,
                        "night_catalog_output_count_invalid");
        }

        catalog.reset(new (std::nothrow) NightCatalog());
        if (!catalog ||
            !catalog->allocate(nights.size(), final_sessions, 0, final_files,
                               final_files, final_signal_layouts, final_fallback_files,
                               final_fallback_sections, final_paths)) {
            return fail(NightCatalogBuildFailure::AllocationFailed,
                        "night_catalog_output_alloc_failed");
        }

        return true;
    }

    bool write_night() {
        const BuildNight &source = nights.data()[cursor];
        NightCatalogRecord &record = catalog->records_[cursor];
        record.sleep_day = source.sleep_day;
        record.day_start_ms = source.day_start_ms;
        record.day_end_ms = source.day_end_ms;
        record.session_offset = static_cast<uint32_t>(next_session);
        record.mask_window_offset = 0;
        record.file_offset = static_cast<uint32_t>(next_file);
        record.fallback_file_offset =
            static_cast<uint32_t>(next_fallback_file);
        record.timezone_offset_minutes = source.timezone_offset_minutes;
        record.timezone_offset_valid = source.timezone_offset_valid;
        if (source.has_edf) record.source_flags |= NIGHT_CATALOG_SOURCE_EDF;
        if (source.active_capture) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_ACTIVE_CAPTURE;
        }
        if (source.has_str) record.source_flags |= NIGHT_CATALOG_SOURCE_STR;
        if (source.has_fallback) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK;
        }
        if (source.local_history) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_LOCAL_HISTORY;
        }

        if (source.has_str) {
            (void)apply_metrics(record.metrics,
                                source.str_metrics,
                                NightCatalogMetricSource::Str,
                                false,
                                false);
        }
        if (source.has_summary) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK;
            if (source.summary_expired) {
                record.source_flags |= NIGHT_CATALOG_SOURCE_SUMMARY_EXPIRED;
            }
            record.summary_identity = source.summary_identity;
            if (source.summary_metrics_valid) {
                (void)apply_metrics(record.metrics,
                                    source.summary_metrics,
                                    NightCatalogMetricSource::Summary,
                                    false,
                                    true);
            }
        }

        NightCatalogTimeRange previous;
        bool have_previous = false;
        for (size_t i = first_owned_index(sessions, source.owner);
             i < sessions.size(); ++i) {
            const BuildSession &session = sessions.data()[i];
            if (session.owner != source.owner) break;
            if (!selected_session(source, session)) continue;
            if (have_previous && same_range(previous, session.range)) continue;

            catalog->sessions_[next_session++] = session.range;
            previous = session.range;
            have_previous = true;
            ++record.session_count;
        }

        for (size_t i = first_owned_index(files, source.owner);
             i < files.size(); ++i) {
            const BuildFile &source_file = files.data()[i];
            if (source_file.owner != source.owner) break;

            const size_t path_len = strlen(source_file.source.path);
            NightCatalogSourceFile &file = catalog->files_[next_file];
            file.kind = source_file.source.kind;
            file.path_offset = static_cast<uint32_t>(next_path);
            file.path_length = static_cast<uint16_t>(path_len);
            file.session_index = source_file.has_session
                ? find_session_index(*catalog,
                                     record,
                                     source_file.session_range)
                : NIGHT_CATALOG_NO_SESSION;
            if (source_file.has_session &&
                file.session_index == NIGHT_CATALOG_NO_SESSION) {
                return fail(NightCatalogBuildFailure::InvariantViolation,
                            "night_catalog_file_session_missing");
            }

            file.coverage_offset = static_cast<uint32_t>(next_file);
            file.coverage_count = 1;
            file.signal_layout_offset =
                static_cast<uint32_t>(next_signal_layout);
            file.signal_layout_count = static_cast<uint16_t>(
                source_file.source.signal_layout_count);
            file.file_size = source_file.source.file_size;
            file.last_write_ms = source_file.source.last_write_ms;
            file.data_offset = source_file.source.data_offset;
            file.data_size = source_file.source.data_size;
            file.identity = source_file.source.identity;
            file.record_start_ms = source_file.source.record_start_ms;
            file.header_size = source_file.source.header_size;
            file.record_size = source_file.source.record_size;
            file.record_duration_ms =
                source_file.source.record_duration_ms;
            file.complete_records = source_file.source.complete_records;
            catalog->coverage_[next_file] = source_file.source.coverage;
            for (size_t layout_index = 0;
                 layout_index < source_file.source.signal_layout_count;
                 ++layout_index) {
                catalog->signal_layouts_[next_signal_layout++] =
                    source_file.source.signal_layouts[layout_index];
            }

            memcpy(catalog->paths_ + next_path,
                   source_file.source.path,
                   path_len + 1);
            next_path += path_len + 1;
            ++next_file;
            ++record.file_count;
        }

        for (size_t i = first_owned_index(fallbacks, source.owner);
             i < fallbacks.size(); ++i) {
            const BuildFallback &source_fallback = fallbacks.data()[i];
            if (source_fallback.owner != source.owner) break;

            const size_t path_len = strlen(source_fallback.source.path);
            NightCatalogFallbackFile &file =
                catalog->fallback_files_[next_fallback_file];
            file.path_offset = static_cast<uint32_t>(next_path);
            file.path_length = static_cast<uint16_t>(path_len);
            file.section_offset =
                static_cast<uint32_t>(next_fallback_section);
            file.section_count = static_cast<uint16_t>(
                source_fallback.source.section_count);
            file.file_size = source_fallback.source.file_size;
            file.last_write_ms = source_fallback.source.last_write_ms;
            file.identity = source_fallback.source.identity;
            file.metadata_bytes = source_fallback.source.metadata_bytes;
            file.time_adjust_ms = source_fallback.time_adjust_ms;

            for (size_t section_index = 0;
                 section_index < source_fallback.source.section_count;
                 ++section_index) {
                const NightCatalogFallbackSectionInput &source_section =
                    source_fallback.source.sections[section_index];
                NightCatalogFallbackSection &section =
                    catalog->fallback_sections_[next_fallback_section++];
                section.kind = source_section.kind;
                section.source = source_section.source;
                section.signal = source_section.signal;
                section.event_mask = source_section.event_mask;
                section.payload_schema = source_section.payload_schema;
                section.record_count = source_section.record_count;
                section.sample_interval_ms =
                    source_section.sample_interval_ms;
                if (source_fallback.source.coordinates_are_resolved) {
                    section.coverage = source_section.coverage;
                } else if (!adjust_range(source_section.coverage,
                                         source_fallback.time_adjust_ms,
                                         section.coverage)) {
                    return fail(NightCatalogBuildFailure::InvariantViolation,
                                "night_catalog_fallback_adjustment_invalid");
                }
                section.data_offset = source_section.data_offset;
                section.data_size = source_section.data_size;
                section.data_crc32 = source_section.data_crc32;
            }

            memcpy(catalog->paths_ + next_path,
                   source_fallback.source.path,
                   path_len + 1);
            next_path += path_len + 1;
            ++next_fallback_file;
            ++record.fallback_file_count;
        }

        record.source_revision =
            SourceRevision(calculate_revision(*catalog, record));

        return true;
    }

    bool finish() {
        if (next_session != final_sessions || next_file != final_files ||
            next_signal_layout != final_signal_layouts ||
            next_fallback_file != final_fallback_files ||
            next_fallback_section != final_fallback_sections ||
            next_path != final_paths) {
            return fail(NightCatalogBuildFailure::InvariantViolation,
                        "night_catalog_output_mismatch");
        }

        phase = Phase::Done;
        return true;
    }

    NightCatalogBuildInput input;
    NightCatalogBuildStatus &status;
    Phase phase = Phase::CountEdf;
    size_t cursor = 0;
    IncrementalSort sort;

    size_t max_nights = 0;
    size_t max_sessions = 0;
    size_t max_files = 0;
    LargeScratchArray<BuildNight> nights;
    LargeScratchArray<BuildSession> sessions;
    LargeScratchArray<BuildFile> files;
    LargeScratchArray<BuildFallback> fallbacks;

    size_t final_sessions = 0;
    size_t final_files = 0;
    size_t final_signal_layouts = 0;
    size_t final_fallback_files = 0;
    size_t final_fallback_sections = 0;
    size_t final_paths = 0;

    std::shared_ptr<NightCatalog> catalog;
    size_t next_session = 0;
    size_t next_file = 0;
    size_t next_signal_layout = 0;
    size_t next_fallback_file = 0;
    size_t next_fallback_section = 0;
    size_t next_path = 0;
};

struct NightCatalogBuilder::ProjectionRuntime {
    enum class Phase { Count, Allocate, Copy, Done };

    ProjectionRuntime(const NightCatalog &value, Projection mode, SleepDayId day,
                      const NightCatalog *other, const SleepDayId *selected,
                      size_t selected_count, NightCatalogBuildStatus &result)
        : source(value), projection(mode), sleep_day(day), replacement(other),
          days(selected), day_count(selected_count), status(result),
          externalize(mode == Projection::Index) {}

    bool fail(NightCatalogBuildFailure failure) {
        status.failure = failure;
        status.detail = failure == NightCatalogBuildFailure::AllocationFailed
                            ? "night_catalog_projection_alloc_failed"
                            : "night_catalog_projection_invalid";
        phase = Phase::Done;
        catalog.reset();
        return false;
    }

    bool select_record(const NightCatalog *&from, const NightCatalogRecord *&record) {
        if (projection == Projection::Night) {
            if (source_index++ > 0) return false;
            from = &source;
            record = source.find(sleep_day);
            return record != nullptr;
        }

        const auto *base = source.record(source_index);
        if (day_index < day_count && (!base || !(days[day_index] < base->sleep_day))) {
            const SleepDayId day = days[day_index];
            if ((day_index > 0 && !(day < days[day_index - 1])) || !replacement ||
                !(record = replacement->find(day))) {
                return fail(NightCatalogBuildFailure::InvalidInput);
            }

            from = replacement;
            ++day_index;
            if (base && base->sleep_day == day) ++source_index;
            return true;
        }

        if (!base) return false;
        from = &source;
        record = base;
        ++source_index;
        return true;
    }

    bool count_record(const NightCatalog &catalog, const NightCatalogRecord &record) {
        if (!add_count(counts.records, 1) ||
            !add_count(counts.sessions, record.session_count) ||
            !add_count(counts.mask_windows, record.mask_window_count)) {
            return false;
        }

        if (record.sources_external &&
            (record.file_count != 0 || record.fallback_file_count != 0)) {
            return false;
        }
        if (externalize || record.sources_external) return true;

        if (!add_count(counts.files, record.file_count) ||
            !add_count(counts.fallback_files,
                       record.fallback_file_count)) {
            return false;
        }

        size_t file_count = 0;
        const NightCatalogSourceFile *files =
            catalog.files(record, file_count);
        if (file_count != record.file_count ||
            (file_count > 0 && !files)) {
            return false;
        }
        for (size_t i = 0; i < file_count; ++i) {
            if (!catalog.path(files[i]) ||
                !add_count(counts.coverage, files[i].coverage_count) ||
                !add_count(counts.signal_layouts,
                           files[i].signal_layout_count) ||
                !add_count(counts.paths,
                           static_cast<size_t>(files[i].path_length) + 1)) {
                return false;
            }
        }

        size_t fallback_count = 0;
        const NightCatalogFallbackFile *fallback_files =
            catalog.fallback_files(record, fallback_count);
        if (fallback_count != record.fallback_file_count ||
            (fallback_count > 0 && !fallback_files)) {
            return false;
        }
        for (size_t i = 0; i < fallback_count; ++i) {
            if (!catalog.path(fallback_files[i]) ||
                !add_count(counts.fallback_sections,
                           fallback_files[i].section_count) ||
                !add_count(
                    counts.paths,
                    static_cast<size_t>(fallback_files[i].path_length) + 1)) {
                return false;
            }
        }
        return true;
    }

    bool append_record(const NightCatalog &from, const NightCatalogRecord &old_record) {
        NightCatalogRecord &record = catalog->records_[next_record++];
        record = old_record;
        record.session_offset = static_cast<uint32_t>(next_session);
        record.mask_window_offset =
            static_cast<uint32_t>(next_mask_window);
        record.file_offset = static_cast<uint32_t>(next_file);
        record.fallback_file_offset =
            static_cast<uint32_t>(next_fallback_file);

        size_t session_count = 0;
        const NightCatalogTimeRange *sessions =
            from.sessions(old_record, session_count);
        if (session_count != old_record.session_count ||
            (session_count > 0 && !sessions)) {
            return false;
        }
        for (size_t i = 0; i < session_count; ++i) {
            catalog->sessions_[next_session++] = sessions[i];
        }

        size_t mask_window_count = 0;
        const NightCatalogTimeRange *mask_windows =
            from.mask_windows(old_record, mask_window_count);
        if (mask_window_count != old_record.mask_window_count ||
            (mask_window_count > 0 && !mask_windows)) {
            return false;
        }
        for (size_t i = 0; i < mask_window_count; ++i) {
            catalog->mask_windows_[next_mask_window++] = mask_windows[i];
        }

        if (externalize || old_record.sources_external) {
            record.sources_external = true;
            record.file_count = 0;
            record.fallback_file_count = 0;
            return true;
        }

        size_t file_count = 0;
        const NightCatalogSourceFile *files =
            from.files(old_record, file_count);
        if (file_count != old_record.file_count ||
            (file_count > 0 && !files)) {
            return false;
        }
        for (size_t i = 0; i < file_count; ++i) {
            const char *path = from.path(files[i]);
            size_t coverage_count = 0;
            const NightCatalogSourceCoverage *coverage =
                from.coverage(files[i], coverage_count);
            size_t signal_layout_count = 0;
            const EdfReportSignalLayout *signal_layouts =
                from.signal_layouts(files[i], signal_layout_count);
            if (!path || coverage_count != files[i].coverage_count ||
                signal_layout_count != files[i].signal_layout_count ||
                (coverage_count > 0 && !coverage) ||
                (signal_layout_count > 0 && !signal_layouts)) {
                return false;
            }

            NightCatalogSourceFile &file = catalog->files_[next_file++];
            file = files[i];
            file.path_offset = static_cast<uint32_t>(next_path);
            file.coverage_offset = static_cast<uint32_t>(next_coverage);
            file.signal_layout_offset =
                static_cast<uint32_t>(next_signal_layout);

            for (size_t j = 0; j < coverage_count; ++j) {
                catalog->coverage_[next_coverage++] = coverage[j];
            }
            for (size_t j = 0; j < signal_layout_count; ++j) {
                catalog->signal_layouts_[next_signal_layout++] =
                    signal_layouts[j];
            }

            const size_t path_bytes =
                static_cast<size_t>(file.path_length) + 1;
            memcpy(catalog->paths_ + next_path, path, path_bytes);
            next_path += path_bytes;
        }

        size_t fallback_count = 0;
        const NightCatalogFallbackFile *fallback_files =
            from.fallback_files(old_record, fallback_count);
        if (fallback_count != old_record.fallback_file_count ||
            (fallback_count > 0 && !fallback_files)) {
            return false;
        }
        for (size_t i = 0; i < fallback_count; ++i) {
            const char *path = from.path(fallback_files[i]);
            size_t section_count = 0;
            const NightCatalogFallbackSection *sections =
                from.fallback_sections(fallback_files[i], section_count);
            if (!path || section_count != fallback_files[i].section_count ||
                (section_count > 0 && !sections)) {
                return false;
            }

            NightCatalogFallbackFile &file =
                catalog->fallback_files_[next_fallback_file++];
            file = fallback_files[i];
            file.path_offset = static_cast<uint32_t>(next_path);
            file.section_offset =
                static_cast<uint32_t>(next_fallback_section);
            for (size_t j = 0; j < section_count; ++j) {
                catalog->fallback_sections_[next_fallback_section++] =
                    sections[j];
            }

            const size_t path_bytes =
                static_cast<size_t>(file.path_length) + 1;
            memcpy(catalog->paths_ + next_path, path, path_bytes);
            next_path += path_bytes;
        }
        return true;
    }

    bool poll() {
        const NightCatalog *from = nullptr;
        const NightCatalogRecord *record = nullptr;
        switch (phase) {
            case Phase::Count:
                if (select_record(from, record)) {
                    if (!count_record(*from, *record)) {
                        fail(NightCatalogBuildFailure::InvariantViolation);
                    }
                } else if (phase != Phase::Done) {
                    phase = Phase::Allocate;
                }
                break;

            case Phase::Allocate:
                catalog.reset(new (std::nothrow) NightCatalog());
                if (!catalog || !catalog->allocate(
                                    counts.records, counts.sessions,
                                    counts.mask_windows, counts.files, counts.coverage,
                                    counts.signal_layouts, counts.fallback_files,
                                    counts.fallback_sections, counts.paths)) {
                    fail(NightCatalogBuildFailure::AllocationFailed);
                } else {
                    source_index = day_index = 0;
                    phase = Phase::Copy;
                }
                break;

            case Phase::Copy:
                if (select_record(from, record)) {
                    if (!append_record(*from, *record)) {
                        fail(NightCatalogBuildFailure::InvariantViolation);
                    }
                } else if (phase != Phase::Done) {
                    finish();
                }
                break;

            case Phase::Done:
                return false;
        }
        return true;
    }

    bool finish() {
        if (next_record != counts.records || next_session != counts.sessions ||
            next_mask_window != counts.mask_windows || next_file != counts.files ||
            next_coverage != counts.coverage ||
            next_signal_layout != counts.signal_layouts ||
            next_fallback_file != counts.fallback_files ||
            next_fallback_section != counts.fallback_sections ||
            next_path != counts.paths) {
            return fail(NightCatalogBuildFailure::InvariantViolation);
        }

        phase = Phase::Done;
        return true;
    }

    struct CatalogCounts {
        size_t records = 0;
        size_t sessions = 0;
        size_t mask_windows = 0;
        size_t files = 0;
        size_t coverage = 0;
        size_t signal_layouts = 0;
        size_t fallback_files = 0;
        size_t fallback_sections = 0;
        size_t paths = 0;
    };

    const NightCatalog &source;
    Projection projection;
    SleepDayId sleep_day;
    const NightCatalog *replacement;
    const SleepDayId *days;
    size_t day_count;
    NightCatalogBuildStatus &status;
    bool externalize;
    Phase phase = Phase::Count;
    size_t source_index = 0;
    size_t day_index = 0;
    CatalogCounts counts;
    std::shared_ptr<NightCatalog> catalog;
    size_t next_record = 0;
    size_t next_session = 0;
    size_t next_mask_window = 0;
    size_t next_file = 0;
    size_t next_coverage = 0;
    size_t next_signal_layout = 0;
    size_t next_fallback_file = 0;
    size_t next_fallback_section = 0;
    size_t next_path = 0;
};

NightCatalogBuilder::~NightCatalogBuilder() {
    reset();
}

void NightCatalogBuilder::reset() {
    LargeObject::destroy(runtime_);
    runtime_ = nullptr;
    LargeObject::destroy(projection_);
    projection_ = nullptr;
    status_ = {};
}

bool NightCatalogBuilder::begin(const NightCatalogBuildInput &input) {
    reset();

    if ((input.edf_session_count > 0 && !input.edf_sessions) ||
        (input.str_record_count > 0 && !input.str_records) ||
        (input.summary_record_count > 0 && !input.summary_records) ||
        (input.fallback_record_count > 0 && !input.fallback_records)) {
        status_.failure = NightCatalogBuildFailure::InvalidInput;
        status_.detail = "night_catalog_input_invalid";
        return false;
    }

    runtime_ = LargeObject::create<Runtime>(input, status_);
    if (!runtime_) {
        status_.failure = NightCatalogBuildFailure::AllocationFailed;
        status_.detail = "night_catalog_scratch_alloc_failed";
        return false;
    }

    Runtime &r = *runtime_;
    r.max_sessions = input.edf_session_count;
    r.max_files = input.str_record_count;
    if (!add_count(r.max_nights, input.edf_session_count) ||
        !add_count(r.max_nights, input.str_record_count) ||
        !add_count(r.max_nights, input.summary_record_count) ||
        !add_count(r.max_nights, input.fallback_record_count)) {
        return r.fail(NightCatalogBuildFailure::InvalidInput,
                      "night_catalog_count_overflow");
    }
    return true;
}

bool NightCatalogBuilder::begin_projection(const NightCatalog &source,
                                           Projection projection, SleepDayId sleep_day,
                                           const NightCatalog *replacement,
                                           const SleepDayId *days, size_t day_count) {
    reset();
    if (day_count > 0 && (!days || !replacement)) {
        status_.failure = NightCatalogBuildFailure::InvalidInput;
        status_.detail = "night_catalog_projection_invalid";
        return false;
    }

    projection_ = LargeObject::create<ProjectionRuntime>(
        source, projection, sleep_day, replacement, days, day_count, status_);
    if (!projection_) {
        status_.failure = NightCatalogBuildFailure::AllocationFailed;
        status_.detail = "night_catalog_projection_alloc_failed";
    }
    return projection_ != nullptr;
}

bool NightCatalogBuilder::begin_merge(const NightCatalog &source,
                                      const NightCatalog &replacement,
                                      const SleepDayId *days, size_t day_count) {
    return begin_projection(source, Projection::Upsert, {}, &replacement, days,
                            day_count);
}

bool NightCatalogBuilder::active() const {
    return (runtime_ && runtime_->phase != Runtime::Phase::Done) ||
           (projection_ && projection_->phase != ProjectionRuntime::Phase::Done);
}

std::shared_ptr<const NightCatalog> NightCatalogBuilder::take_result() {
    if (active()) return {};
    if (runtime_) return std::move(runtime_->catalog);
    if (projection_) return std::move(projection_->catalog);
    return {};
}

bool NightCatalogBuilder::poll() {
    if (!active()) return false;
    if (projection_) return projection_->poll();

    Runtime &r = *runtime_;
    const NightCatalogBuildInput &input = r.input;
    using Phase = Runtime::Phase;
    auto count = [&](size_t &total, size_t value) {
        if (add_count(total, value)) return true;
        return r.fail(NightCatalogBuildFailure::InvalidInput,
                      "night_catalog_count_overflow");
    };
    auto allocated = [&](bool ok, Phase next) {
        if (ok) {
            r.advance(next);
        } else {
            r.fail(NightCatalogBuildFailure::AllocationFailed,
                   "night_catalog_scratch_alloc_failed");
        }
    };
    auto ingested = [&](bool ok, const char *error) {
        if (ok) {
            ++r.cursor;
        } else {
            r.fail(NightCatalogBuildFailure::InvalidInput, error);
        }
    };

    switch (r.phase) {
        case Phase::CountEdf:
            if (r.cursor < input.edf_session_count) {
                count(r.max_files, input.edf_sessions[r.cursor++].file_count);
            } else {
                r.advance(Phase::CountSummary);
            }
            break;

        case Phase::CountSummary:
            if (r.cursor < input.summary_record_count) {
                count(r.max_sessions, input.summary_records[r.cursor++].session_count);
            } else {
                r.advance(Phase::CountFallback);
            }
            break;

        case Phase::CountFallback:
            if (r.cursor < input.fallback_record_count) {
                count(r.max_sessions, input.fallback_records[r.cursor++].session_count);
            } else {
                r.advance(Phase::AllocateNights);
            }
            break;

        case Phase::AllocateNights:
            allocated(r.nights.allocate(r.max_nights), Phase::AllocateSessions);
            break;

        case Phase::AllocateSessions:
            allocated(r.sessions.allocate(r.max_sessions), Phase::AllocateFiles);
            break;

        case Phase::AllocateFiles:
            allocated(r.files.allocate(r.max_files), Phase::AllocateFallbacks);
            break;

        case Phase::AllocateFallbacks:
            allocated(r.fallbacks.allocate(input.fallback_record_count), Phase::Edf);
            break;

        case Phase::Edf:
            if (r.cursor < input.edf_session_count) {
                ingested(ingest_edf(input, r.nights, r.sessions, r.files, r.cursor),
                         "night_catalog_edf_input_invalid");
            } else {
                r.advance(Phase::Str);
            }
            break;

        case Phase::Str:
            if (r.cursor < input.str_record_count) {
                ingested(ingest_str(input, r.nights, r.files, r.cursor),
                         "night_catalog_str_input_invalid");
            } else {
                r.advance(Phase::Summary);
            }
            break;

        case Phase::Summary:
            if (r.cursor < input.summary_record_count) {
                ingested(ingest_summary(input, r.nights, r.sessions, r.cursor),
                         "night_catalog_summary_input_invalid");
            } else {
                r.advance(Phase::Fallback);
            }
            break;

        case Phase::Fallback:
            if (r.cursor < input.fallback_record_count) {
                ingested(ingest_fallback(input, r.nights, r.sessions, r.fallbacks,
                                         status_.invalid_fallback_records, r.cursor),
                         "night_catalog_fallback_input_invalid");
            } else {
                r.advance(Phase::SortNights);
            }
            break;

        case Phase::SortNights:
            if (r.sort.poll(r.nights.data(), r.nights.size(),
                            [](const BuildNight &lhs, const BuildNight &rhs) {
                                return rhs.sleep_day < lhs.sleep_day;
                            })) {
                r.advance(Phase::SortSessions);
            }
            break;

        case Phase::SortSessions:
            if (r.sort.poll(r.sessions.data(), r.sessions.size(),
                            [](const BuildSession &lhs, const BuildSession &rhs) {
                                if (lhs.owner != rhs.owner) {
                                    return lhs.owner < rhs.owner;
                                }
                                if (lhs.range.start_ms != rhs.range.start_ms) {
                                    return lhs.range.start_ms < rhs.range.start_ms;
                                }
                                if (lhs.range.end_ms != rhs.range.end_ms) {
                                    return lhs.range.end_ms < rhs.range.end_ms;
                                }
                                return static_cast<uint8_t>(lhs.origin) <
                                       static_cast<uint8_t>(rhs.origin);
                            })) {
                r.advance(Phase::SortFiles);
            }
            break;

        case Phase::SortFiles:
            if (r.sort.poll(r.files.data(), r.files.size(),
                            [](const BuildFile &lhs, const BuildFile &rhs) {
                                if (lhs.owner != rhs.owner) {
                                    return lhs.owner < rhs.owner;
                                }
                                if (lhs.source.coverage.range.start_ms !=
                                    rhs.source.coverage.range.start_ms) {
                                    return lhs.source.coverage.range.start_ms <
                                           rhs.source.coverage.range.start_ms;
                                }
                                if (lhs.source.kind != rhs.source.kind) {
                                    return static_cast<uint8_t>(lhs.source.kind) <
                                           static_cast<uint8_t>(rhs.source.kind);
                                }
                                return strcmp(lhs.source.path, rhs.source.path) < 0;
                            })) {
                r.advance(Phase::SortFallbacks);
            }
            break;

        case Phase::SortFallbacks:
            if (r.sort.poll(r.fallbacks.data(), r.fallbacks.size(),
                            [](const BuildFallback &lhs, const BuildFallback &rhs) {
                                return lhs.owner < rhs.owner;
                            })) {
                r.advance(Phase::CountOutput);
            }
            break;

        case Phase::CountOutput:
            if (r.cursor < r.nights.size()) {
                if (r.count_night()) ++r.cursor;
            } else {
                r.advance(Phase::AllocateOutput);
            }
            break;

        case Phase::AllocateOutput:
            if (r.allocate_output()) r.advance(Phase::WriteOutput);
            break;

        case Phase::WriteOutput:
            if (r.cursor < r.nights.size()) {
                if (r.write_night()) ++r.cursor;
            } else {
                r.advance(Phase::Finish);
            }
            break;

        case Phase::Finish:
            r.finish();
            break;

        case Phase::Done:
            return false;
    }
    return true;
}

std::shared_ptr<const NightCatalog>
NightCatalogBuilder::build(const NightCatalogBuildInput &input,
                           NightCatalogBuildStatus *status) {
    NightCatalogBuilder builder;
    if (builder.begin(input)) {
        while (builder.active())
            builder.poll();
    }
    if (status) *status = builder.status();
    return builder.take_result();
}

std::shared_ptr<const NightCatalog>
NightCatalogBuilder::index(const NightCatalog &source) {
    return project(source, Projection::Index, SleepDayId());
}

std::shared_ptr<const NightCatalog>
NightCatalogBuilder::select_night(const NightCatalog &source, SleepDayId sleep_day) {
    if (!source.find(sleep_day)) return {};

    return project(source, Projection::Night, sleep_day);
}

std::shared_ptr<const NightCatalog> NightCatalogBuilder::upsert_night(
    const NightCatalog &source, const NightCatalog &replacement, SleepDayId sleep_day) {
    if (!replacement.find(sleep_day)) return {};

    return project(source, Projection::Upsert, sleep_day, &replacement);
}

std::shared_ptr<const NightCatalog>
NightCatalogBuilder::project(const NightCatalog &source, Projection projection,
                             SleepDayId sleep_day, const NightCatalog *replacement) {
    NightCatalogBuilder builder;
    if (builder.begin_projection(source, projection, sleep_day, replacement,
                                 replacement ? &sleep_day : nullptr,
                                 replacement ? 1 : 0)) {
        while (builder.active())
            builder.poll();
    }
    return builder.take_result();
}

std::shared_ptr<const NightCatalog> NightCatalogBuilder::replace_fallback(
    const NightCatalog &source,
    const char *path,
    const std::shared_ptr<const LargeByteBuffer> &artifact,
    int64_t last_write_ms) {
    if (!path || !path[0] || !artifact || artifact->size() == 0) return {};

    ReportFallbackArtifactView replacement;
    if (!ReportFallbackArtifactCodec::decode_metadata(
            artifact->data(), artifact->size(), replacement) ||
        replacement.info.total_bytes != artifact->size()) {
        return {};
    }

    const auto *night = source.find(replacement.info.sleep_day);
    if (!night || night->sources_external) return {};

    return restore_fallback(source, path, replacement, last_write_ms);
}

std::shared_ptr<const NightCatalog> NightCatalogBuilder::restore_fallback(
    const NightCatalog &source,
    const char *path,
    const ReportFallbackArtifactView &replacement,
    int64_t last_write_ms) {
    if (!path || !path[0]) return {};

    const size_t path_length = strlen(path);
    if (path_length > UINT16_MAX) return {};

    const NightCatalogRecord *source_night =
        source.find(replacement.info.sleep_day);
    if (!source_night ||
        (source_night->source_flags & NIGHT_CATALOG_SOURCE_EDF) != 0) return {};

    size_t removed_file_count = 0;
    const NightCatalogFallbackFile *removed_files =
        source.fallback_files(*source_night, removed_file_count);
    if (removed_file_count > 0 && !removed_files) return {};

    int32_t adjustment = 0;
    for (size_t i = 0; i < removed_file_count; ++i) {
        const auto &file = removed_files[i];
        const char *old_path = source.path(file);
        if (old_path && strcmp(old_path, path) == 0 &&
            file.identity == replacement.info.content_identity) {
            adjustment = file.time_adjust_ms;
            break;
        }
    }

    int64_t day_start = 0;
    int64_t day_end = 0;
    if (!adjust_time(replacement.info.day_start_ms, adjustment, day_start) ||
        !adjust_time(replacement.info.day_end_ms, adjustment, day_end) ||
        source_night->day_start_ms != day_start ||
        source_night->day_end_ms != day_end ||
        (source_night->timezone_offset_valid &&
         (!replacement.info.timezone_offset_valid ||
          static_cast<int64_t>(source_night->timezone_offset_minutes) * 60000 !=
              static_cast<int64_t>(replacement.info.timezone_offset_minutes) *
                  60000 - adjustment))) {
        return {};
    }

    size_t source_session_count = 0;
    const NightCatalogTimeRange *source_sessions =
        source.sessions(*source_night, source_session_count);
    if (!source_sessions) return {};

    for (size_t i = 0; i < replacement.info.session_count; ++i) {
        NightCatalogTimeRange replacement_session;
        if (!replacement.session(i, replacement_session) ||
            !adjust_range(replacement_session, adjustment, replacement_session)) {
            return {};
        }

        bool retained = false;
        for (size_t j = 0; j < source_session_count; ++j) {
            if (source_sessions[j].start_ms <= replacement_session.start_ms &&
                source_sessions[j].end_ms >= replacement_session.end_ms) {
                retained = true;
                break;
            }
        }
        if (!retained) return {};
    }

    size_t removed_section_count = 0;
    size_t removed_path_bytes = 0;
    for (size_t i = 0; i < removed_file_count; ++i) {
        const NightCatalogFallbackFile &file = removed_files[i];
        if (!source.path(file) ||
            !add_count(removed_section_count, file.section_count) ||
            !add_count(removed_path_bytes,
                       static_cast<size_t>(file.path_length) + 1)) {
            return {};
        }
    }

    if (removed_file_count > source.fallback_file_count_ ||
        removed_section_count > source.fallback_section_count_ ||
        removed_path_bytes > source.path_bytes_) {
        return {};
    }

    size_t fallback_file_count =
        source.fallback_file_count_ - removed_file_count;
    size_t fallback_section_count =
        source.fallback_section_count_ - removed_section_count;
    size_t path_bytes = source.path_bytes_ - removed_path_bytes;
    if (!add_count(fallback_file_count, 1) ||
        !add_count(fallback_section_count,
                   replacement.info.section_count) ||
        !add_count(path_bytes, path_length + 1)) {
        return {};
    }

    std::shared_ptr<NightCatalog> catalog(new (std::nothrow) NightCatalog());
    if (!catalog ||
        !catalog->allocate(source.record_count_,
                           source.session_count_,
                           source.mask_window_count_,
                           source.file_count_,
                           source.coverage_count_,
                           source.signal_layout_count_,
                           fallback_file_count,
                           fallback_section_count,
                           path_bytes)) {
        return {};
    }

    if (source.session_count_ > 0) {
        memcpy(catalog->sessions_,
               source.sessions_,
               source.session_count_ * sizeof(*source.sessions_));
    }
    if (source.mask_window_count_ > 0) {
        memcpy(catalog->mask_windows_,
               source.mask_windows_,
               source.mask_window_count_ * sizeof(*source.mask_windows_));
    }
    if (source.coverage_count_ > 0) {
        memcpy(catalog->coverage_,
               source.coverage_,
               source.coverage_count_ * sizeof(*source.coverage_));
    }
    if (source.signal_layout_count_ > 0) {
        memcpy(catalog->signal_layouts_,
               source.signal_layouts_,
               source.signal_layout_count_ * sizeof(*source.signal_layouts_));
    }

    size_t next_file = 0;
    size_t next_fallback_file = 0;
    size_t next_fallback_section = 0;
    size_t next_path = 0;
    for (size_t night_index = 0;
         night_index < source.record_count_;
         ++night_index) {
        const NightCatalogRecord &old_record = source.records_[night_index];
        NightCatalogRecord &record = catalog->records_[night_index];
        record = old_record;
        record.file_offset = static_cast<uint32_t>(next_file);
        record.fallback_file_offset =
            static_cast<uint32_t>(next_fallback_file);

        size_t file_count = 0;
        const NightCatalogSourceFile *files =
            source.files(old_record, file_count);
        if (file_count > 0 && !files) return {};
        for (size_t i = 0; i < file_count; ++i) {
            const char *source_path = source.path(files[i]);
            if (!source_path) return {};

            NightCatalogSourceFile &file = catalog->files_[next_file++];
            file = files[i];
            file.path_offset = static_cast<uint32_t>(next_path);
            memcpy(catalog->paths_ + next_path,
                   source_path,
                   static_cast<size_t>(file.path_length) + 1);
            next_path += static_cast<size_t>(file.path_length) + 1;
        }

        if (old_record.sleep_day == replacement.info.sleep_day) {
            record.sources_external = false;
            record.source_flags |= NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK;
            record.fallback_file_count = 1;
            if (!record.timezone_offset_valid &&
                replacement.info.timezone_offset_valid && adjustment == 0) {
                record.timezone_offset_minutes =
                    replacement.info.timezone_offset_minutes;
                record.timezone_offset_valid = true;
            }

            NightCatalogFallbackFile &file =
                catalog->fallback_files_[next_fallback_file++];
            file.path_offset = static_cast<uint32_t>(next_path);
            file.path_length = static_cast<uint16_t>(path_length);
            file.section_offset =
                static_cast<uint32_t>(next_fallback_section);
            file.section_count = static_cast<uint16_t>(
                replacement.info.section_count);
            file.file_size = replacement.info.total_bytes;
            file.last_write_ms = last_write_ms;
            file.identity = replacement.info.content_identity;
            file.metadata_bytes = static_cast<uint32_t>(
                replacement.info.metadata_bytes);
            file.time_adjust_ms = adjustment;

            for (size_t i = 0;
                 i < replacement.info.section_count;
                 ++i) {
                ReportFallbackSection replacement_section;
                if (!replacement.section(i, replacement_section)) return {};

                NightCatalogFallbackSection &section =
                    catalog->fallback_sections_[next_fallback_section++];
                section.kind = replacement_section.kind;
                section.source = replacement_section.source;
                section.signal = replacement_section.signal;
                section.event_mask = replacement_section.event_mask;
                section.payload_schema = replacement_section.payload_schema;
                section.record_count = replacement_section.record_count;
                section.sample_interval_ms =
                    replacement_section.sample_interval_ms;
                if (!adjust_range(replacement_section.coverage, adjustment,
                                  section.coverage)) return {};
                section.data_offset = replacement_section.data_offset;
                section.data_size = replacement_section.data_size;
                section.data_crc32 = replacement_section.data_crc32;
            }

            memcpy(catalog->paths_ + next_path, path, path_length + 1);
            next_path += path_length + 1;
        } else {
            size_t fallback_count = 0;
            const NightCatalogFallbackFile *fallbacks =
                source.fallback_files(old_record, fallback_count);
            if (fallback_count > 0 && !fallbacks) return {};
            record.fallback_file_count = static_cast<uint16_t>(fallback_count);

            for (size_t i = 0; i < fallback_count; ++i) {
                const NightCatalogFallbackFile &old_file = fallbacks[i];
                const char *source_path = source.path(old_file);
                size_t section_count = 0;
                const NightCatalogFallbackSection *sections =
                    source.fallback_sections(old_file, section_count);
                if (!source_path || (section_count > 0 && !sections)) return {};

                NightCatalogFallbackFile &file =
                    catalog->fallback_files_[next_fallback_file++];
                file = old_file;
                file.path_offset = static_cast<uint32_t>(next_path);
                file.section_offset =
                    static_cast<uint32_t>(next_fallback_section);
                for (size_t section_index = 0;
                     section_index < section_count;
                     ++section_index) {
                    catalog->fallback_sections_[next_fallback_section++] =
                        sections[section_index];
                }

                memcpy(catalog->paths_ + next_path,
                       source_path,
                       static_cast<size_t>(file.path_length) + 1);
                next_path += static_cast<size_t>(file.path_length) + 1;
            }
        }

        if (record.sleep_day == replacement.info.sleep_day) {
            record.source_revision =
                SourceRevision(calculate_revision(*catalog, record));
        }
    }

    if (next_file != source.file_count_ ||
        next_fallback_file != fallback_file_count ||
        next_fallback_section != fallback_section_count ||
        next_path != path_bytes) {
        return {};
    }
    return catalog;
}

}  // namespace aircannect
