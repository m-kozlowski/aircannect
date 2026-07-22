#include "night_catalog_builder.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr uint64_t FNV_OFFSET = UINT64_C(14695981039346656037);
constexpr uint64_t FNV_PRIME = UINT64_C(1099511628211);

enum class SessionOrigin : uint8_t {
    Edf,
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
    bool summary_boundary_used = false;
};

struct BuildSession {
    size_t owner = 0;
    SessionOrigin origin = SessionOrigin::Edf;
    NightCatalogTimeRange range;
};

struct BuildMaskWindow {
    size_t owner = 0;
    NightCatalogTimeRange range;
};

struct BuildFile {
    size_t owner = 0;
    NightCatalogSourceFileInput source;
    NightCatalogTimeRange session_range;
    bool has_session = false;
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
                   bool fill_only) {
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
    if (input.has_duration_min) {
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
    if (file.identity != 0) return file.identity;

    const char *path = file.path ? file.path : "";
    uint64_t hash = FNV_OFFSET;
    hash = hash_byte(hash, static_cast<uint8_t>(file.kind));
    hash = hash_text(hash, path, strlen(path));
    hash = hash_u64(hash, file.file_size);
    hash = hash_i64(hash, file.last_write_ms);
    hash = hash_u64(hash, file.data_offset);
    hash = hash_u64(hash, file.data_size);
    hash = hash_u32(hash, file.header_size);
    hash = hash_u32(hash, file.record_size);
    hash = hash_u32(hash, file.record_duration_ms);
    hash = hash_u32(hash, file.complete_records);
    hash = hash_i64(hash, file.coverage.range.start_ms);
    hash = hash_i64(hash, file.coverage.range.end_ms);
    hash = hash_u32(hash, file.coverage.primary_signal_mask);
    hash = hash_u32(hash, file.coverage.fallback_signal_mask);
    return hash == 0 ? 1 : hash;
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
    if (!source.path || !source.path[0]) return false;
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
                ScratchArray<BuildMaskWindow> &masks,
                ScratchArray<BuildFile> &files) {
    if (input.str_record_count > 0 && !input.resolve_local_minute) {
        return false;
    }

    for (size_t i = 0; i < input.str_record_count; ++i) {
        const NightCatalogStrInput &source = input.str_records[i];
        if (!source.record.sleep_day.valid() || !source.path ||
            !source.path[0] || source.record_size == 0 ||
            source.record.source_identity == 0) {
            return false;
        }

        BuildNight *night = find_or_add_night(nights,
                                               source.record.sleep_day);
        if (!night || night->has_str) return false;

        int64_t day_start_ms = 0;
        int64_t day_end_ms = 0;
        if (!input.resolve_local_minute(input.clock_context,
                                        night->sleep_day,
                                        0,
                                        day_start_ms) ||
            !input.resolve_local_minute(input.clock_context,
                                        night->sleep_day,
                                        1440,
                                        day_end_ms) ||
            !set_primary_boundary(*night, day_start_ms, day_end_ms)) {
            return false;
        }

        for (size_t mask_index = 0;
             mask_index < source.record.mask_window_count;
             ++mask_index) {
            const NightStrMaskWindow &source_window =
                source.record.mask_windows[mask_index];
            BuildMaskWindow *mask = masks.append();
            if (!mask) return false;

            mask->owner = night->owner;
            if (!input.resolve_local_minute(input.clock_context,
                                            night->sleep_day,
                                            source_window.on_minute,
                                            mask->range.start_ms) ||
                !input.resolve_local_minute(input.clock_context,
                                            night->sleep_day,
                                            source_window.off_minute,
                                            mask->range.end_ms) ||
                mask->range.end_ms < mask->range.start_ms) {
                return false;
            }
        }

        NightCatalogSourceFileInput file;
        file.kind = NightCatalogFileKind::Str;
        file.path = source.path;
        file.coverage.range = {day_start_ms, day_end_ms};
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
        BuildNight *night = find_or_add_night(nights, source.sleep_day);
        if (!night || night->has_summary || source.identity == 0 ||
            !valid_boundary(source.day_start_ms, source.day_end_ms)) {
            return false;
        }

        if (!night->boundary_set) {
            night->day_start_ms = source.day_start_ms;
            night->day_end_ms = source.day_end_ms;
            night->boundary_set = true;
            night->summary_boundary_used = true;
        }

        for (size_t session_index = 0;
             session_index < source.session_count;
             ++session_index) {
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
    }
    return true;
}

bool selected_session(const BuildNight &night,
                      const BuildSession &session) {
    return session.owner == night.owner &&
           session.origin == (night.has_edf ? SessionOrigin::Edf
                                            : SessionOrigin::Summary);
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

size_t count_unique_masks(const BuildNight &night,
                          const ScratchArray<BuildMaskWindow> &masks) {
    size_t count = 0;
    NightCatalogTimeRange previous;
    bool have_previous = false;
    for (size_t i = 0; i < masks.size(); ++i) {
        const BuildMaskWindow &mask = masks.data()[i];
        if (mask.owner != night.owner) continue;
        if (have_previous && same_range(previous, mask.range)) continue;
        previous = mask.range;
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
    }

    return hash == 0 ? 1 : hash;
}

}  // namespace

std::shared_ptr<const NightCatalog> NightCatalogBuilder::build(
    const NightCatalogBuildInput &input) {
    if ((input.edf_session_count > 0 && !input.edf_sessions) ||
        (input.str_record_count > 0 && !input.str_records) ||
        (input.summary_record_count > 0 && !input.summary_records)) {
        return {};
    }

    size_t max_nights = 0;
    size_t max_sessions = 0;
    size_t max_masks = 0;
    size_t max_files = 0;
    if (!add_count(max_nights, input.edf_session_count) ||
        !add_count(max_nights, input.str_record_count) ||
        !add_count(max_nights, input.summary_record_count) ||
        !add_count(max_sessions, input.edf_session_count) ||
        !add_count(max_files, input.str_record_count)) {
        return {};
    }

    for (size_t i = 0; i < input.edf_session_count; ++i) {
        if (!add_count(max_files, input.edf_sessions[i].file_count)) return {};
    }
    for (size_t i = 0; i < input.str_record_count; ++i) {
        if (!add_count(max_masks,
                       input.str_records[i].record.mask_window_count)) {
            return {};
        }
    }
    for (size_t i = 0; i < input.summary_record_count; ++i) {
        if (!add_count(max_sessions,
                       input.summary_records[i].session_count)) {
            return {};
        }
    }

    ScratchArray<BuildNight> nights;
    ScratchArray<BuildSession> sessions;
    ScratchArray<BuildMaskWindow> masks;
    ScratchArray<BuildFile> files;
    if (!nights.allocate(max_nights) ||
        !sessions.allocate(max_sessions) ||
        !masks.allocate(max_masks) ||
        !files.allocate(max_files) ||
        !ingest_edf(input, nights, sessions, files) ||
        !ingest_str(input, nights, masks, files) ||
        !ingest_summary(input, nights, sessions)) {
        return {};
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
    if (masks.size() > 1) {
        std::sort(masks.data(),
                  masks.data() + masks.size(),
                  [](const BuildMaskWindow &lhs,
                     const BuildMaskWindow &rhs) {
                      if (lhs.owner != rhs.owner) {
                          return lhs.owner < rhs.owner;
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

    size_t final_sessions = 0;
    size_t final_masks = 0;
    size_t final_files = 0;
    size_t final_paths = 0;
    for (size_t i = 0; i < nights.size(); ++i) {
        BuildNight &night = nights.data()[i];
        if (!night.boundary_set) return {};

        const size_t session_count = count_unique_sessions(night, sessions);
        const size_t mask_count = count_unique_masks(night, masks);
        const size_t file_count = count_files(night, files, final_paths);
        if (file_count == SIZE_MAX || session_count > UINT16_MAX ||
            mask_count > UINT16_MAX || file_count > UINT16_MAX ||
            !add_count(final_sessions, session_count) ||
            !add_count(final_masks, mask_count) ||
            !add_count(final_files, file_count)) {
            return {};
        }
    }

    if (nights.size() > UINT32_MAX || final_sessions > UINT32_MAX ||
        final_masks > UINT32_MAX || final_files > UINT32_MAX ||
        final_paths > UINT32_MAX) {
        return {};
    }

    std::shared_ptr<NightCatalog> catalog(new (std::nothrow) NightCatalog());
    if (!catalog || !catalog->allocate(nights.size(),
                                       final_sessions,
                                       final_masks,
                                       final_files,
                                       final_files,
                                       final_paths)) {
        return {};
    }

    size_t next_session = 0;
    size_t next_mask = 0;
    size_t next_file = 0;
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
        record.mask_window_offset = static_cast<uint32_t>(next_mask);
        record.file_offset = static_cast<uint32_t>(next_file);
        if (source.has_edf) record.source_flags |= NIGHT_CATALOG_SOURCE_EDF;
        if (source.has_str) record.source_flags |= NIGHT_CATALOG_SOURCE_STR;

        bool summary_used = source.summary_boundary_used ||
                            (!source.has_edf && source.has_summary);
        if (source.has_str) {
            (void)apply_metrics(record.metrics,
                                source.str_metrics,
                                NightCatalogMetricSource::Str,
                                false);
        }
        if (source.has_summary &&
            apply_metrics(record.metrics,
                          source.summary_metrics,
                          NightCatalogMetricSource::Summary,
                          true)) {
            summary_used = true;
        }
        if (summary_used) {
            record.source_flags |= NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK;
            record.summary_identity = source.summary_identity;
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

        have_previous = false;
        for (size_t i = 0; i < masks.size(); ++i) {
            const BuildMaskWindow &mask = masks.data()[i];
            if (mask.owner != source.owner) continue;
            if (have_previous && same_range(previous, mask.range)) continue;

            catalog->mask_windows_[next_mask++] = mask.range;
            previous = mask.range;
            have_previous = true;
            ++record.mask_window_count;
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
                return {};
            }

            file.coverage_offset = static_cast<uint32_t>(next_file);
            file.coverage_count = 1;
            file.file_size = source_file.source.file_size;
            file.last_write_ms = source_file.source.last_write_ms;
            file.data_offset = source_file.source.data_offset;
            file.data_size = source_file.source.data_size;
            file.identity = source_file.source.identity;
            file.header_size = source_file.source.header_size;
            file.record_size = source_file.source.record_size;
            file.record_duration_ms =
                source_file.source.record_duration_ms;
            file.complete_records = source_file.source.complete_records;
            catalog->coverage_[next_file] = source_file.source.coverage;

            memcpy(catalog->paths_ + next_path,
                   source_file.source.path,
                   path_len + 1);
            next_path += path_len + 1;
            ++next_file;
            ++record.file_count;
        }

        record.source_revision =
            SourceRevision(calculate_revision(*catalog, record));
    }

    if (next_session != final_sessions || next_mask != final_masks ||
        next_file != final_files || next_path != final_paths) {
        return {};
    }
    return catalog;
}

}  // namespace aircannect
