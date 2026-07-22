#include "report_planner.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>

#include "storage_read_port.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

enum class SelectedStorage : uint8_t {
    Edf,
    Fallback,
};

struct SelectedSource {
    NightCatalogTimeRange output_window;
    ReportSeriesDescriptor series;
    EdfReportSignalLayout layout;
    ReportReadQuality quality = ReportReadQuality::Primary;
    SelectedStorage storage = SelectedStorage::Edf;
    uint16_t session_index = 0;
    uint16_t catalog_file_index = 0;
    bool complete = false;
};

struct SourceCandidate {
    ReportSeriesDescriptor series;
    EdfReportSignalLayout layout;
    uint16_t catalog_file_index = 0;
    ReportReadQuality quality = ReportReadQuality::Primary;
    SelectedStorage storage = SelectedStorage::Edf;
    int64_t covered_ms = 0;
    bool available = false;
    bool complete = false;
};

struct PendingOperation {
    ReportReadOperation operation;
    ReportReadMapping mapping;
    bool has_mapping = false;
};

void *allocate_scratch(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes, false);
#else
    return calloc(1, bytes);
#endif
}

void free_scratch(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
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
        for (size_t i = 0; i < capacity_; ++i) new (&values_[i]) T();
        return true;
    }

    T *append() {
        return size_ < capacity_ ? &values_[size_++] : nullptr;
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

NightCatalogTimeRange intersect_ranges(const NightCatalogTimeRange &lhs,
                                       const NightCatalogTimeRange &rhs) {
    return {
        std::max(lhs.start_ms, rhs.start_ms),
        std::min(lhs.end_ms, rhs.end_ms),
    };
}

NightCatalogTimeRange requested_window(const ReportArtifactKey &artifact,
                                       const NightCatalogTimeRange &session) {
    if (artifact.kind != ReportArtifactKind::RangeTile) return session;
    return intersect_ranges(session,
                            {artifact.range_start_ms,
                             artifact.range_end_ms});
}

size_t requested_session_count(const NightCatalog &catalog,
                               const NightCatalogRecord &night,
                               const ReportArtifactKey &artifact) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    if (catalog_session_count > 0 && !sessions) return SIZE_MAX;

    size_t count = 0;
    for (size_t i = 0; i < catalog_session_count; ++i) {
        if (requested_window(artifact, sessions[i]).valid()) ++count;
    }
    return count;
}

