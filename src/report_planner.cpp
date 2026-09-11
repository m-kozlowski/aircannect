#include "report_planner.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

#include "board_report.h"
#include "checked_size.h"
#include "large_scratch_array.h"
#include "report_records.h"
#include "report_source_progress.h"
#include "storage_read_port.h"

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

bool same_float(float lhs, float rhs) {
    uint32_t lhs_bits = 0;
    uint32_t rhs_bits = 0;
    static_assert(sizeof(lhs_bits) == sizeof(lhs), "float must be 32-bit");
    memcpy(&lhs_bits, &lhs, sizeof(lhs_bits));
    memcpy(&rhs_bits, &rhs, sizeof(rhs_bits));
    return lhs_bits == rhs_bits;
}

bool same_scale(const EdfSignalScale &lhs, const EdfSignalScale &rhs) {
    return lhs.digital_min == rhs.digital_min &&
           lhs.digital_max == rhs.digital_max &&
           same_float(lhs.physical_min, rhs.physical_min) &&
           same_float(lhs.physical_max, rhs.physical_max) &&
           same_float(lhs.scale, rhs.scale) &&
           same_float(lhs.offset, rhs.offset);
}

bool same_path(const ReportSourceProgressEntry &lhs,
               const ReportSourceProgressEntry &rhs) {
    return lhs.path_length == rhs.path_length && lhs.path && rhs.path &&
           memcmp(lhs.path, rhs.path, lhs.path_length) == 0;
}

bool same_progress_base(const ReportSourceProgressEntry &lhs,
                        const ReportSourceProgressEntry &rhs) {
    return lhs.storage == rhs.storage && same_path(lhs, rhs) &&
           lhs.series.signal == rhs.series.signal &&
           lhs.series.source == rhs.series.source &&
           lhs.series.sample_interval_ms == rhs.series.sample_interval_ms &&
           lhs.series.primary == rhs.series.primary &&
           lhs.session_start_ms == rhs.session_start_ms &&
           lhs.mapping_start_ms == rhs.mapping_start_ms;
}

bool same_progress_layout(const ReportSourceProgressEntry &lhs,
                          const ReportSourceProgressEntry &rhs) {
    if (lhs.storage == ReportSourceProgressStorage::Fallback) {
        return lhs.fallback_payload_schema == rhs.fallback_payload_schema &&
               lhs.fallback_coverage_start_ms ==
                   rhs.fallback_coverage_start_ms;
    }

    return lhs.file_start_ms == rhs.file_start_ms &&
           lhs.file_header_size == rhs.file_header_size &&
           lhs.file_record_size == rhs.file_record_size &&
           lhs.file_record_duration_ms == rhs.file_record_duration_ms &&
           lhs.samples_per_record == rhs.samples_per_record &&
           lhs.byte_offset_in_record == rhs.byte_offset_in_record &&
           same_scale(lhs.scale, rhs.scale);
}

bool same_progress_key(const ReportSourceProgressEntry &lhs,
                       const ReportSourceProgressEntry &rhs) {
    return same_progress_base(lhs, rhs) &&
           same_progress_layout(lhs, rhs);
}

bool compatible_source(const ReportSourceProgressEntry &old_entry,
                       const ReportSourceProgressEntry &current) {
    if (!same_progress_base(old_entry, current) ||
        !same_progress_layout(old_entry, current) ||
        current.full_end_ms < old_entry.full_end_ms) {
        return false;
    }
    return true;
}

bool add_count(size_t &total, size_t amount) {
    return CheckedSize::add_to(total, amount);
}

NightCatalogTimeRange requested_window(const ReportPlanRequest &request,
                                       const NightCatalogTimeRange &session) {
    (void)request;
    return session;
}

