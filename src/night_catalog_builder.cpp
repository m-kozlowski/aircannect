#include "night_catalog_builder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "report_records.h"
#include "report_fallback_artifact.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

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
    size_t owner = 0;
    bool boundary_set = false;
    bool has_edf = false;
    bool has_str = false;
    bool has_summary = false;
    bool has_fallback = false;
    bool summary_metrics_valid = false;
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
};

void *allocate_scratch(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes, false);
#else
    return calloc(1, bytes);
#endif
}

void free_scratch(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

template <typename T>
class ScratchArray {
public:
    ~ScratchArray() {
        for (size_t i = 0; i < capacity_; ++i) values_[i].~T();
        free_scratch(values_);
    }

    bool allocate(size_t capacity) {
        if (capacity == 0) return true;
        if (capacity > std::numeric_limits<size_t>::max() / sizeof(T)) {
            return false;
        }

        values_ = static_cast<T *>(
            allocate_scratch(capacity * sizeof(T)));
        if (!values_) return false;
        capacity_ = capacity;
        for (size_t i = 0; i < capacity; ++i) new (&values_[i]) T();
        return true;
    }

    T *append() {
        if (size_ >= capacity_) return nullptr;
        return &values_[size_++];
    }

    T *data() { return values_; }
    const T *data() const { return values_; }
    size_t size() const { return size_; }

private:
    T *values_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

bool add_count(size_t &total, size_t amount) {
    if (total > std::numeric_limits<size_t>::max() - amount) return false;
    total += amount;
    return true;
}

bool valid_boundary(int64_t start_ms, int64_t end_ms) {
    return start_ms > 0 && end_ms > start_ms;
}

bool same_range(const NightCatalogTimeRange &lhs,
                const NightCatalogTimeRange &rhs) {
    return lhs.start_ms == rhs.start_ms && lhs.end_ms == rhs.end_ms;
}

uint16_t metric_bit(NightCatalogMetric metric) {
    return static_cast<uint16_t>(1u << static_cast<uint8_t>(metric));
}

void set_metric(NightCatalogMetrics &out,
                NightCatalogMetric metric,
                float value,
                NightCatalogMetricSource source,
                bool fill_only,
                bool &used) {
    const uint16_t bit = metric_bit(metric);
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
        case NightCatalogMetric::DurationMinutes:
        case NightCatalogMetric::Count:
            return;
    }

    out.valid_mask |= bit;
    if (source == NightCatalogMetricSource::Str) {
        out.str_mask |= bit;
        out.summary_mask &= static_cast<uint16_t>(~bit);
    } else if (source == NightCatalogMetricSource::Summary) {
        out.summary_mask |= bit;
        out.str_mask &= static_cast<uint16_t>(~bit);
    }
    used = true;
}

void set_duration_metric(NightCatalogMetrics &out,
                         uint32_t value,
                         NightCatalogMetricSource source,
                         bool fill_only,
                         bool &used) {
    const uint16_t bit = metric_bit(NightCatalogMetric::DurationMinutes);
    if (fill_only && (out.valid_mask & bit) != 0) return;

    out.duration_min = value;
    out.valid_mask |= bit;
    if (source == NightCatalogMetricSource::Str) {
        out.str_mask |= bit;
        out.summary_mask &= static_cast<uint16_t>(~bit);
    } else if (source == NightCatalogMetricSource::Summary) {
        out.summary_mask |= bit;
        out.str_mask &= static_cast<uint16_t>(~bit);
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
    if (include_duration && input.has_duration_min) {
        set_duration_metric(out, input.duration_min,
                            source, fill_only, used);
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
                static_cast<uint8_t>(ReportSourceId::Leak0p5Hz) ||
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

BuildNight *find_or_add_night(ScratchArray<BuildNight> &nights,
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

BuildNight *find_night(ScratchArray<BuildNight> &nights,
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

bool append_session(ScratchArray<BuildSession> &sessions,
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

bool append_file(ScratchArray<BuildFile> &files,
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

bool fallback_section_valid(
    const NightCatalogFallbackSectionInput &section,
    int64_t day_start_ms,
    int64_t day_end_ms,
    uint64_t file_size,
    uint32_t metadata_bytes) {
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
               section.sample_interval_ms > 0 &&
               section.data_size > 0 &&
               section.payload_schema ==
                   REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    }
    if (section.kind == ReportFallbackSectionKind::Events) {
        const size_t record_bytes = report_event_record_wire_size();
        if (section.record_count > SIZE_MAX / record_bytes) return false;

        return section.source == ReportSourceId::RespiratoryEvents &&
               section.signal == ReportSignalId::Count &&
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

bool fallback_payload_valid(const NightCatalogFallbackInput &source) {
    size_t payload_bytes = 0;
    for (size_t i = 0; i < source.section_count; ++i) {
        const NightCatalogFallbackSectionInput &section = source.sections[i];
        if (!fallback_section_valid(section,
                                    source.day_start_ms,
                                    source.day_end_ms,
                                    source.file_size,
                                    source.metadata_bytes) ||
            !add_count(payload_bytes, section.data_size)) {
            return false;
        }

        if (section.data_size == 0) continue;
        const uint64_t section_end =
            section.data_offset + section.data_size;
        for (size_t previous_index = 0;
             previous_index < i;
             ++previous_index) {
            const NightCatalogFallbackSectionInput &previous =
                source.sections[previous_index];
            if (previous.data_size == 0) continue;

            const uint64_t previous_end =
                previous.data_offset + previous.data_size;
            if (section.data_offset < previous_end &&
                previous.data_offset < section_end) {
                return false;
            }
        }
    }

    return payload_bytes <= source.file_size - source.metadata_bytes &&
           source.metadata_bytes + payload_bytes == source.file_size;
}

bool ingest_fallback(const NightCatalogBuildInput &input,
                     ScratchArray<BuildNight> &nights,
                     ScratchArray<BuildSession> &sessions,
                     ScratchArray<BuildFallback> &fallbacks) {
    for (size_t i = 0; i < input.fallback_record_count; ++i) {
        const NightCatalogFallbackInput &source =
            input.fallback_records[i];
        if (!source.sleep_day.valid()) return false;

        BuildNight *night = find_night(nights, source.sleep_day);
        if (night && night->has_edf) continue;

        if (!source.path || !source.path[0] ||
            source.identity == 0 || source.metadata_bytes == 0 ||
            source.metadata_bytes > source.file_size ||
            source.session_count == 0 || !source.sessions ||
            source.section_count == 0 || !source.sections ||
            source.section_count > UINT16_MAX) {
            return false;
        }

        size_t unmatched_session_count = 0;
        NightCatalogTimeRange previous_session;
        for (size_t session_index = 0;
             session_index < source.session_count;
             ++session_index) {
            const NightCatalogTimeRange &session =
                source.sessions[session_index];
            if (session.start_ms < source.day_start_ms ||
                session.end_ms > source.day_end_ms ||
                (session_index > 0 &&
                 session.start_ms < previous_session.end_ms)) {
                return false;
            }
            if (!summary_session_matches_raw_edf(input,
                                                 source.sleep_day,
                                                 session)) {
                ++unmatched_session_count;
            }
            previous_session = session;
        }

        if (!fallback_payload_valid(source)) return false;

        if (unmatched_session_count == 0) continue;
        if (!night) night = find_or_add_night(nights, source.sleep_day);
        if (!night || night->has_fallback ||
            !set_primary_boundary(*night,
                                  source.day_start_ms,
                                  source.day_end_ms)) {
            return false;
        }

        for (size_t session_index = 0;
             session_index < source.session_count;
             ++session_index) {
            const NightCatalogTimeRange &session =
                source.sessions[session_index];
            if (summary_session_matches_raw_edf(input,
                                                source.sleep_day,
                                                session)) {
                continue;
            }
            if (!append_session(sessions,
                                night->owner,
                                SessionOrigin::Fallback,
                                session)) {
                return false;
            }
        }

        BuildFallback *fallback = fallbacks.append();
        if (!fallback) return false;
        fallback->owner = night->owner;
        fallback->source = source;
        night->has_fallback = true;
    }
    return true;
}

bool ingest_edf(const NightCatalogBuildInput &input,
                ScratchArray<BuildNight> &nights,
                ScratchArray<BuildSession> &sessions,
                ScratchArray<BuildFile> &files) {
    for (size_t i = 0; i < input.edf_session_count; ++i) {
        const NightCatalogEdfSessionInput &source = input.edf_sessions[i];
        if (!source.display_window.valid() || !source.files ||
            source.file_count == 0) {
            return false;
        }

        BuildNight *night = find_or_add_night(nights, source.sleep_day);
        if (!night || !set_primary_boundary(*night,
                                            source.day_start_ms,
                                            source.day_end_ms) ||
            !append_session(sessions,
                            night->owner,
                            SessionOrigin::Edf,
                            source.display_window)) {
            return false;
        }

        night->has_edf = true;
        for (size_t file_index = 0;
             file_index < source.file_count;
             ++file_index) {
            if (!append_file(files,
                             night->owner,
                             source.files[file_index],
                             &source.display_window)) {
                return false;
            }
        }
    }
    return true;
}

bool ingest_str(const NightCatalogBuildInput &input,
                ScratchArray<BuildNight> &nights,
                ScratchArray<BuildFile> &files) {
    for (size_t i = 0; i < input.str_record_count; ++i) {
        const NightCatalogStrInput &source = input.str_records[i];
        if (!source.record.sleep_day.valid() || !source.path ||
            !source.path[0] || source.record_size == 0 ||
            source.record.source_identity == 0) {
            return false;
        }

        BuildNight *night = find_night(nights, source.record.sleep_day);
        if (!night || !night->has_edf) continue;
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
    }
    return true;
}

bool ingest_summary(const NightCatalogBuildInput &input,
                    ScratchArray<BuildNight> &nights,
                    ScratchArray<BuildSession> &sessions) {
    for (size_t i = 0; i < input.summary_record_count; ++i) {
        const NightCatalogSummaryInput &source = input.summary_records[i];
        if (!source.sleep_day.valid() || source.identity == 0 ||
            !valid_boundary(source.day_start_ms, source.day_end_ms) ||
            (source.session_count > 0 && !source.sessions)) {
            return false;
        }

        size_t matched_session_count = 0;
        NightCatalogTimeRange previous_session;
        for (size_t session_index = 0;
             session_index < source.session_count;
             ++session_index) {
            const NightCatalogTimeRange &session =
                source.sessions[session_index];
            if (session.start_ms < source.day_start_ms ||
                session.end_ms > source.day_end_ms ||
                (session_index > 0 &&
                 session.start_ms < previous_session.end_ms)) {
                return false;
            }
            if (summary_session_matches_raw_edf(input,
                                                source.sleep_day,
                                                session)) {
                ++matched_session_count;
            }
            previous_session = session;
        }

        BuildNight *night = find_night(nights, source.sleep_day);
        if ((night && night->has_edf) ||
            (source.session_count > 0 &&
             matched_session_count == source.session_count)) {
            continue;
        }
        if (!night) night = find_or_add_night(nights, source.sleep_day);
        if (!night || night->has_summary ||
            !set_primary_boundary(*night,
                                  source.day_start_ms,
                                  source.day_end_ms)) {
            return false;
        }

        for (size_t session_index = 0;
             session_index < source.session_count;
             ++session_index) {
            if (summary_session_matches_raw_edf(input,
                                                source.sleep_day,
                                                source.sessions[session_index])) {
                continue;
            }
            if (!append_session(sessions,
                                night->owner,
                                SessionOrigin::Summary,
                                source.sessions[session_index])) {
                return false;
            }
        }

        night->summary_metrics = source.metrics;
        night->summary_identity = source.identity;
        night->has_summary = true;
        night->summary_metrics_valid = matched_session_count == 0;
    }
    return true;
}

bool selected_session(const BuildNight &night,
                      const BuildSession &session) {
    const SessionOrigin selected = night.has_edf
        ? SessionOrigin::Edf
        : (night.has_fallback ? SessionOrigin::Fallback
                              : SessionOrigin::Summary);
    return session.owner == night.owner && session.origin == selected;
}

size_t count_unique_sessions(const BuildNight &night,
                             const ScratchArray<BuildSession> &sessions) {
    size_t count = 0;
    NightCatalogTimeRange previous;
    bool have_previous = false;
    for (size_t i = 0; i < sessions.size(); ++i) {
        const BuildSession &session = sessions.data()[i];
        if (!selected_session(night, session)) continue;
        if (have_previous && same_range(previous, session.range)) continue;
        previous = session.range;
        have_previous = true;
        ++count;
    }
    return count;
}

size_t count_files(const BuildNight &night,
                   const ScratchArray<BuildFile> &files,
                   size_t &path_bytes) {
    size_t count = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const BuildFile &file = files.data()[i];
        if (file.owner != night.owner) continue;

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
                            const ScratchArray<BuildFallback> &fallbacks,
                            size_t &section_count,
                            size_t &path_bytes) {
    size_t count = 0;
    for (size_t i = 0; i < fallbacks.size(); ++i) {
        const BuildFallback &fallback = fallbacks.data()[i];
        if (fallback.owner != night.owner) continue;

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
    hash = hash_u32(hash,
                    static_cast<uint32_t>(record.sleep_day.epoch_days()));
    hash = hash_i64(hash, record.day_start_ms);
    hash = hash_i64(hash, record.day_end_ms);
    hash = hash_byte(hash, record.source_flags);
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

std::shared_ptr<const NightCatalog> build_failed(
    NightCatalogBuildStatus *status,
    NightCatalogBuildFailure failure,
    const char *detail) {
    if (status) {
        status->failure = failure;
        status->detail = detail;
    }
    return {};
}

}  // namespace

std::shared_ptr<const NightCatalog> NightCatalogBuilder::build(
    const NightCatalogBuildInput &input,
    NightCatalogBuildStatus *status) {
    if (status) *status = {};

    if ((input.edf_session_count > 0 && !input.edf_sessions) ||
        (input.str_record_count > 0 && !input.str_records) ||
        (input.summary_record_count > 0 && !input.summary_records) ||
        (input.fallback_record_count > 0 && !input.fallback_records)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_input_invalid");
    }

    size_t max_nights = 0;
    size_t max_sessions = 0;
    size_t max_files = 0;
    size_t max_fallbacks = 0;
    if (!add_count(max_nights, input.edf_session_count) ||
        !add_count(max_nights, input.str_record_count) ||
        !add_count(max_nights, input.summary_record_count) ||
        !add_count(max_nights, input.fallback_record_count) ||
        !add_count(max_sessions, input.edf_session_count) ||
        !add_count(max_files, input.str_record_count) ||
        !add_count(max_fallbacks, input.fallback_record_count)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_count_overflow");
    }

    for (size_t i = 0; i < input.edf_session_count; ++i) {
        if (!add_count(max_files, input.edf_sessions[i].file_count)) {
            return build_failed(status,
                                NightCatalogBuildFailure::InvalidInput,
                                "night_catalog_count_overflow");
        }
    }
    for (size_t i = 0; i < input.summary_record_count; ++i) {
        if (!add_count(max_sessions,
                       input.summary_records[i].session_count)) {
            return build_failed(status,
                                NightCatalogBuildFailure::InvalidInput,
                                "night_catalog_count_overflow");
        }
    }
    for (size_t i = 0; i < input.fallback_record_count; ++i) {
        if (!add_count(max_sessions,
                       input.fallback_records[i].session_count)) {
            return build_failed(status,
                                NightCatalogBuildFailure::InvalidInput,
                                "night_catalog_count_overflow");
        }
    }

    ScratchArray<BuildNight> nights;
    ScratchArray<BuildSession> sessions;
    ScratchArray<BuildFile> files;
    ScratchArray<BuildFallback> fallbacks;
    if (!nights.allocate(max_nights) ||
        !sessions.allocate(max_sessions) ||
        !files.allocate(max_files) ||
        !fallbacks.allocate(max_fallbacks)) {
        return build_failed(status,
                            NightCatalogBuildFailure::AllocationFailed,
                            "night_catalog_scratch_alloc_failed");
    }
    if (!ingest_edf(input, nights, sessions, files)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_edf_input_invalid");
    }
    if (!ingest_str(input, nights, files)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_str_input_invalid");
    }
    if (!ingest_summary(input, nights, sessions)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_summary_input_invalid");
    }
    if (!ingest_fallback(input, nights, sessions, fallbacks)) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvalidInput,
                            "night_catalog_fallback_input_invalid");
    }

    if (nights.size() > 1) {
        std::sort(nights.data(),
                  nights.data() + nights.size(),
                  [](const BuildNight &lhs, const BuildNight &rhs) {
                      return rhs.sleep_day < lhs.sleep_day;
                  });
    }
    if (sessions.size() > 1) {
        std::sort(sessions.data(),
                  sessions.data() + sessions.size(),
                  [](const BuildSession &lhs, const BuildSession &rhs) {
                      if (lhs.owner != rhs.owner) {
                          return lhs.owner < rhs.owner;
                      }
                      if (lhs.origin != rhs.origin) {
                          return static_cast<uint8_t>(lhs.origin) <
                                 static_cast<uint8_t>(rhs.origin);
                      }
                      if (lhs.range.start_ms != rhs.range.start_ms) {
                          return lhs.range.start_ms < rhs.range.start_ms;
                      }
                      return lhs.range.end_ms < rhs.range.end_ms;
                  });
    }
    if (files.size() > 1) {
        std::sort(files.data(),
                  files.data() + files.size(),
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
                  });
    }
    if (fallbacks.size() > 1) {
        std::sort(fallbacks.data(),
                  fallbacks.data() + fallbacks.size(),
                  [](const BuildFallback &lhs,
                     const BuildFallback &rhs) {
                      return lhs.owner < rhs.owner;
                  });
    }

    size_t final_sessions = 0;
    size_t final_files = 0;
    size_t final_signal_layouts = 0;
    size_t final_fallback_files = 0;
    size_t final_fallback_sections = 0;
    size_t final_paths = 0;
    for (size_t i = 0; i < nights.size(); ++i) {
        BuildNight &night = nights.data()[i];
        if (!night.boundary_set) {
            return build_failed(status,
                                NightCatalogBuildFailure::InvariantViolation,
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
            return build_failed(status,
                                NightCatalogBuildFailure::InvariantViolation,
                                "night_catalog_output_count_invalid");
        }

        for (size_t file_index = 0; file_index < files.size(); ++file_index) {
            const BuildFile &file = files.data()[file_index];
            if (file.owner != night.owner) continue;
            if (file.source.signal_layout_count > UINT16_MAX ||
                !add_count(final_signal_layouts,
                           file.source.signal_layout_count)) {
                return build_failed(
                    status,
                    NightCatalogBuildFailure::InvariantViolation,
                    "night_catalog_signal_count_invalid");
            }
        }
    }

    if (nights.size() > UINT32_MAX || final_sessions > UINT32_MAX ||
        final_files > UINT32_MAX ||
        final_signal_layouts > UINT32_MAX ||
        final_fallback_files > UINT32_MAX ||
        final_fallback_sections > UINT32_MAX ||
        final_paths > UINT32_MAX) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvariantViolation,
                            "night_catalog_output_count_invalid");
    }

    std::shared_ptr<NightCatalog> catalog(new (std::nothrow) NightCatalog());
    if (!catalog || !catalog->allocate(nights.size(),
                                       final_sessions,
                                       0,
                                       final_files,
                                       final_files,
                                       final_signal_layouts,
                                       final_fallback_files,
                                       final_fallback_sections,
                                       final_paths)) {
        return build_failed(status,
                            NightCatalogBuildFailure::AllocationFailed,
                            "night_catalog_output_alloc_failed");
    }

    size_t next_session = 0;
    size_t next_file = 0;
    size_t next_signal_layout = 0;
    size_t next_fallback_file = 0;
    size_t next_fallback_section = 0;
    size_t next_path = 0;
    for (size_t night_index = 0;
         night_index < nights.size();
         ++night_index) {
        const BuildNight &source = nights.data()[night_index];
        NightCatalogRecord &record = catalog->records_[night_index];
        record.sleep_day = source.sleep_day;
        record.day_start_ms = source.day_start_ms;
        record.day_end_ms = source.day_end_ms;
        record.session_offset = static_cast<uint32_t>(next_session);
        record.mask_window_offset = 0;
        record.file_offset = static_cast<uint32_t>(next_file);
        record.fallback_file_offset =
            static_cast<uint32_t>(next_fallback_file);
        if (source.has_edf) record.source_flags |= NIGHT_CATALOG_SOURCE_EDF;
        if (source.has_str) record.source_flags |= NIGHT_CATALOG_SOURCE_STR;
        if (source.has_fallback) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK;
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
        for (size_t i = 0; i < sessions.size(); ++i) {
            const BuildSession &session = sessions.data()[i];
            if (!selected_session(source, session)) continue;
            if (have_previous && same_range(previous, session.range)) continue;

            catalog->sessions_[next_session++] = session.range;
            previous = session.range;
            have_previous = true;
            ++record.session_count;
        }

        for (size_t i = 0; i < files.size(); ++i) {
            const BuildFile &source_file = files.data()[i];
            if (source_file.owner != source.owner) continue;

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
                return build_failed(
                    status,
                    NightCatalogBuildFailure::InvariantViolation,
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

        for (size_t i = 0; i < fallbacks.size(); ++i) {
            const BuildFallback &source_fallback = fallbacks.data()[i];
            if (source_fallback.owner != source.owner) continue;

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
                section.coverage = source_section.coverage;
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
    }

    if (next_session != final_sessions || next_file != final_files ||
        next_signal_layout != final_signal_layouts ||
        next_fallback_file != final_fallback_files ||
        next_fallback_section != final_fallback_sections ||
        next_path != final_paths) {
        return build_failed(status,
                            NightCatalogBuildFailure::InvariantViolation,
                            "night_catalog_output_mismatch");
    }
    return catalog;
}

std::shared_ptr<const NightCatalog> NightCatalogBuilder::replace_fallback(
    const NightCatalog &source,
    const char *path,
    const std::shared_ptr<const LargeByteBuffer> &artifact,
    int64_t last_write_ms) {
    if (!path || !path[0] || !artifact || artifact->size() == 0) return {};

    const size_t path_length = strlen(path);
    if (path_length > UINT16_MAX) return {};

    ReportFallbackArtifactView replacement;
    if (!ReportFallbackArtifactCodec::decode_metadata(
            artifact->data(), artifact->size(), replacement) ||
        replacement.info.total_bytes != artifact->size()) {
        return {};
    }

    const NightCatalogRecord *source_night =
        source.find(replacement.info.sleep_day);
    if (!source_night ||
        (source_night->source_flags & NIGHT_CATALOG_SOURCE_EDF) != 0 ||
        source_night->day_start_ms != replacement.info.day_start_ms ||
        source_night->day_end_ms != replacement.info.day_end_ms) {
        return {};
    }

    size_t source_session_count = 0;
    const NightCatalogTimeRange *source_sessions =
        source.sessions(*source_night, source_session_count);
    if (!source_sessions ||
        source_session_count != replacement.info.session_count) {
        return {};
    }
    for (size_t i = 0; i < source_session_count; ++i) {
        NightCatalogTimeRange replacement_session;
        if (!replacement.session(i, replacement_session) ||
            !same_range(source_sessions[i], replacement_session)) {
            return {};
        }
    }

    size_t removed_file_count = 0;
    const NightCatalogFallbackFile *removed_files =
        source.fallback_files(*source_night, removed_file_count);
    if (removed_file_count > 0 && !removed_files) return {};

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
            record.source_flags |= NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK;
            record.fallback_file_count = 1;

            NightCatalogFallbackFile &file =
                catalog->fallback_files_[next_fallback_file++];
            file.path_offset = static_cast<uint32_t>(next_path);
            file.path_length = static_cast<uint16_t>(path_length);
            file.section_offset =
                static_cast<uint32_t>(next_fallback_section);
            file.section_count = static_cast<uint16_t>(
                replacement.info.section_count);
            file.file_size = artifact->size();
            file.last_write_ms = last_write_ms;
            file.identity = replacement.info.content_identity;
            file.metadata_bytes = static_cast<uint32_t>(
                replacement.info.metadata_bytes);

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
                section.coverage = replacement_section.coverage;
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

        record.source_revision =
            SourceRevision(calculate_revision(*catalog, record));
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