size_t bit_count(uint32_t value) {
    size_t count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

ReportReadQuality source_quality(ReportSignalId signal,
                                 ReportSourceId source) {
    const ReportSignalDef *definition = report_signal_def(signal);
    return definition && source == definition->preferred_source
        ? ReportReadQuality::Primary
        : ReportReadQuality::Fallback;
}

ReportSeriesDescriptor series_descriptor(
    const EdfReportSignalLayout &layout) {
    ReportSeriesDescriptor series;
    series.signal = layout.signal;
    series.source = layout.source;
    series.sample_interval_ms = layout.sample_interval_ms;
    series.primary = layout.primary;
    return series;
}

bool file_data_valid(const NightCatalogSourceFile &file) {
    if (file.kind == NightCatalogFileKind::Str ||
        file.identity == 0 || file.record_start_ms <= 0 ||
        file.record_size == 0 || file.data_offset > file.file_size ||
        file.data_size > file.file_size - file.data_offset) {
        return false;
    }

    const uint64_t complete_bytes =
        static_cast<uint64_t>(file.complete_records) * file.record_size;
    return complete_bytes <= file.data_size;
}

uint32_t coverage_mask(const NightCatalogSourceCoverage &coverage,
                       ReportReadQuality quality) {
    return quality == ReportReadQuality::Primary
        ? coverage.primary_signal_mask
        : coverage.fallback_signal_mask;
}

const EdfReportSignalLayout *find_signal_layout(
    const NightCatalog &catalog,
    const NightCatalogSourceFile &file,
    ReportSignalId signal,
    ReportReadQuality quality) {
    size_t count = 0;
    const EdfReportSignalLayout *layouts =
        catalog.signal_layouts(file, count);
    const bool primary = quality == ReportReadQuality::Primary;
    for (size_t i = 0; layouts && i < count; ++i) {
        if (layouts[i].signal == signal && layouts[i].primary == primary) {
            return &layouts[i];
        }
    }
    return nullptr;
}

SourceCandidate evaluate_edf_candidate(
    const NightCatalog &catalog,
    const NightCatalogSourceFile &file,
    uint16_t file_index,
    ReportSignalId signal,
    ReportReadQuality quality,
    const NightCatalogTimeRange &window) {
    SourceCandidate candidate;
    candidate.catalog_file_index = file_index;
    candidate.quality = quality;
    candidate.storage = SelectedStorage::Edf;
    if (!file_data_valid(file) || file.record_duration_ms == 0) {
        return candidate;
    }

    const EdfReportSignalLayout *layout =
        find_signal_layout(catalog, file, signal, quality);
    if (!layout) return candidate;
    candidate.layout = *layout;
    candidate.series = series_descriptor(*layout);

    const uint32_t bit = report_signal_bit(signal);
    size_t coverage_count = 0;
    const NightCatalogSourceCoverage *coverage =
        catalog.coverage(file, coverage_count);
    if (bit == 0 || coverage_count == 0 || !coverage) return candidate;

    int64_t covered_ms = 0;
    for (size_t i = 0; i < coverage_count; ++i) {
        if ((coverage_mask(coverage[i], quality) & bit) == 0) continue;

        const NightCatalogTimeRange overlap =
            intersect_ranges(window, coverage[i].range);
        if (!overlap.valid()) continue;

        candidate.available = true;
        const int64_t duration = overlap.end_ms - overlap.start_ms;
        if (covered_ms > window.end_ms - window.start_ms - duration) {
            covered_ms = window.end_ms - window.start_ms;
        } else {
            covered_ms += duration;
        }
    }
    candidate.covered_ms = covered_ms;
    if (!candidate.available) return candidate;

    const int64_t target_start = std::min(
        window.end_ms,
        window.start_ms + REPORT_SOURCE_EDGE_TOLERANCE_MS);
    const int64_t target_end = std::max(
        window.start_ms,
        window.end_ms - REPORT_SOURCE_EDGE_TOLERANCE_MS);
    if (target_end <= target_start) {
        candidate.complete = true;
        return candidate;
    }

    int64_t cursor = target_start;
    for (size_t pass = 0; pass < coverage_count && cursor < target_end;
         ++pass) {
        int64_t next = cursor;
        for (size_t i = 0; i < coverage_count; ++i) {
            if ((coverage_mask(coverage[i], quality) & bit) == 0) continue;

            const NightCatalogTimeRange overlap =
                intersect_ranges(window, coverage[i].range);
            if (overlap.valid() && overlap.start_ms <= cursor &&
                overlap.end_ms > next) {
                next = overlap.end_ms;
            }
        }
        if (next == cursor) break;
        cursor = next;
    }
    candidate.complete = cursor >= target_end;
    return candidate;
}

SourceCandidate evaluate_fallback_candidate(
    const NightCatalog &catalog,
    const NightCatalogFallbackFile &file,
    uint16_t file_index,
    ReportSignalId signal,
    ReportSourceId source,
    const NightCatalogTimeRange &window,
    bool &catalog_valid) {
    SourceCandidate candidate;
    candidate.catalog_file_index = file_index;
    candidate.quality = source_quality(signal, source);
    candidate.storage = SelectedStorage::Fallback;
    candidate.series.signal = signal;
    candidate.series.source = source;
    candidate.series.primary =
        candidate.quality == ReportReadQuality::Primary;

    size_t section_count = 0;
    const NightCatalogFallbackSection *sections =
        catalog.fallback_sections(file, section_count);
    if (section_count > 0 && !sections) {
        catalog_valid = false;
        return candidate;
    }

    for (size_t i = 0; i < section_count; ++i) {
        const NightCatalogFallbackSection &section = sections[i];
        if (section.kind != ReportFallbackSectionKind::Series ||
            section.signal != signal || section.source != source) {
            continue;
        }
        if (candidate.series.sample_interval_ms != 0 &&
            candidate.series.sample_interval_ms !=
                section.sample_interval_ms) {
            catalog_valid = false;
            return candidate;
        }

        const NightCatalogTimeRange overlap =
            intersect_ranges(window, section.coverage);
        if (!overlap.valid()) continue;

        candidate.available = true;
        candidate.series.sample_interval_ms = section.sample_interval_ms;
        const int64_t duration = overlap.end_ms - overlap.start_ms;
        if (candidate.covered_ms >
            window.end_ms - window.start_ms - duration) {
            candidate.covered_ms = window.end_ms - window.start_ms;
        } else {
            candidate.covered_ms += duration;
        }
    }
    if (!candidate.available) return candidate;

    const int64_t target_start = std::min(
        window.end_ms,
        window.start_ms + REPORT_SOURCE_EDGE_TOLERANCE_MS);
    const int64_t target_end = std::max(
        window.start_ms,
        window.end_ms - REPORT_SOURCE_EDGE_TOLERANCE_MS);
    if (target_end <= target_start) {
        candidate.complete = true;
        return candidate;
    }

    int64_t cursor = target_start;
    for (size_t pass = 0; pass < section_count && cursor < target_end;
         ++pass) {
        int64_t next = cursor;
        for (size_t i = 0; i < section_count; ++i) {
            const NightCatalogFallbackSection &section = sections[i];
            if (section.kind != ReportFallbackSectionKind::Series ||
                section.signal != signal || section.source != source) {
                continue;
            }

            const NightCatalogTimeRange overlap =
                intersect_ranges(window, section.coverage);
            if (overlap.valid() && overlap.start_ms <= cursor &&
                overlap.end_ms > next) {
                next = overlap.end_ms;
            }
        }
        if (next == cursor) break;
        cursor = next;
    }
    candidate.complete = cursor >= target_end;
    return candidate;
}

uint8_t candidate_rank(const SourceCandidate &candidate) {
    if (!candidate.available) return 0;
    if (candidate.complete) {
        if (candidate.storage == SelectedStorage::Fallback) return 6;
        return candidate.quality == ReportReadQuality::Primary ? 8 : 7;
    }
    if (candidate.storage == SelectedStorage::Edf) {
        return candidate.quality == ReportReadQuality::Primary ? 4 : 2;
    }
    return candidate.quality == ReportReadQuality::Primary ? 3 : 1;
}

bool candidate_better(const SourceCandidate &candidate,
                      const SourceCandidate &current) {
    const uint8_t candidate_score = candidate_rank(candidate);
    const uint8_t current_score = candidate_rank(current);
    if (candidate_score != current_score) {
        return candidate_score > current_score;
    }
    if (candidate.covered_ms != current.covered_ms) {
        return candidate.covered_ms > current.covered_ms;
    }
    return candidate.available &&
           (!current.available ||
            candidate.catalog_file_index < current.catalog_file_index);
}

SourceCandidate select_source(const NightCatalog &catalog,
                              const NightCatalogRecord &night,
                              uint16_t catalog_session_index,
                              ReportSignalId signal,
                              const NightCatalogTimeRange &window,
                              bool &catalog_valid) {
    SourceCandidate selected;
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    if (file_count > 0 && !files) {
        catalog_valid = false;
        return selected;
    }

    for (size_t i = 0; i < file_count; ++i) {
        const NightCatalogSourceFile &file = files[i];
        if (file.session_index != catalog_session_index ||
            file.kind == NightCatalogFileKind::Eve ||
            file.kind == NightCatalogFileKind::Csl ||
            file.kind == NightCatalogFileKind::Str) {
            continue;
        }
        if (i > UINT16_MAX) {
            catalog_valid = false;
            return selected;
        }

        for (ReportReadQuality quality : {ReportReadQuality::Primary,
                                          ReportReadQuality::Fallback}) {
            const SourceCandidate candidate = evaluate_edf_candidate(
                catalog,
                file,
                static_cast<uint16_t>(i),
                signal,
                quality,
                window);
            if (candidate_better(candidate, selected)) selected = candidate;
        }
    }

    const ReportSignalDef *signal_definition = report_signal_def(signal);
    size_t fallback_file_count = 0;
    const NightCatalogFallbackFile *fallback_files =
        catalog.fallback_files(night, fallback_file_count);
    if (!signal_definition ||
        (fallback_file_count > 0 && !fallback_files)) {
        catalog_valid = false;
        return selected;
    }

    for (size_t i = 0; i < fallback_file_count; ++i) {
        if (i > UINT16_MAX) {
            catalog_valid = false;
            return selected;
        }

        const ReportSourceId sources[] = {
            signal_definition->preferred_source,
            signal_definition->fallback_source,
        };
        for (size_t source_index = 0; source_index < 2; ++source_index) {
            if (source_index > 0 && sources[1] == sources[0]) continue;

            const SourceCandidate candidate = evaluate_fallback_candidate(
                catalog,
                fallback_files[i],
                static_cast<uint16_t>(i),
                signal,
                sources[source_index],
                window,
                catalog_valid);
            if (!catalog_valid) return selected;
            if (candidate_better(candidate, selected)) selected = candidate;
        }
    }
    return selected;
}

bool select_sources(const ReportPlanRequest &request,
                    const NightCatalog &catalog,
                    const NightCatalogRecord &night,
                    ScratchArray<SelectedSource> &selected) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    if (catalog_session_count > 0 && !sessions) return false;

    uint16_t plan_session_index = 0;
    for (size_t catalog_session_index = 0;
         catalog_session_index < catalog_session_count;
         ++catalog_session_index) {
        const NightCatalogTimeRange window =
            requested_window(request.artifact, sessions[catalog_session_index]);
        if (!window.valid()) continue;
        if (catalog_session_index > UINT16_MAX) return false;

        for (uint8_t signal_index = 0;
             signal_index < static_cast<uint8_t>(ReportSignalId::Count);
             ++signal_index) {
            const ReportSignalId signal =
                static_cast<ReportSignalId>(signal_index);
            if ((request.signal_mask & report_signal_bit(signal)) == 0) {
                continue;
            }

            bool catalog_valid = true;
            const SourceCandidate source = select_source(
                catalog,
                night,
                static_cast<uint16_t>(catalog_session_index),
                signal,
                window,
                catalog_valid);
            if (!catalog_valid) return false;
            if (!source.available) continue;

            SelectedSource *entry = selected.append();
            if (!entry) return false;
            entry->output_window = window;
            entry->series = source.series;
            entry->layout = source.layout;
            entry->quality = source.quality;
            entry->storage = source.storage;
            entry->session_index = plan_session_index;
            entry->catalog_file_index = source.catalog_file_index;
            entry->complete = source.complete;
        }
        ++plan_session_index;
    }
    return true;
}