size_t requested_session_count(const NightCatalog &catalog,
                               const NightCatalogRecord &night,
                               const ReportPlanRequest &request) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    if (catalog_session_count > 0 && !sessions) return SIZE_MAX;

    size_t count = 0;
    for (size_t i = 0; i < catalog_session_count; ++i) {
        if (requested_window(request, sessions[i]).valid()) ++count;
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

bool make_progress_entry(const ReportReadPlan &plan,
                         const ReportReadOperation &operation,
                         const ReportReadMapping &mapping,
                         ReportSourceProgressEntry &out) {
    if (operation.kind != ReportReadOperationKind::Numeric &&
        operation.kind != ReportReadOperationKind::FallbackSeries) {
        return false;
    }

    const ReportReadSession *session = plan.session(operation.session_index);
    const char *path = plan.source_path(operation);
    if (!session || !path || !mapping.output_window.valid()) return false;

    const size_t path_length = strlen(path);
    if (path_length == 0 || path_length > UINT16_MAX) return false;

    out = {};
    out.path = path;
    out.path_length = static_cast<uint16_t>(path_length);
    out.series = mapping.series;
    out.session_start_ms = session->output_window.start_ms;
    out.mapping_start_ms = mapping.output_window.start_ms;
    out.full_end_ms = mapping.output_window.end_ms;
    out.cursor_ms = out.mapping_start_ms;

    if (operation.kind == ReportReadOperationKind::Numeric) {
        const NightCatalogSourceFile *file = plan.source_file(operation);
        if (!file || !file_data_valid(*file)) return false;

        out.storage = ReportSourceProgressStorage::Edf;
        out.scale = mapping.layout.scale;
        out.samples_per_record = mapping.layout.samples_per_record;
        out.byte_offset_in_record = mapping.layout.byte_offset_in_record;
        out.file_start_ms = file->record_start_ms;
        out.file_header_size = file->header_size;
        out.file_record_size = file->record_size;
        out.file_record_duration_ms = file->record_duration_ms;
        return true;
    }

    const NightCatalogFallbackFile *file = plan.fallback_file(operation);
    const NightCatalogFallbackSection *section =
        plan.fallback_section(operation);
    if (!file || !section || section->kind != ReportFallbackSectionKind::Series ||
        section->data_size == 0 || section->record_count == 0 ||
        section->sample_interval_ms == 0) {
        return false;
    }

    out.storage = ReportSourceProgressStorage::Fallback;
    out.fallback_payload_schema = section->payload_schema;
    out.fallback_coverage_start_ms = section->coverage.start_ms;
    return true;
}

bool progress_cursor(const ReportSourceProgressEntry &entry,
                     int64_t closed_before_ms,
                     int64_t &out) {
    if (entry.full_end_ms <= entry.mapping_start_ms) return false;
    out = std::min(entry.full_end_ms,
                   std::max(entry.mapping_start_ms, closed_before_ms));
    return true;
}

bool processed_progress_overlap(
    const ReportSourceProgressEntry &old_entry,
    const ReportSourceProgressEntry &current) {
    if (old_entry.series.signal != current.series.signal ||
        old_entry.cursor_ms <= old_entry.mapping_start_ms) {
        return false;
    }

    const int64_t processed_end =
        std::min(old_entry.cursor_ms, old_entry.full_end_ms);
    return old_entry.mapping_start_ms < current.full_end_ms &&
           current.mapping_start_ms < processed_end;
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
            night_catalog_intersection(window, coverage[i].range);
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
                night_catalog_intersection(window, coverage[i].range);
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
            night_catalog_intersection(window, section.coverage);
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
                night_catalog_intersection(window, section.coverage);
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
                    LargeScratchArray<SelectedSource> &selected) {
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, catalog_session_count);
    if (catalog_session_count > 0 && !sessions) return false;

    uint16_t plan_session_index = 0;
    for (size_t catalog_session_index = 0;
         catalog_session_index < catalog_session_count;
         ++catalog_session_index) {
        const NightCatalogTimeRange window =
            requested_window(request, sessions[catalog_session_index]);
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

bool fallback_record_window(const NightCatalogFallbackSection &section,
                            const NightCatalogTimeRange &window,
                            uint32_t &first_record,
                            uint32_t &end_record) {
    first_record = 0;
    end_record = 0;
    if (section.sample_interval_ms == 0 || section.record_count == 0 ||
        !window.valid() || !section.coverage.valid() ||
        window.start_ms < section.coverage.start_ms ||
        window.end_ms > section.coverage.end_ms) {
        return false;
    }

    const uint64_t start_delta = static_cast<uint64_t>(
        window.start_ms - section.coverage.start_ms);
    const uint64_t end_delta = static_cast<uint64_t>(
        window.end_ms - section.coverage.start_ms);
    const uint64_t interval = section.sample_interval_ms;
    const uint64_t first_sample =
        (start_delta + interval - 1u) / interval;
    const uint64_t end_sample = (end_delta + interval - 1u) / interval;
    const uint32_t bounded_first = static_cast<uint32_t>(
        std::min<uint64_t>(section.record_count, first_sample));
    const uint32_t bounded_end = static_cast<uint32_t>(
        std::min<uint64_t>(section.record_count, end_sample));
    if (bounded_end <= bounded_first) return false;

    first_record = bounded_first;
    end_record = bounded_end;
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
            night_catalog_intersection(selected.output_window,
                                       coverage[i].range);
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
            !night_catalog_intersection(selected.output_window,
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
                    night_catalog_intersection(window, section.coverage);
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

    if ((definition->flags & REPORT_SIGNAL_NO_FALLBACK) != 0) return true;

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
        if (!requested_window(request, sessions[catalog_session]).valid()) {
            continue;
        }

        const NightCatalogTimeRange filter =
            requested_window(request, sessions[catalog_session]);
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
                    !night_catalog_intersection(filter,
                                                section.coverage).valid()) {
                    continue;
                }
                if (!add_count(count, 1)) return false;
            }
        }
    }
    return true;
}