bool record_window(const NightCatalogSourceFile &file,
                   const NightCatalogTimeRange &window,
                   uint32_t &first_record,
                   uint32_t &end_record) {
    first_record = 0;
    end_record = 0;
    if (!file_data_valid(file) || file.record_duration_ms == 0 ||
        file.complete_records == 0 || !window.valid()) {
        return false;
    }

    const int64_t duration = file.record_duration_ms;
    int64_t first = 0;
    if (window.start_ms > file.record_start_ms) {
        first = (window.start_ms - file.record_start_ms) / duration;
    }

    int64_t end = 0;
    if (window.end_ms > file.record_start_ms) {
        const int64_t delta = window.end_ms - file.record_start_ms;
        end = (delta + duration - 1) / duration;
    }

    first = std::max<int64_t>(0, first);
    end = std::max<int64_t>(0, end);
    first = std::min<int64_t>(file.complete_records, first);
    end = std::min<int64_t>(file.complete_records, end);
    if (end <= first) return false;

    first_record = static_cast<uint32_t>(first);
    end_record = static_cast<uint32_t>(end);
    return true;
}

uint32_t records_per_operation(const NightCatalogSourceFile &file) {
    if (file.record_size == 0 ||
        file.record_size > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
        return 0;
    }
    return static_cast<uint32_t>(
        AC_STORAGE_PREPARED_READ_MAX_BYTES / file.record_size);
}

bool operation_span(const NightCatalogSourceFile &file,
                    uint32_t first_record,
                    uint32_t record_count,
                    uint64_t &offset,
                    uint32_t &length) {
    const uint64_t relative =
        static_cast<uint64_t>(first_record) * file.record_size;
    const uint64_t bytes =
        static_cast<uint64_t>(record_count) * file.record_size;
    if (relative > file.data_size || bytes > file.data_size - relative ||
        file.data_offset > UINT64_MAX - relative || bytes > UINT32_MAX ||
        bytes > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
        return false;
    }

    offset = file.data_offset + relative;
    length = static_cast<uint32_t>(bytes);
    return true;
}

size_t operation_count_for_records(const NightCatalogSourceFile &file,
                                   uint32_t record_count) {
    const uint32_t per_operation = records_per_operation(file);
    if (per_operation == 0) return SIZE_MAX;
    return (static_cast<size_t>(record_count) + per_operation - 1) /
           per_operation;
}

bool count_edf_numeric_operations(const NightCatalog &catalog,
                                  const NightCatalogRecord &night,
                                  const SelectedSource &selected,
                                  size_t &count) {
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    if (!files || selected.catalog_file_index >= file_count) return false;

    const NightCatalogSourceFile &file = files[selected.catalog_file_index];
    size_t coverage_count = 0;
    const NightCatalogSourceCoverage *coverage =
        catalog.coverage(file, coverage_count);
    if (!coverage) return false;

    const uint32_t bit = report_signal_bit(selected.layout.signal);
    for (size_t i = 0; i < coverage_count; ++i) {
        if ((coverage_mask(coverage[i], selected.quality) & bit) == 0) {
            continue;
        }

        const NightCatalogTimeRange output =
            intersect_ranges(selected.output_window, coverage[i].range);
        uint32_t first_record = 0;
        uint32_t end_record = 0;
        if (!record_window(file, output, first_record, end_record)) continue;

        const size_t operations =
            operation_count_for_records(file, end_record - first_record);
        if (operations == SIZE_MAX || !add_count(count, operations)) {
            return false;
        }
    }
    return true;
}

bool count_fallback_series_operations(const NightCatalog &catalog,
                                      const NightCatalogRecord &night,
                                      const SelectedSource &selected,
                                      size_t &count) {
    size_t file_count = 0;
    const NightCatalogFallbackFile *files =
        catalog.fallback_files(night, file_count);
    if (!files || selected.catalog_file_index >= file_count) return false;

    size_t section_count = 0;
    const NightCatalogFallbackSection *sections =
        catalog.fallback_sections(files[selected.catalog_file_index],
                                  section_count);
    if (section_count > 0 && !sections) return false;

    for (size_t i = 0; i < section_count; ++i) {
        const NightCatalogFallbackSection &section = sections[i];
        if (section.kind != ReportFallbackSectionKind::Series ||
            section.signal != selected.series.signal ||
            section.source != selected.series.source ||
            !intersect_ranges(selected.output_window,
                              section.coverage).valid()) {
            continue;
        }
        if (!add_count(count, 1)) return false;
    }
    return true;
}

bool count_numeric_operations(const NightCatalog &catalog,
                              const NightCatalogRecord &night,
                              const SelectedSource &selected,
                              size_t &count) {
    return selected.storage == SelectedStorage::Edf
        ? count_edf_numeric_operations(catalog, night, selected, count)
        : count_fallback_series_operations(catalog,
                                           night,
                                           selected,
                                           count);
}

bool event_kind(NightCatalogFileKind kind,
                uint8_t &event_mask,
                ReportReadOperationKind &operation_kind) {
    if (kind == NightCatalogFileKind::Eve) {
        event_mask = REPORT_EVENT_SCORED;
        operation_kind = ReportReadOperationKind::ScoredEvents;
        return true;
    }
    if (kind == NightCatalogFileKind::Csl) {
        event_mask = REPORT_EVENT_CSR;
        operation_kind = ReportReadOperationKind::CsrEvents;
        return true;
    }
    return false;
}

uint8_t edf_captured_events(const ReportPlanRequest &request,
                            const NightCatalog &catalog,
                            const NightCatalogRecord &night,
                            uint16_t catalog_session_index) {
    uint8_t captured = 0;
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    for (size_t i = 0; files && i < file_count; ++i) {
        uint8_t mask = 0;
        ReportReadOperationKind ignored;
        if (files[i].session_index == catalog_session_index &&
            event_kind(files[i].kind, mask, ignored)) {
            captured |= mask;
        }
    }
    return captured & request.event_mask;
}

template <typename SectionMatches>
bool fallback_sections_complete(const NightCatalog &catalog,
                                const NightCatalogRecord &night,
                                const NightCatalogTimeRange &window,
                                SectionMatches section_matches,
                                bool &catalog_valid) {
    size_t file_count = 0;
    const NightCatalogFallbackFile *files =
        catalog.fallback_files(night, file_count);
    if (file_count > 0 && !files) {
        catalog_valid = false;
        return false;
    }

    const int64_t target_start = std::min(
        window.end_ms,
        window.start_ms + REPORT_SOURCE_EDGE_TOLERANCE_MS);
    const int64_t target_end = std::max(
        window.start_ms,
        window.end_ms - REPORT_SOURCE_EDGE_TOLERANCE_MS);
    if (target_end <= target_start) return true;

    int64_t cursor = target_start;
    size_t section_total = 0;
    for (size_t file_index = 0; file_index < file_count; ++file_index) {
        size_t section_count = 0;
        const NightCatalogFallbackSection *sections =
            catalog.fallback_sections(files[file_index], section_count);
        if (section_count > 0 && !sections) {
            catalog_valid = false;
            return false;
        }
        if (!add_count(section_total, section_count)) {
            catalog_valid = false;
            return false;
        }
    }

    for (size_t pass = 0; pass < section_total && cursor < target_end;
         ++pass) {
        int64_t next = cursor;
        for (size_t file_index = 0; file_index < file_count; ++file_index) {
            size_t section_count = 0;
            const NightCatalogFallbackSection *sections =
                catalog.fallback_sections(files[file_index], section_count);
            for (size_t i = 0; sections && i < section_count; ++i) {
                const NightCatalogFallbackSection &section = sections[i];
                if (!section_matches(section)) continue;

                const NightCatalogTimeRange overlap =
                    intersect_ranges(window, section.coverage);
                if (overlap.valid() && overlap.start_ms <= cursor &&
                    overlap.end_ms > next) {
                    next = overlap.end_ms;
                }
            }
        }
        if (next == cursor) break;
        cursor = next;
    }
    return cursor >= target_end;
}

bool fallback_event_complete(const NightCatalog &catalog,
                             const NightCatalogRecord &night,
                             uint8_t event_bit,
                             const NightCatalogTimeRange &window,
                             bool &catalog_valid) {
    return fallback_sections_complete(
        catalog,
        night,
        window,
        [event_bit](const NightCatalogFallbackSection &section) {
            return section.kind == ReportFallbackSectionKind::Events &&
                   (section.event_mask & event_bit) != 0;
        },
        catalog_valid);
}

bool fallback_signal_unavailable(const NightCatalog &catalog,
                                 const NightCatalogRecord &night,
                                 ReportSignalId signal,
                                 const NightCatalogTimeRange &window,
                                 bool &catalog_valid) {
    const ReportSignalDef *definition = report_signal_def(signal);
    if (!definition) {
        catalog_valid = false;
        return false;
    }

    const ReportSourceId source = definition->fallback_source;
    return fallback_sections_complete(
        catalog,
        night,
        window,
        [signal, source](const NightCatalogFallbackSection &section) {
            return section.kind ==
                       ReportFallbackSectionKind::Unavailable &&
                   section.signal == signal && section.source == source;
        },
        catalog_valid);
}