bool append_record_operations(LargeScratchArray<PendingOperation> &pending,
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
    LargeScratchArray<PendingOperation> &pending) {
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
            night_catalog_intersection(selected.output_window,
                                       coverage[i].range);
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
    LargeScratchArray<PendingOperation> &pending) {
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
            night_catalog_intersection(selected.output_window,
                                       section.coverage);
        if (!mapping.output_window.valid()) continue;

        uint32_t bounded_first = 0;
        uint32_t bounded_end = 0;
        if (!fallback_record_window(section,
                                    mapping.output_window,
                                    bounded_first,
                                    bounded_end)) {
            continue;
        }

        if (i > UINT16_MAX || section.data_size == 0 ||
            section.data_size > AC_STORAGE_PREPARED_READ_MAX_BYTES) {
            return false;
        }
        mapping.series = selected.series;

        PendingOperation *entry = pending.append();
        if (!entry) return false;
        entry->operation.first_record = bounded_first;
        entry->operation.record_count = bounded_end - bounded_first;
        entry->operation.session_index = selected.session_index;
        entry->operation.catalog_file_index =
            selected.catalog_file_index;
        entry->operation.fallback_section_index =
            static_cast<uint16_t>(i);

        entry->operation.offset = section.data_offset;
        entry->operation.length = section.data_size;
        entry->operation.kind = ReportReadOperationKind::FallbackSeries;
        entry->mapping = mapping;
        entry->has_mapping = true;
    }
    return true;
}