uint8_t captured_events(const ReportPlanRequest &request,
                        const NightCatalog &catalog,
                        const NightCatalogRecord &night,
                        uint16_t catalog_session_index,
                        const NightCatalogTimeRange &window,
                        bool &catalog_valid) {
    uint8_t captured = edf_captured_events(request,
                                           catalog,
                                           night,
                                           catalog_session_index);
    for (uint8_t bit : {REPORT_EVENT_SCORED, REPORT_EVENT_CSR}) {
        if ((request.event_mask & bit) == 0 || (captured & bit) != 0) {
            continue;
        }
        if (fallback_event_complete(catalog,
                                    night,
                                    bit,
                                    window,
                                    catalog_valid)) {
            captured |= bit;
        }
        if (!catalog_valid) return 0;
    }
    return captured;
}

bool count_event_operations(const ReportPlanRequest &request,
                            const NightCatalog &catalog,
                            const NightCatalogRecord &night,
                            size_t &count) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    size_t fallback_file_count = 0;
    const NightCatalogFallbackFile *fallback_files =
        catalog.fallback_files(night, fallback_file_count);
    if ((catalog_session_count > 0 && !sessions) ||
        (file_count > 0 && !files) ||
        (fallback_file_count > 0 && !fallback_files)) {
        return false;
    }

    for (size_t catalog_session = 0;
         catalog_session < catalog_session_count;
         ++catalog_session) {
        if (!requested_window(request.artifact,
                              sessions[catalog_session]).valid()) {
            continue;
        }

        const NightCatalogTimeRange filter =
            requested_window(request.artifact, sessions[catalog_session]);
        const uint8_t fallback_mask = request.event_mask &
            ~edf_captured_events(request,
                                 catalog,
                                 night,
                                 static_cast<uint16_t>(catalog_session));

        for (size_t file_index = 0; file_index < file_count; ++file_index) {
            const NightCatalogSourceFile &file = files[file_index];
            uint8_t mask = 0;
            ReportReadOperationKind ignored;
            if (file.session_index != catalog_session ||
                !event_kind(file.kind, mask, ignored) ||
                (request.event_mask & mask) == 0) {
                continue;
            }
            if (!file_data_valid(file)) return false;
            if (file.complete_records == 0) continue;

            const size_t operations =
                operation_count_for_records(file, file.complete_records);
            if (operations == SIZE_MAX || !add_count(count, operations)) {
                return false;
            }
        }

        if (fallback_mask == 0) continue;
        for (size_t file_index = 0;
             file_index < fallback_file_count;
             ++file_index) {
            size_t section_count = 0;
            const NightCatalogFallbackSection *sections =
                catalog.fallback_sections(fallback_files[file_index],
                                          section_count);
            if (section_count > 0 && !sections) return false;

            for (size_t section_index = 0;
                 section_index < section_count;
                 ++section_index) {
                const NightCatalogFallbackSection &section =
                    sections[section_index];
                if (section.kind != ReportFallbackSectionKind::Events ||
                    section.record_count == 0 ||
                    (section.event_mask & fallback_mask) == 0 ||
                    !intersect_ranges(filter, section.coverage).valid()) {
                    continue;
                }
                if (!add_count(count, 1)) return false;
            }
        }
    }
    return true;
}

bool append_record_operations(ScratchArray<PendingOperation> &pending,
                              const NightCatalogSourceFile &file,
                              uint16_t catalog_file_index,
                              uint16_t session_index,
                              ReportReadOperationKind kind,
                              uint32_t first_record,
                              uint32_t end_record,
                              const NightCatalogTimeRange &event_filter,
                              const ReportReadMapping *mapping) {
    const uint32_t per_operation = records_per_operation(file);
    if (per_operation == 0) return false;

    for (uint32_t record = first_record; record < end_record;) {
        const uint32_t count =
            std::min(per_operation, end_record - record);
        uint64_t offset = 0;
        uint32_t length = 0;
        if (!operation_span(file, record, count, offset, length)) return false;

        PendingOperation *entry = pending.append();
        if (!entry) return false;
        entry->operation.offset = offset;
        entry->operation.length = length;
        entry->operation.first_record = record;
        entry->operation.record_count = count;
        entry->operation.session_index = session_index;
        entry->operation.catalog_file_index = catalog_file_index;
        entry->operation.kind = kind;
        entry->operation.event_filter = event_filter;
        if (mapping) {
            entry->mapping = *mapping;
            entry->has_mapping = true;
        }
        record += count;
    }
    return true;
}

bool append_edf_numeric_operations(
    const NightCatalog &catalog,
    const NightCatalogRecord &night,
    const SelectedSource &selected,
    ScratchArray<PendingOperation> &pending) {
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    if (!files || selected.catalog_file_index >= file_count) return false;

    const NightCatalogSourceFile &file = files[selected.catalog_file_index];
    size_t coverage_count = 0;
    const NightCatalogSourceCoverage *coverage =
        catalog.coverage(file, coverage_count);
    if (!coverage) return false;

    const uint32_t bit = report_signal_bit(selected.layout.signal);
    for (size_t i = 0; i < coverage_count; ++i) {
        if ((coverage_mask(coverage[i], selected.quality) & bit) == 0) {
            continue;
        }

        ReportReadMapping mapping;
        mapping.output_window =
            intersect_ranges(selected.output_window, coverage[i].range);
        mapping.series = selected.series;
        mapping.layout = selected.layout;

        uint32_t first_record = 0;
        uint32_t end_record = 0;
        if (!record_window(file,
                           mapping.output_window,
                           first_record,
                           end_record)) {
            continue;
        }
        if (!append_record_operations(pending,
                                      file,
                                      selected.catalog_file_index,
                                      selected.session_index,
                                      ReportReadOperationKind::Numeric,
                                      first_record,
                                      end_record,
                                      {},
                                      &mapping)) {
            return false;
        }
    }
    return true;
}

bool append_fallback_series_operations(
    const NightCatalog &catalog,
    const NightCatalogRecord &night,
    const SelectedSource &selected,
    ScratchArray<PendingOperation> &pending) {
    size_t file_count = 0;
    const NightCatalogFallbackFile *files =
        catalog.fallback_files(night, file_count);
    if (!files || selected.catalog_file_index >= file_count) return false;

    size_t section_count = 0;
    const NightCatalogFallbackSection *sections =
        catalog.fallback_sections(files[selected.catalog_file_index],
                                  section_count);
    if (section_count > 0 && !sections) return false;

    for (size_t i = 0; i < section_count; ++i) {
        const NightCatalogFallbackSection &section = sections[i];
        if (section.kind != ReportFallbackSectionKind::Series ||
            section.signal != selected.series.signal ||
            section.source != selected.series.source) {
            continue;
        }

        ReportReadMapping mapping;
        mapping.output_window =
            intersect_ranges(selected.output_window, section.coverage);
        if (!mapping.output_window.valid()) continue;
        if (i > UINT16_MAX || section.data_size == 0 ||
            section.data_size > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
            return false;
        }
        mapping.series = selected.series;

        PendingOperation *entry = pending.append();
        if (!entry) return false;
        entry->operation.offset = section.data_offset;
        entry->operation.length = section.data_size;
        entry->operation.record_count = section.record_count;
        entry->operation.session_index = selected.session_index;
        entry->operation.catalog_file_index =
            selected.catalog_file_index;
        entry->operation.fallback_section_index =
            static_cast<uint16_t>(i);
        entry->operation.kind = ReportReadOperationKind::FallbackSeries;
        entry->mapping = mapping;
        entry->has_mapping = true;
    }
    return true;
}

bool append_numeric_operations(const NightCatalog &catalog,
                               const NightCatalogRecord &night,
                               const SelectedSource &selected,
                               ScratchArray<PendingOperation> &pending) {
    return selected.storage == SelectedStorage::Edf
        ? append_edf_numeric_operations(catalog,
                                        night,
                                        selected,
                                        pending)
        : append_fallback_series_operations(catalog,
                                            night,
                                            selected,
                                            pending);
}

bool append_event_operations(const ReportPlanRequest &request,
                             const NightCatalog &catalog,
                             const NightCatalogRecord &night,
                             ScratchArray<PendingOperation> &pending) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    size_t file_count = 0;
    const NightCatalogSourceFile *files = catalog.files(night, file_count);
    size_t fallback_file_count = 0;
    const NightCatalogFallbackFile *fallback_files =
        catalog.fallback_files(night, fallback_file_count);
    if ((catalog_session_count > 0 && !sessions) ||
        (file_count > 0 && !files) ||
        (fallback_file_count > 0 && !fallback_files)) {
        return false;
    }

    uint16_t plan_session = 0;
    for (size_t catalog_session = 0;
         catalog_session < catalog_session_count;
         ++catalog_session) {
        const NightCatalogTimeRange filter =
            requested_window(request.artifact, sessions[catalog_session]);
        if (!filter.valid()) continue;
        const uint8_t fallback_mask = request.event_mask &
            ~edf_captured_events(request,
                                 catalog,
                                 night,
                                 static_cast<uint16_t>(catalog_session));

        for (size_t file_index = 0; file_index < file_count; ++file_index) {
            const NightCatalogSourceFile &file = files[file_index];
            uint8_t mask = 0;
            ReportReadOperationKind kind;
            if (file.session_index != catalog_session ||
                !event_kind(file.kind, mask, kind) ||
                (request.event_mask & mask) == 0) {
                continue;
            }
            if (!file_data_valid(file) || file_index > UINT16_MAX) {
                return false;
            }
            if (file.complete_records == 0) continue;

            if (!append_record_operations(
                    pending,
                    file,
                    static_cast<uint16_t>(file_index),
                    plan_session,
                    kind,
                    0,
                    file.complete_records,
                    filter,
                    nullptr)) {
                return false;
            }
        }

        if (fallback_mask != 0) {
            for (size_t file_index = 0;
                 file_index < fallback_file_count;
                 ++file_index) {
                if (file_index > UINT16_MAX) return false;

                size_t section_count = 0;
                const NightCatalogFallbackSection *sections =
                    catalog.fallback_sections(fallback_files[file_index],
                                              section_count);
                if (section_count > 0 && !sections) return false;

                for (size_t section_index = 0;
                     section_index < section_count;
                     ++section_index) {
                    const NightCatalogFallbackSection &section =
                        sections[section_index];
                    const uint8_t selected_mask =
                        section.event_mask & fallback_mask;
                    if (section.kind !=
                            ReportFallbackSectionKind::Events ||
                        section.record_count == 0 || selected_mask == 0 ||
                        !intersect_ranges(filter,
                                          section.coverage).valid() ||
                        section_index > UINT16_MAX ||
                        section.data_size == 0 ||
                        section.data_size >
                            AC_STORAGE_PREPARED_READ_MAX_BYTES) {
                        continue;
                    }

                    PendingOperation *entry = pending.append();
                    if (!entry) return false;
                    entry->operation.offset = section.data_offset;
                    entry->operation.length = section.data_size;
                    entry->operation.record_count = section.record_count;
                    entry->operation.session_index = plan_session;
                    entry->operation.catalog_file_index =
                        static_cast<uint16_t>(file_index);
                    entry->operation.fallback_section_index =
                        static_cast<uint16_t>(section_index);
                    entry->operation.kind =
                        ReportReadOperationKind::FallbackEvents;
                    entry->operation.event_mask = selected_mask;
                    entry->operation.event_filter = filter;
                }
            }
        }
        ++plan_session;
    }
    return true;
}

bool same_operation(const PendingOperation &lhs,
                    const PendingOperation &rhs) {
    const ReportReadOperation &a = lhs.operation;
    const ReportReadOperation &b = rhs.operation;
    return a.offset == b.offset && a.length == b.length &&
           a.first_record == b.first_record &&
           a.record_count == b.record_count &&
           a.session_index == b.session_index &&
           a.catalog_file_index == b.catalog_file_index &&
           a.fallback_section_index == b.fallback_section_index &&
           a.kind == b.kind &&
           a.event_mask == b.event_mask &&
           a.event_filter.start_ms == b.event_filter.start_ms &&
           a.event_filter.end_ms == b.event_filter.end_ms;
}