bool append_numeric_operations(const NightCatalog &catalog,
                               const NightCatalogRecord &night,
                               const SelectedSource &selected,
                               LargeScratchArray<PendingOperation> &pending) {
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
                             LargeScratchArray<PendingOperation> &pending) {
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
            requested_window(request, sessions[catalog_session]);
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
                        !night_catalog_intersection(
                            filter, section.coverage).valid() ||
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

void count_final_entries(const LargeScratchArray<PendingOperation> &pending,
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
                   const LargeScratchArray<SelectedSource> &selected,
                   ReportReadSession *plan_sessions,
                   size_t plan_session_count,
                   bool fallback_acquisition_allowed,
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
            requested_window(request, sessions[catalog_session]);
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
        if (!fallback_acquisition_allowed) {
            output.unavailable_signal_mask = output.missing_signal_mask;
        } else {
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
        if (fallback_acquisition_allowed) {
            acquirable_signals |= output.missing_signal_mask &
                                  ~output.unavailable_signal_mask;
        }
        missing_events |= output.missing_event_mask;
        ++plan_session;
    }
    return plan_session == plan_session_count;
}

bool fill_operations(const LargeScratchArray<PendingOperation> &pending,
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

bool find_progress_entry(const ReportSourceProgressReader &reader,
                         const ReportSourceProgressEntry &needle,
                         size_t &index,
                         ReportSourceProgressEntry &out) {
    for (size_t i = 0; i < reader.count(); ++i) {
        ReportSourceProgressEntry candidate;
        if (!reader.entry(i, candidate)) return false;
        if (same_progress_key(candidate, needle)) {
            index = i;
            out = candidate;
            return true;
        }
    }
    index = SIZE_MAX;
    return true;
}

bool collect_current_progress_entries(
    const ReportReadPlan &plan,
    LargeScratchArray<ReportSourceProgressEntry> &entries) {
    if (!entries.allocate(plan.mapping_count())) return false;

    for (size_t operation_index = 0;
         operation_index < plan.operation_count();
         ++operation_index) {
        const ReportReadOperation *operation = plan.operation(operation_index);
        if (!operation ||
            (operation->kind != ReportReadOperationKind::Numeric &&
             operation->kind != ReportReadOperationKind::FallbackSeries)) {
            continue;
        }

        size_t mapping_count = 0;
        const ReportReadMapping *mappings =
            plan.mappings(*operation, mapping_count);
        if (!mappings || mapping_count == 0) return false;
        for (size_t mapping_index = 0;
             mapping_index < mapping_count;
             ++mapping_index) {
            ReportSourceProgressEntry current;
            if (!make_progress_entry(plan,
                                     *operation,
                                     mappings[mapping_index],
                                     current)) {
                return false;
            }

            bool duplicate = false;
            for (size_t i = 0; i < entries.size(); ++i) {
                const ReportSourceProgressEntry &known = entries.data()[i];
                if (same_progress_key(known, current)) {
                    duplicate = true;
                    break;
                }
                if (same_progress_base(known, current)) return false;
            }
            if (!duplicate && !entries.append()) return false;
            if (!duplicate) entries.data()[entries.size() - 1] = current;
        }
    }
    return true;
}

bool append_resumed_numeric(
    const ReportReadPlan &plan,
    const ReportReadOperation &operation,
    const ReportSourceProgressReader &reader,
    LargeScratchArray<PendingOperation> &pending) {
    const NightCatalogSourceFile *file = plan.source_file(operation);
    if (!file || !file_data_valid(*file)) return false;

    size_t mapping_count = 0;
    const ReportReadMapping *mappings =
        plan.mappings(operation, mapping_count);
    if (!mappings || mapping_count == 0 ||
        mapping_count > static_cast<size_t>(ReportSignalId::Count)) {
        return false;
    }
    if (operation.first_record > file->complete_records ||
        operation.record_count >
            file->complete_records - operation.first_record) {
        return false;
    }
    const uint32_t operation_end =
        operation.first_record + operation.record_count;

    LargeScratchArray<ReportReadMapping> remaining;
    if (!remaining.allocate(mapping_count)) return false;
    uint32_t first_record = UINT32_MAX;
    uint32_t end_record = 0;
    for (size_t i = 0; i < mapping_count; ++i) {
        ReportReadMapping mapping = mappings[i];
        ReportSourceProgressEntry current;
        if (!make_progress_entry(plan, operation, mapping, current)) {
            return false;
        }

        size_t progress_index = SIZE_MAX;
        ReportSourceProgressEntry previous;
        if (!find_progress_entry(reader,
                                  current,
                                  progress_index,
                                  previous)) {
            return false;
        }
        if (progress_index != SIZE_MAX) {
            if (!compatible_source(previous, current)) return false;
            mapping.output_window.start_ms = std::max(
                mapping.output_window.start_ms, previous.cursor_ms);
            if (mapping.output_window.start_ms >=
                mapping.output_window.end_ms) {
                continue;
            }
        }

        uint32_t mapping_first = 0;
        uint32_t mapping_end = 0;
        if (!record_window(*file,
                           mapping.output_window,
                           mapping_first,
                           mapping_end)) {
            continue;
        }
        mapping_first = std::max(mapping_first, operation.first_record);
        mapping_end = std::min(mapping_end, operation_end);
        if (mapping_end <= mapping_first) continue;
        if (mapping_first < first_record) first_record = mapping_first;
        if (mapping_end > end_record) end_record = mapping_end;
        ReportReadMapping *remaining_mapping = remaining.append();
        if (!remaining_mapping) return false;
        *remaining_mapping = mapping;
    }
    if (remaining.size() == 0 || first_record == UINT32_MAX ||
        end_record <= first_record) {
        return true;
    }

    ReportReadOperation resumed = operation;
    resumed.first_record = first_record;
    resumed.record_count = end_record - first_record;
    if (!operation_span(*file,
                        resumed.first_record,
                        resumed.record_count,
                        resumed.offset,
                        resumed.length)) {
        return false;
    }
    for (size_t i = 0; i < remaining.size(); ++i) {
        PendingOperation *entry = pending.append();
        if (!entry) return false;
        entry->operation = resumed;
        entry->mapping = remaining.data()[i];
        entry->has_mapping = true;
    }
    return true;
}

bool append_resumed_fallback_series(
    const ReportReadPlan &plan,
    const ReportReadOperation &operation,
    const ReportSourceProgressReader &reader,
    LargeScratchArray<PendingOperation> &pending) {
    size_t mapping_count = 0;
    const ReportReadMapping *mappings =
        plan.mappings(operation, mapping_count);
    const NightCatalogFallbackSection *section =
        plan.fallback_section(operation);
    if (!mappings || mapping_count != 1 || !section ||
        section->kind != ReportFallbackSectionKind::Series ||
        section->data_size == 0) {
        return false;
    }

    ReportReadMapping mapping = mappings[0];
    ReportSourceProgressEntry current;
    if (!make_progress_entry(plan, operation, mapping, current)) return false;

    size_t progress_index = SIZE_MAX;
    ReportSourceProgressEntry previous;
    if (!find_progress_entry(reader,
                             current,
                             progress_index,
                             previous)) {
        return false;
    }
    if (progress_index != SIZE_MAX) {
        if (!compatible_source(previous, current)) return false;
        mapping.output_window.start_ms = std::max(
            mapping.output_window.start_ms, previous.cursor_ms);
        if (mapping.output_window.start_ms >= mapping.output_window.end_ms) {
            return true;
        }
    }

    uint32_t first_record = 0;
    uint32_t end_record = 0;
    if (!fallback_record_window(*section,
                                mapping.output_window,
                                first_record,
                                end_record)) {
        return true;
    }

    ReportReadOperation resumed = operation;
    resumed.first_record = first_record;
    resumed.record_count = end_record - first_record;
    PendingOperation *entry = pending.append();
    if (!entry) return false;
    entry->operation = resumed;
    entry->mapping = mapping;
    entry->has_mapping = true;
    return true;
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

    if (night->sources_external) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    const size_t session_count =
        requested_session_count(*catalog, *night, request);
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

    LargeScratchArray<SelectedSource> selected;
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

    LargeScratchArray<PendingOperation> pending;
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
    plan->fallback_acquisition_allowed_ =
        (night->source_flags & NIGHT_CATALOG_SOURCE_EDF) == 0;
    if (!fill_sessions(request,
                       plan->catalog(),
                       plan->night(),
                       selected,
                       plan->sessions_,
                       plan->session_count_,
                       plan->fallback_acquisition_allowed_,
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

std::shared_ptr<const LargeByteBuffer> ReportPlanner::capture_progress(
    const ReportReadPlan &full,
    int64_t closed_before_ms,
    const uint8_t *previous,
    size_t previous_length) {
    ReportSourceProgressReader previous_reader;
    if (previous_length > 0 &&
        (!previous || !previous_reader.open(previous, previous_length))) {
        return {};
    }

    LargeScratchArray<ReportSourceProgressEntry> current;
    if (!collect_current_progress_entries(full, current)) return {};

    size_t capacity = 0;
    if (!CheckedSize::add(previous_reader.count(), current.size(), capacity)) {
        return {};
    }
    LargeScratchArray<ReportSourceProgressEntry> merged;
    if (!merged.allocate(capacity)) return {};

    const size_t old_count = previous_reader.count();
    for (size_t i = 0; i < old_count; ++i) {
        ReportSourceProgressEntry retained;
        if (!previous_reader.entry(i, retained)) return {};

        int64_t promoted_cursor = 0;
        if (!progress_cursor(retained,
                             closed_before_ms,
                             promoted_cursor)) {
            return {};
        }
        retained.cursor_ms = std::max(retained.cursor_ms, promoted_cursor);
        ReportSourceProgressEntry *slot = merged.append();
        if (!slot) return {};
        *slot = retained;
    }

    for (size_t i = 0; i < current.size(); ++i) {
        ReportSourceProgressEntry next = current.data()[i];
        if (!progress_cursor(next, closed_before_ms, next.cursor_ms)) {
            return {};
        }

        size_t matched_index = SIZE_MAX;
        ReportSourceProgressEntry matched;
        for (size_t j = 0; j < merged.size(); ++j) {
            if (same_progress_key(merged.data()[j], next)) {
                matched_index = j;
                matched = merged.data()[j];
                break;
            }
        }
        if (matched_index != SIZE_MAX) {
            if (matched_index < old_count) {
                if (!compatible_source(matched, next)) return {};
                next.cursor_ms = std::max(next.cursor_ms,
                                          matched.cursor_ms);
                merged.data()[matched_index] = next;
            }
            continue;
        }

        ReportSourceProgressEntry *slot = merged.append();
        if (!slot) return {};
        *slot = next;
    }

    return encode_report_source_progress(merged.data(), merged.size());
}

ReportPlanResult ReportPlanner::resume(
    std::shared_ptr<const ReportReadPlan> full,
    const uint8_t *progress,
    size_t length) {
    ReportPlanResult result;
    if (!full) {
        result.status = ReportPlanStatus::InvalidRequest;
        return result;
    }

    ReportSourceProgressReader reader;
    if (length > 0 && (!progress || !reader.open(progress, length))) {
        result.status = ReportPlanStatus::InvalidRequest;
        return result;
    }

    LargeScratchArray<ReportSourceProgressEntry> current;
    if (!collect_current_progress_entries(*full, current)) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }

    for (size_t i = 0; i < reader.count(); ++i) {
        ReportSourceProgressEntry old_entry;
        if (!reader.entry(i, old_entry)) {
            result.status = ReportPlanStatus::InvalidRequest;
            return result;
        }
        for (size_t j = 0; j < i; ++j) {
            ReportSourceProgressEntry earlier;
            if (!reader.entry(j, earlier)) {
                result.status = ReportPlanStatus::InvalidRequest;
                return result;
            }
            if (same_progress_key(earlier, old_entry)) {
                result.status = ReportPlanStatus::InvalidRequest;
                return result;
            }
        }

        bool has_base_match = false;
        for (size_t j = 0; j < current.size(); ++j) {
            const ReportSourceProgressEntry &candidate = current.data()[j];
            if (same_progress_base(old_entry, candidate)) {
                has_base_match = true;
                if (!compatible_source(old_entry, candidate)) {
                    result.status = ReportPlanStatus::InvalidCatalog;
                    return result;
                }
            }
        }
        if (has_base_match) continue;

        for (size_t j = 0; j < current.size(); ++j) {
            if (processed_progress_overlap(old_entry, current.data()[j])) {
                result.status = ReportPlanStatus::InvalidCatalog;
                return result;
            }

            // A saved per-track tail cannot be split between a missing
            // source and another source still providing that signal.
            if (old_entry.cursor_ms < old_entry.full_end_ms &&
                old_entry.series.signal == current.data()[j].series.signal) {
                result.status = ReportPlanStatus::InvalidRequest;
                return result;
            }
        }
    }

    size_t pending_capacity = 0;
    if (!CheckedSize::add(full->mapping_count(),
                          full->operation_count(),
                          pending_capacity)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }
    LargeScratchArray<PendingOperation> pending;
    if (!pending.allocate(pending_capacity)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }

    for (size_t operation_index = 0;
         operation_index < full->operation_count();
         ++operation_index) {
        const ReportReadOperation *operation = full->operation(operation_index);
        if (!operation) {
            result.status = ReportPlanStatus::InvalidCatalog;
            return result;
        }

        if (operation->kind == ReportReadOperationKind::Numeric) {
            if (!append_resumed_numeric(*full,
                                        *operation,
                                        reader,
                                        pending)) {
                result.status = ReportPlanStatus::InvalidCatalog;
                return result;
            }
        } else if (operation->kind == ReportReadOperationKind::FallbackSeries) {
            if (!append_resumed_fallback_series(*full,
                                                *operation,
                                                reader,
                                                pending)) {
                result.status = ReportPlanStatus::InvalidCatalog;
                return result;
            }
        } else if (operation->kind == ReportReadOperationKind::ScoredEvents ||
                   operation->kind == ReportReadOperationKind::CsrEvents ||
                   operation->kind == ReportReadOperationKind::FallbackEvents) {
            if (operation->mapping_count != 0) {
                result.status = ReportPlanStatus::InvalidCatalog;
                return result;
            }
            PendingOperation *entry = pending.append();
            if (!entry) {
                result.status = ReportPlanStatus::AllocationFailed;
                return result;
            }
            entry->operation = *operation;
            entry->has_mapping = false;
        } else {
            result.status = ReportPlanStatus::InvalidCatalog;
            return result;
        }
    }

    size_t operation_count = 0;
    size_t mapping_count = 0;
    count_final_entries(pending, operation_count, mapping_count);

    std::shared_ptr<ReportReadPlan> resumed_plan(
        new (std::nothrow) ReportReadPlan());
    if (!resumed_plan || !resumed_plan->allocate(full->session_count(),
                                                 operation_count,
                                                 mapping_count)) {
        result.status = ReportPlanStatus::AllocationFailed;
        return result;
    }

    resumed_plan->catalog_ = full->catalog_;
    resumed_plan->night_ = full->night_;
    resumed_plan->key_ = full->key_;
    resumed_plan->requested_signal_mask_ = full->requested_signal_mask_;
    resumed_plan->missing_required_signal_mask_ =
        full->missing_required_signal_mask_;
    resumed_plan->missing_optional_signal_mask_ =
        full->missing_optional_signal_mask_;
    resumed_plan->unavailable_signal_mask_ = full->unavailable_signal_mask_;
    resumed_plan->acquirable_signal_mask_ = full->acquirable_signal_mask_;
    resumed_plan->fallback_acquisition_allowed_ =
        full->fallback_acquisition_allowed_;
    resumed_plan->requested_event_mask_ = full->requested_event_mask_;
    resumed_plan->missing_event_mask_ = full->missing_event_mask_;
    for (size_t i = 0; i < full->session_count(); ++i) {
        const ReportReadSession *session = full->session(i);
        if (!session) {
            result.status = ReportPlanStatus::InvalidCatalog;
            return result;
        }
        resumed_plan->sessions_[i] = *session;
    }

    if (!fill_operations(pending,
                         resumed_plan->operations_,
                         resumed_plan->operation_count_,
                         resumed_plan->mappings_,
                         resumed_plan->mapping_count_)) {
        result.status = ReportPlanStatus::InvalidCatalog;
        return result;
    }
    result.status = ReportPlanStatus::Ready;
    result.plan = std::move(resumed_plan);
    return result;
}

}  // namespace aircannect