bool pending_less(const PendingOperation &lhs,
                  const PendingOperation &rhs) {
    const ReportReadOperation &a = lhs.operation;
    const ReportReadOperation &b = rhs.operation;
    if (a.catalog_file_index != b.catalog_file_index) {
        return a.catalog_file_index < b.catalog_file_index;
    }
    if (a.fallback_section_index != b.fallback_section_index) {
        return a.fallback_section_index < b.fallback_section_index;
    }
    if (a.first_record != b.first_record) {
        return a.first_record < b.first_record;
    }
    if (a.record_count != b.record_count) {
        return a.record_count < b.record_count;
    }
    if (a.session_index != b.session_index) {
        return a.session_index < b.session_index;
    }
    if (a.kind != b.kind) {
        return static_cast<uint8_t>(a.kind) < static_cast<uint8_t>(b.kind);
    }
    if (a.event_mask != b.event_mask) return a.event_mask < b.event_mask;
    if (a.event_filter.start_ms != b.event_filter.start_ms) {
        return a.event_filter.start_ms < b.event_filter.start_ms;
    }
    if (a.event_filter.end_ms != b.event_filter.end_ms) {
        return a.event_filter.end_ms < b.event_filter.end_ms;
    }
    if (lhs.has_mapping != rhs.has_mapping) return !lhs.has_mapping;
    if (!lhs.has_mapping) return false;
    if (lhs.mapping.series.signal != rhs.mapping.series.signal) {
        return static_cast<uint8_t>(lhs.mapping.series.signal) <
               static_cast<uint8_t>(rhs.mapping.series.signal);
    }
    if (lhs.mapping.series.source != rhs.mapping.series.source) {
        return static_cast<uint8_t>(lhs.mapping.series.source) <
               static_cast<uint8_t>(rhs.mapping.series.source);
    }
    if (lhs.mapping.series.primary != rhs.mapping.series.primary) {
        return lhs.mapping.series.primary < rhs.mapping.series.primary;
    }
    if (lhs.mapping.series.sample_interval_ms !=
        rhs.mapping.series.sample_interval_ms) {
        return lhs.mapping.series.sample_interval_ms <
               rhs.mapping.series.sample_interval_ms;
    }
    if (lhs.mapping.output_window.start_ms !=
        rhs.mapping.output_window.start_ms) {
        return lhs.mapping.output_window.start_ms <
               rhs.mapping.output_window.start_ms;
    }
    return lhs.mapping.output_window.end_ms <
           rhs.mapping.output_window.end_ms;
}

bool mappings_merge(const ReportReadMapping &lhs,
                    const ReportReadMapping &rhs) {
    return lhs.series.signal == rhs.series.signal &&
           lhs.series.source == rhs.series.source &&
           lhs.series.primary == rhs.series.primary &&
           lhs.series.sample_interval_ms == rhs.series.sample_interval_ms &&
           rhs.output_window.start_ms <= lhs.output_window.end_ms;
}

void count_final_entries(const ScratchArray<PendingOperation> &pending,
                         size_t &operation_count,
                         size_t &mapping_count) {
    operation_count = 0;
    mapping_count = 0;
    const PendingOperation *previous_operation = nullptr;
    ReportReadMapping previous_mapping;
    bool have_mapping = false;

    for (size_t i = 0; i < pending.size(); ++i) {
        const PendingOperation &entry = pending.data()[i];
        const bool new_operation =
            !previous_operation || !same_operation(*previous_operation, entry);
        if (new_operation) {
            ++operation_count;
            previous_operation = &entry;
            have_mapping = false;
        }
        if (!entry.has_mapping) continue;

        if (have_mapping && mappings_merge(previous_mapping, entry.mapping)) {
            previous_mapping.output_window.end_ms = std::max(
                previous_mapping.output_window.end_ms,
                entry.mapping.output_window.end_ms);
            continue;
        }
        ++mapping_count;
        previous_mapping = entry.mapping;
        have_mapping = true;
    }
}

bool fill_sessions(const ReportPlanRequest &request,
                   const NightCatalog &catalog,
                   const NightCatalogRecord &night,
                   const ScratchArray<SelectedSource> &selected,
                   ReportReadSession *plan_sessions,
                   size_t plan_session_count,
                   uint32_t &missing_required,
                   uint32_t &missing_optional,
                   uint32_t &unavailable_signals,
                   uint32_t &acquirable_signals,
                   uint8_t &missing_events) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    if (catalog_session_count > 0 && !sessions) return false;

    const uint32_t required_mask = report_signal_required_mask();
    uint16_t plan_session = 0;
    for (size_t catalog_session = 0;
         catalog_session < catalog_session_count;
         ++catalog_session) {
        const NightCatalogTimeRange window =
            requested_window(request.artifact, sessions[catalog_session]);
        if (!window.valid()) continue;

        if (plan_session >= plan_session_count) return false;
        ReportReadSession &output = plan_sessions[plan_session];
        output.output_window = window;
        output.catalog_session_index =
            static_cast<uint16_t>(catalog_session);
        for (size_t i = 0; i < selected.size(); ++i) {
            const SelectedSource &source = selected.data()[i];
            if (source.session_index != plan_session) continue;

            const uint32_t bit = report_signal_bit(source.series.signal);
            output.selected_signal_mask |= bit;
            if (source.complete) output.complete_signal_mask |= bit;
            if (source.quality == ReportReadQuality::Fallback) {
                output.fallback_signal_mask |= bit;
            }
        }

        output.missing_signal_mask =
            request.signal_mask & ~output.complete_signal_mask;
        bool catalog_valid = true;
        for (uint8_t signal_index = 0;
             signal_index < static_cast<uint8_t>(ReportSignalId::Count);
             ++signal_index) {
            const ReportSignalId signal =
                static_cast<ReportSignalId>(signal_index);
            const uint32_t bit = report_signal_bit(signal);
            if ((output.missing_signal_mask & bit) == 0) continue;

            if (fallback_signal_unavailable(catalog,
                                            night,
                                            signal,
                                            window,
                                            catalog_valid)) {
                output.unavailable_signal_mask |= bit;
            }
            if (!catalog_valid) return false;
        }

        output.captured_event_mask = captured_events(
            request,
            catalog,
            night,
            static_cast<uint16_t>(catalog_session),
            window,
            catalog_valid);
        if (!catalog_valid) return false;
        output.missing_event_mask =
            request.event_mask & ~output.captured_event_mask;

        missing_required |= output.missing_signal_mask & required_mask;
        missing_optional |= output.missing_signal_mask & ~required_mask;
        unavailable_signals |= output.unavailable_signal_mask;
        acquirable_signals |= output.missing_signal_mask &
                              ~output.unavailable_signal_mask;
        missing_events |= output.missing_event_mask;
        ++plan_session;
    }
    return plan_session == plan_session_count;
}

bool fill_operations(const ScratchArray<PendingOperation> &pending,
                     ReportReadOperation *plan_operations,
                     size_t plan_operation_count,
                     ReportReadMapping *plan_mappings,
                     size_t plan_mapping_count) {
    size_t operation_index = 0;
    size_t mapping_index = 0;
    const PendingOperation *previous_pending = nullptr;
    ReportReadOperation *operation = nullptr;
    ReportReadMapping *mapping = nullptr;

    for (size_t i = 0; i < pending.size(); ++i) {
        const PendingOperation &entry = pending.data()[i];
        const bool new_operation =
            !previous_pending || !same_operation(*previous_pending, entry);
        if (new_operation) {
            if (operation_index >= plan_operation_count) return false;
            operation = &plan_operations[operation_index++];
            *operation = entry.operation;
            operation->mapping_offset = static_cast<uint32_t>(mapping_index);
            operation->mapping_count = 0;
            mapping = nullptr;
            previous_pending = &entry;
        }
        if (!entry.has_mapping) continue;

        if (mapping && mappings_merge(*mapping, entry.mapping)) {
            mapping->output_window.end_ms = std::max(
                mapping->output_window.end_ms,
                entry.mapping.output_window.end_ms);
            continue;
        }
        if (!operation || mapping_index >= plan_mapping_count ||
            operation->mapping_count == UINT16_MAX) {
            return false;
        }

        mapping = &plan_mappings[mapping_index++];
        *mapping = entry.mapping;
        ++operation->mapping_count;
    }
    return operation_index == plan_operation_count &&
           mapping_index == plan_mapping_count;
}

}  // namespace

bool ReportPlanRequest::valid() const {
    return artifact.valid() &&
           (signal_mask & ~report_signal_mask_all()) == 0 &&
           (event_mask & ~REPORT_EVENT_ALL) == 0;
}

ReportPlanResult ReportPlanner::build(
    const ReportPlanRequest &request,
    std::shared_ptr<const NightCatalog> catalog) {
    ReportPlanResult result;
    if (!request.valid() || !catalog) {
        result.status = ReportPlanStatus::InvalidRequest;
        return result;
    }

    const NightCatalogRecord *night = catalog->find(request.artifact.sleep_day);
    if (!night) {
        result.status = ReportPlanStatus::NightMissing;
        return result;
    }
    if (night->source_revision != request.artifact.source_revision) {
        result.status = ReportPlanStatus::StaleRevision;
        return result;
    }

    const size_t session_count =
        requested_session_count(*catalog, *night, request.artifact);
    if (session_count == SIZE_MAX || session_count > UINT16_MAX) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    const size_t requested_signals = bit_count(request.signal_mask);
    if (requested_signals > 0 &&
        session_count > std::numeric_limits<size_t>::max() /
                            requested_signals) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }

    ScratchArray<SelectedSource> selected;
    if (!selected.allocate(session_count * requested_signals)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }
    if (!select_sources(request, *catalog, *night, selected)) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    size_t pending_count = 0;
    for (size_t i = 0; i < selected.size(); ++i) {
        if (!count_numeric_operations(*catalog,
                                      *night,
                                      selected.data()[i],
                                      pending_count)) {
            result.status = ReportPlanStatus::InvalidCatalog;
            return result;
        }
    }
    if (!count_event_operations(request, *catalog, *night, pending_count)) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    ScratchArray<PendingOperation> pending;
    if (!pending.allocate(pending_count)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        if (!append_numeric_operations(*catalog,
                                       *night,
                                       selected.data()[i],
                                       pending)) {
            result.status = ReportPlanStatus::InvalidCatalog;
            return result;
        }
    }
    if (!append_event_operations(request, *catalog, *night, pending) ||
        pending.size() != pending_count) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }
    if (pending.size() > 1) {
        std::sort(pending.data(),
                  pending.data() + pending.size(),
                  pending_less);
    }

    size_t operation_count = 0;
    size_t mapping_count = 0;
    count_final_entries(pending, operation_count, mapping_count);

    std::shared_ptr<ReportReadPlan> plan(
        new (std::nothrow) ReportReadPlan());
    if (!plan || !plan->allocate(session_count,
                                 operation_count,
                                 mapping_count)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }

    plan->catalog_ = std::move(catalog);
    plan->night_ = night;
    plan->key_ = request.artifact;
    plan->requested_signal_mask_ = request.signal_mask;
    plan->requested_event_mask_ = request.event_mask;
    if (!fill_sessions(request,
                       plan->catalog(),
                       plan->night(),
                       selected,
                       plan->sessions_,
                       plan->session_count_,
                       plan->missing_required_signal_mask_,
                       plan->missing_optional_signal_mask_,
                       plan->unavailable_signal_mask_,
                       plan->acquirable_signal_mask_,
                       plan->missing_event_mask_) ||
        !fill_operations(pending,
                         plan->operations_,
                         plan->operation_count_,
                         plan->mappings_,
                         plan->mapping_count_)) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    result.status = ReportPlanStatus::Ready;
    result.plan = std::move(plan);
    return result;
}

}  // namespace aircannect
