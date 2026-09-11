#include "night_catalog_capture.h"

#include <algorithm>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "edf_layout.h"
#include "edf_report_catalog.h"
#include "large_byte_buffer.h"
#include "large_object.h"
#include "large_scratch_array.h"
#include "night_catalog_builder.h"
#include "report_sources.h"

namespace aircannect {
namespace {

constexpr int64_t CAPTURE_QUARTER_MS = 15LL * 60LL * 1000LL;
constexpr int64_t CAPTURE_DAY_MS = 24LL * 60LL * 60LL * 1000LL;
constexpr int64_t CAPTURE_NOON_MS = 12LL * 60LL * 60LL * 1000LL;
constexpr int64_t CAPTURE_MINUTE_MS = 60LL * 1000LL;

struct CaptureFiles {
    NightCatalogSourceFileInput files[AC_EDF_REPORT_SESSION_FILE_MAX] = {};
    EdfReportSignalLayout layouts[AC_EDF_REPORT_SESSION_FILE_MAX]
        [AC_EDF_REPORT_FILE_SIGNAL_MAX] = {};
    size_t file_count = 0;
    int64_t earliest_start_ms = 0;
    int64_t latest_brp_end_ms = 0;
    int64_t latest_pld_end_ms = 0;
    int64_t latest_primary_end_ms = 0;
};

using CaptureFilesPtr = std::unique_ptr<
    CaptureFiles,
    decltype(&LargeObject::destroy<CaptureFiles>)>;

CaptureFilesPtr make_capture_files() {
    return CaptureFilesPtr(LargeObject::create<CaptureFiles>(),
                           &LargeObject::destroy<CaptureFiles>);
}

struct CaptureWorkspace {
    LargeScratchArray<NightCatalogEdfSessionInput> sessions;
    LargeScratchArray<NightCatalogSourceFileInput> files;
    LargeScratchArray<NightCatalogFallbackInput> fallbacks;
    LargeScratchArray<NightCatalogFallbackSectionInput> fallback_sections;
    LargeScratchArray<NightCatalogTimeRange> fallback_sessions;
};

bool subtract_offset(int64_t local_ms,
                     int32_t timezone_offset_minutes,
                     int64_t &utc_ms) {
    const int64_t offset_ms = static_cast<int64_t>(
        timezone_offset_minutes) * CAPTURE_MINUTE_MS;
    if ((offset_ms > 0 && local_ms < INT64_MIN + offset_ms) ||
        (offset_ms < 0 && local_ms > INT64_MAX + offset_ms)) {
        return false;
    }

    utc_ms = local_ms - offset_ms;
    return utc_ms > 0;
}

bool day_noon(SleepDayId sleep_day,
              int32_t timezone_offset_minutes,
              int64_t &utc_ms) {
    if (!sleep_day.valid()) return false;

    const int64_t days = sleep_day.epoch_days();
    if (days > (INT64_MAX - CAPTURE_NOON_MS) / CAPTURE_DAY_MS ||
        days < (INT64_MIN + CAPTURE_NOON_MS) / CAPTURE_DAY_MS) {
        return false;
    }

    const int64_t local_ms = days * CAPTURE_DAY_MS + CAPTURE_NOON_MS;
    return subtract_offset(local_ms, timezone_offset_minutes, utc_ms);
}

bool day_boundaries(SleepDayId sleep_day,
                    int32_t timezone_offset_minutes,
                    int64_t &start_ms,
                    int64_t &end_ms) {
    if (!day_noon(sleep_day, timezone_offset_minutes, start_ms) ||
        start_ms > INT64_MAX - CAPTURE_DAY_MS) {
        return false;
    }

    end_ms = start_ms + CAPTURE_DAY_MS;
    return true;
}

bool session_prefix(const EdfSessionMetadata &metadata,
                    char *out,
                    size_t out_size,
                    size_t &length) {
    const int written = snprintf(out,
                                 out_size,
                                 "/DATALOG/%s/%s_",
                                 metadata.datalog_sleep_day,
                                 metadata.session_stamp);
    if (written <= 0 || static_cast<size_t>(written) >= out_size) {
        return false;
    }

    length = static_cast<size_t>(written);
    return true;
}

bool matching_progress_file(const EdfStorageProgressFile &progress_file,
                            const char *prefix,
                            size_t prefix_length) {
    if (!progress_file.path[0] ||
        strncmp(progress_file.path, prefix, prefix_length) != 0) {
        return false;
    }

    const char *suffix = progress_file.path + prefix_length;
    return strcmp(suffix, "BRP.edf") == 0 ||
           strcmp(suffix, "PLD.edf") == 0 ||
           strcmp(suffix, "SA2.edf") == 0 ||
           strcmp(suffix, "TCV.edf") == 0 ||
           strcmp(suffix, "EVE.edf") == 0 ||
           strcmp(suffix, "CSL.edf") == 0;
}

bool normalized_header(const EdfStorageProgressFile &progress_file,
                       std::unique_ptr<LargeByteBuffer> &out) {
    if (!progress_file.header || progress_file.header_size == 0 ||
        progress_file.header->size() < progress_file.header_size ||
        progress_file.header->size() < AC_EDF_HEADER_RECORD_COUNT_OFFSET +
            AC_EDF_HEADER_RECORD_COUNT_WIDTH) {
        return false;
    }

    out = LargeByteBuffer::allocate(progress_file.header->size());
    if (!out) return false;
    memcpy(out->data(),
           progress_file.header->data(),
           progress_file.header->size());

    char record_count[AC_EDF_HEADER_RECORD_COUNT_WIDTH + 1] = {};
    const int written = snprintf(record_count,
                                 sizeof(record_count),
                                 "%*u",
                                 static_cast<int>(
                                     AC_EDF_HEADER_RECORD_COUNT_WIDTH),
                                 static_cast<unsigned>(
                                     progress_file.record_count));
    if (written != static_cast<int>(AC_EDF_HEADER_RECORD_COUNT_WIDTH)) {
        return false;
    }
    memcpy(out->data() + AC_EDF_HEADER_RECORD_COUNT_OFFSET,
           record_count,
           AC_EDF_HEADER_RECORD_COUNT_WIDTH);
    return true;
}

bool add_duration(int64_t start_ms,
                  size_t record_count,
                  uint32_t record_duration_ms,
                  int64_t &end_ms) {
    if (start_ms <= 0 || record_duration_ms == 0 ||
        record_count > UINT64_MAX / record_duration_ms) {
        return false;
    }

    const uint64_t duration_ms = static_cast<uint64_t>(record_count) *
        record_duration_ms;
    if (duration_ms > static_cast<uint64_t>(INT64_MAX - start_ms)) {
        return false;
    }

    end_ms = start_ms + static_cast<int64_t>(duration_ms);
    return end_ms > start_ms;
}

bool records_through(int64_t start_ms,
                     uint32_t record_duration_ms,
                     int64_t publication_end_ms,
                     size_t &record_count) {
    record_count = 0;
    if (publication_end_ms <= start_ms || record_duration_ms == 0) {
        return false;
    }

    const uint64_t elapsed = static_cast<uint64_t>(
        publication_end_ms - start_ms);
    const uint64_t duration = record_duration_ms;
    const uint64_t count = elapsed / duration + (elapsed % duration != 0);
    if (count > SIZE_MAX) return false;
    record_count = static_cast<size_t>(count);
    return record_count > 0;
}

bool map_file_kind(EdfInventoryFileKind source, NightCatalogFileKind &out) {
    switch (source) {
        case EdfInventoryFileKind::Brp:
            out = NightCatalogFileKind::Brp;
            return true;
        case EdfInventoryFileKind::Pld:
            out = NightCatalogFileKind::Pld;
            return true;
        case EdfInventoryFileKind::Sa2:
            out = NightCatalogFileKind::Sa2;
            return true;
        case EdfInventoryFileKind::Tcv:
            out = NightCatalogFileKind::Tcv;
            return true;
        case EdfInventoryFileKind::Eve:
            out = NightCatalogFileKind::Eve;
            return true;
        case EdfInventoryFileKind::Csl:
            out = NightCatalogFileKind::Csl;
            return true;
        default:
            return false;
    }
}

void signal_masks(const EdfReportSignalLayout *layouts,
                  size_t layout_count,
                  uint32_t &primary_mask,
                  uint32_t &fallback_mask) {
    primary_mask = 0;
    fallback_mask = 0;
    for (size_t i = 0; i < layout_count; ++i) {
        const uint32_t bit = report_signal_bit(layouts[i].signal);
        if (layouts[i].primary) {
            primary_mask |= bit;
        } else {
            fallback_mask |= bit;
        }
    }
}

bool numeric_kind(NightCatalogFileKind kind) {
    return kind == NightCatalogFileKind::Brp ||
           kind == NightCatalogFileKind::Pld ||
           kind == NightCatalogFileKind::Sa2 ||
           kind == NightCatalogFileKind::Tcv;
}

bool bounds_file_kind(NightCatalogFileKind kind, uint8_t &group) {
    switch (kind) {
        case NightCatalogFileKind::Brp:
            group = 0;
            return true;
        case NightCatalogFileKind::Pld:
        case NightCatalogFileKind::Sa2:
        case NightCatalogFileKind::Tcv:
            group = 1;
            return true;
        case NightCatalogFileKind::Eve:
        case NightCatalogFileKind::Csl:
            group = 2;
            return true;
        default:
            return false;
    }
}

bool session_bounds_start(const NightCatalogSourceFileInput *files,
                          size_t file_count,
                          int64_t &start_ms) {
    uint8_t selected_group = UINT8_MAX;
    for (size_t i = 0; i < file_count; ++i) {
        uint8_t group = 0;
        if (!bounds_file_kind(files[i].kind, group)) continue;
        if (selected_group == UINT8_MAX || group < selected_group) {
            selected_group = group;
        }
    }
    if (selected_group == UINT8_MAX) return false;

    start_ms = 0;
    for (size_t i = 0; i < file_count; ++i) {
        uint8_t group = 0;
        if (!bounds_file_kind(files[i].kind, group) ||
            group != selected_group) {
            continue;
        }
        const int64_t candidate = files[i].coverage.range.start_ms;
        if (candidate <= 0 || (start_ms != 0 && candidate >= start_ms)) {
            continue;
        }
        start_ms = candidate;
    }
    return start_ms > 0;
}

bool capture_files(const EdfSessionMetadata &metadata,
                   const EdfStorageProgress &progress,
                   CaptureFiles &out) {
    out.file_count = 0;
    out.earliest_start_ms = 0;
    out.latest_brp_end_ms = 0;
    out.latest_pld_end_ms = 0;
    out.latest_primary_end_ms = 0;

    char prefix[AC_STORAGE_PATH_MAX] = {};
    size_t prefix_length = 0;
    if (!edf_session_metadata_valid(metadata) ||
        !session_prefix(metadata,
                        prefix,
                        sizeof(prefix),
                        prefix_length)) {
        return false;
    }

    for (size_t i = 0; i < AC_EDF_STORAGE_PROGRESS_FILE_COUNT; ++i) {
        const EdfStorageProgressFile &progress_file = progress.files[i];
        if (!matching_progress_file(progress_file, prefix, prefix_length) ||
            progress_file.byte_size < progress_file.header_size ||
            progress_file.record_size == 0 ||
            out.file_count >= AC_EDF_REPORT_SESSION_FILE_MAX) {
            continue;
        }

        std::unique_ptr<LargeByteBuffer> header;
        if (!normalized_header(progress_file, header)) continue;

        EdfReportFileDescriptor described;
        if (edf_report_describe_file(progress_file.path,
                                     header->data(),
                                     header->size(),
                                     progress_file.byte_size,
                                     0,
                                     metadata.timezone_offset_minutes,
                                     described) != EdfReportFileStatus::Ok ||
            described.record_size != progress_file.record_size) {
            continue;
        }

        NightCatalogFileKind kind;
        if (!map_file_kind(described.inventory.kind, kind)) continue;

        const size_t available_records = std::min(
            static_cast<size_t>(progress_file.record_count),
            described.inventory.complete_records_from_size);
        const size_t successful_records = available_records;
        if (successful_records == 0) continue;

        const bool annotation = kind == NightCatalogFileKind::Eve ||
            kind == NightCatalogFileKind::Csl;

        EdfReportSignalLayout *layouts = out.layouts[out.file_count];
        size_t layout_count = 0;
        if (!edf_report_file_signal_layouts(described,
                                            layouts,
                                            AC_EDF_REPORT_FILE_SIGNAL_MAX,
                                            layout_count)) {
            continue;
        }

        int64_t complete_end_ms = described.header_end_ms;
        if (!annotation &&
            !add_duration(described.header_start_ms,
                          successful_records,
                          described.record_duration_ms,
                          complete_end_ms)) {
            continue;
        }

        uint32_t primary_mask = 0;
        uint32_t fallback_mask = 0;
        signal_masks(layouts,
                     layout_count,
                     primary_mask,
                     fallback_mask);
        if (numeric_kind(kind) && primary_mask == 0) continue;

        const uint64_t data_size = static_cast<uint64_t>(successful_records) *
            described.record_size;
        if (data_size > UINT64_MAX - described.header_size) continue;

        if (out.earliest_start_ms == 0 ||
            described.header_start_ms < out.earliest_start_ms) {
            out.earliest_start_ms = described.header_start_ms;
        }

        NightCatalogSourceFileInput &file = out.files[out.file_count++];
        file.kind = kind;
        file.path = progress_file.path;
        file.coverage.range = {
            described.header_start_ms,
            annotation && described.header_end_ms < described.header_start_ms
                ? described.header_start_ms
                : complete_end_ms,
        };
        file.coverage.primary_signal_mask = primary_mask;
        file.coverage.fallback_signal_mask = fallback_mask;
        file.file_size = static_cast<uint64_t>(described.header_size) +
            data_size;
        file.last_write_ms = 0;
        file.data_offset = described.header_size;
        file.data_size = data_size;
        file.identity = 0;
        file.record_start_ms = described.header_start_ms;
        file.header_size = described.header_size;
        file.record_size = described.record_size;
        file.record_duration_ms = described.record_duration_ms;
        file.complete_records = static_cast<uint32_t>(successful_records);
        file.signal_layouts = layouts;
        file.signal_layout_count = layout_count;

        if (kind == NightCatalogFileKind::Brp &&
            complete_end_ms > out.latest_brp_end_ms) {
            out.latest_brp_end_ms = complete_end_ms;
        } else if (kind == NightCatalogFileKind::Pld &&
                   complete_end_ms > out.latest_pld_end_ms) {
            out.latest_pld_end_ms = complete_end_ms;
        }
    }

    out.latest_primary_end_ms = out.latest_brp_end_ms > 0
        ? out.latest_brp_end_ms
        : out.latest_pld_end_ms;
    return true;
}

int64_t publication_boundary(int64_t primary_end_ms) {
    if (primary_end_ms <= 0) return 0;
    return primary_end_ms - primary_end_ms % CAPTURE_QUARTER_MS;
}

int64_t closed_end_for_capture(const EdfSessionMetadata &metadata,
                               const CaptureFiles &files) {
    int64_t primary_end_ms = files.latest_primary_end_ms;
    if (metadata.finalized && metadata.canonical_segment_end_ms > 0) {
        primary_end_ms = std::min(primary_end_ms,
                                  metadata.canonical_segment_end_ms);
    }
    return publication_boundary(primary_end_ms);
}

bool clip_capture_files(CaptureFiles &files, int64_t publication_end_ms) {
    if (publication_end_ms <= 0) return false;

    size_t next_file = 0;
    files.earliest_start_ms = 0;
    files.latest_brp_end_ms = 0;
    files.latest_pld_end_ms = 0;

    for (size_t i = 0; i < files.file_count; ++i) {
        NightCatalogSourceFileInput file = files.files[i];
        const bool annotation = file.kind == NightCatalogFileKind::Eve ||
            file.kind == NightCatalogFileKind::Csl;
        size_t retained_records = file.complete_records;
        if (!annotation) {
            size_t records_to_retain = 0;
            if (!records_through(file.record_start_ms,
                                 file.record_duration_ms,
                                 publication_end_ms,
                                 records_to_retain)) {
                continue;
            }
            retained_records = std::min(retained_records,
                                        records_to_retain);
        }
        if (retained_records == 0) continue;

        int64_t complete_end_ms = file.coverage.range.end_ms;
        if (!annotation &&
            !add_duration(file.record_start_ms,
                          retained_records,
                          file.record_duration_ms,
                          complete_end_ms)) {
            continue;
        }

        const uint64_t data_size = static_cast<uint64_t>(retained_records) *
            file.record_size;
        if (data_size > UINT64_MAX - file.header_size) continue;

        file.complete_records = static_cast<uint32_t>(retained_records);
        file.data_size = data_size;
        file.file_size = static_cast<uint64_t>(file.header_size) + data_size;
        if (!annotation) {
            file.coverage.range.end_ms = std::min(complete_end_ms,
                                                  publication_end_ms);
            if (!file.coverage.range.valid()) continue;
        } else if (file.coverage.range.end_ms <
                   file.coverage.range.start_ms) {
            continue;
        }

        files.files[next_file++] = file;
        if (files.earliest_start_ms == 0 ||
            file.record_start_ms < files.earliest_start_ms) {
            files.earliest_start_ms = file.record_start_ms;
        }
        if (file.kind == NightCatalogFileKind::Brp &&
            file.coverage.range.end_ms > files.latest_brp_end_ms) {
            files.latest_brp_end_ms = file.coverage.range.end_ms;
        } else if (file.kind == NightCatalogFileKind::Pld &&
                   file.coverage.range.end_ms > files.latest_pld_end_ms) {
            files.latest_pld_end_ms = file.coverage.range.end_ms;
        }
    }

    files.file_count = next_file;
    files.latest_primary_end_ms = files.latest_brp_end_ms > 0
        ? files.latest_brp_end_ms
        : files.latest_pld_end_ms;
    return files.file_count > 0 && files.earliest_start_ms > 0;
}

bool current_path(const CaptureFiles &current, const char *path) {
    if (!path) return false;
    for (size_t i = 0; i < current.file_count; ++i) {
        if (strcmp(current.files[i].path, path) == 0) return true;
    }
    return false;
}

bool append_existing_file(const NightCatalog &catalog,
                          const NightCatalogSourceFile &source,
                          CaptureWorkspace &workspace) {
    const char *path = catalog.path(source);
    size_t coverage_count = 0;
    const NightCatalogSourceCoverage *coverage =
        catalog.coverage(source, coverage_count);
    size_t layout_count = 0;
    const EdfReportSignalLayout *layouts =
        catalog.signal_layouts(source, layout_count);
    if (!path || coverage_count != 1 || !coverage ||
        layout_count != source.signal_layout_count ||
        (layout_count > 0 && !layouts)) {
        return false;
    }

    NightCatalogSourceFileInput *file = workspace.files.append();
    if (!file) return false;
    file->kind = source.kind;
    file->path = path;
    file->coverage = coverage[0];
    file->file_size = source.file_size;
    file->last_write_ms = source.last_write_ms;
    file->data_offset = source.data_offset;
    file->data_size = source.data_size;
    file->identity = source.identity;
    file->record_start_ms = source.record_start_ms;
    file->header_size = source.header_size;
    file->record_size = source.record_size;
    file->record_duration_ms = source.record_duration_ms;
    file->complete_records = source.complete_records;
    file->signal_layouts = layouts;
    file->signal_layout_count = layout_count;
    return true;
}

bool append_current_file(const NightCatalogSourceFileInput &source,
                         CaptureWorkspace &workspace) {
    NightCatalogSourceFileInput *file = workspace.files.append();
    if (!file) return false;
    *file = source;
    return true;
}

bool append_existing_fallback(const NightCatalog &catalog,
                              const NightCatalogRecord &record,
                              const NightCatalogFallbackFile &source,
                              const NightCatalogTimeRange *sessions,
                              size_t session_count,
                              CaptureWorkspace &workspace) {
    const char *path = catalog.path(source);
    size_t section_count = 0;
    const NightCatalogFallbackSection *sections =
        catalog.fallback_sections(source, section_count);
    if (!path || !path[0] || source.identity == 0 ||
        section_count != source.section_count || section_count == 0 ||
        !sections || session_count == 0 || !sessions) {
        return false;
    }

    const size_t session_offset = workspace.fallback_sessions.size();
    for (size_t i = 0; i < session_count; ++i) {
        if (!workspace.fallback_sessions.append()) {
            return false;
        }
        workspace.fallback_sessions.data()[session_offset + i] = sessions[i];
    }

    const size_t section_offset = workspace.fallback_sections.size();
    for (size_t i = 0; i < section_count; ++i) {
        NightCatalogFallbackSectionInput *section =
            workspace.fallback_sections.append();
        if (!section) return false;
        section->kind = sections[i].kind;
        section->source = sections[i].source;
        section->signal = sections[i].signal;
        section->event_mask = sections[i].event_mask;
        section->payload_schema = sections[i].payload_schema;
        section->record_count = sections[i].record_count;
        section->sample_interval_ms = sections[i].sample_interval_ms;
        section->coverage = sections[i].coverage;
        section->data_offset = sections[i].data_offset;
        section->data_size = sections[i].data_size;
        section->data_crc32 = sections[i].data_crc32;
    }

    NightCatalogFallbackInput *fallback = workspace.fallbacks.append();
    if (!fallback) return false;
    fallback->sleep_day = record.sleep_day;
    fallback->day_start_ms = record.day_start_ms;
    fallback->day_end_ms = record.day_end_ms;
    fallback->sessions = workspace.fallback_sessions.data() + session_offset;
    fallback->session_count = session_count;
    fallback->path = path;
    fallback->file_size = source.file_size;
    fallback->last_write_ms = source.last_write_ms;
    fallback->identity = source.identity;
    fallback->metadata_bytes = source.metadata_bytes;
    fallback->sections = workspace.fallback_sections.data() + section_offset;
    fallback->section_count = section_count;
    fallback->time_adjust_ms = source.time_adjust_ms;
    fallback->resolved_timezone_offset_minutes =
        record.timezone_offset_minutes;
    fallback->resolved_timezone_offset_valid = record.timezone_offset_valid;
    fallback->coordinates_are_resolved = true;
    fallback->retain_with_edf = true;
    return true;
}

bool append_existing_fallbacks(const NightCatalog &catalog,
                               const NightCatalogRecord &record,
                               const NightCatalogTimeRange *sessions,
                               size_t session_count,
                               CaptureWorkspace &workspace) {
    size_t fallback_count = 0;
    const NightCatalogFallbackFile *fallbacks =
        catalog.fallback_files(record, fallback_count);
    if (fallback_count != record.fallback_file_count ||
        (fallback_count > 0 && !fallbacks)) {
        return false;
    }
    for (size_t i = 0; i < fallback_count; ++i) {
        if (!append_existing_fallback(catalog,
                                      record,
                                      fallbacks[i],
                                      sessions,
                                      session_count,
                                      workspace)) {
            return false;
        }
    }
    return true;
}

bool append_session(CaptureWorkspace &workspace,
                    SleepDayId sleep_day,
                    int64_t day_start_ms,
                    int64_t day_end_ms,
                    NightCatalogTimeRange display_window,
                    size_t file_offset,
                    size_t file_count,
                    bool active_capture) {
    if (!display_window.valid() ||
        file_offset > workspace.files.size() ||
        file_count > workspace.files.size() - file_offset) {
        return false;
    }

    NightCatalogEdfSessionInput *session = workspace.sessions.append();
    if (!session) return false;
    session->sleep_day = sleep_day;
    session->day_start_ms = day_start_ms;
    session->day_end_ms = day_end_ms;
    session->display_window = display_window;
    session->files = file_count > 0
        ? workspace.files.data() + file_offset
        : nullptr;
    session->file_count = file_count;
    session->active_capture = active_capture;
    return true;
}

bool build_capture_input(const std::shared_ptr<const NightCatalog> &previous,
                         const EdfSessionMetadata &metadata,
                         const CaptureFiles &current,
                         int64_t day_start_ms,
                         int64_t day_end_ms,
                         int64_t publication_end_ms,
                         CaptureWorkspace &workspace) {
    const NightCatalogRecord *old_night = previous
        ? previous->find(metadata.canonical_sleep_day)
        : nullptr;

    size_t old_session_count = 0;
    const NightCatalogTimeRange *old_sessions = old_night
        ? previous->sessions(*old_night, old_session_count)
        : nullptr;
    size_t old_file_count = 0;
    const NightCatalogSourceFile *old_files = old_night
        ? previous->files(*old_night, old_file_count)
        : nullptr;
    if (old_night &&
        (old_session_count != old_night->session_count ||
         old_file_count != old_night->file_count ||
         (old_session_count > 0 && !old_sessions) ||
         (old_file_count > 0 && !old_files))) {
        return false;
    }
    if (old_night &&
        !append_existing_fallbacks(*previous,
                                   *old_night,
                                   old_sessions,
                                   old_session_count,
                                   workspace)) {
        return false;
    }

    size_t active_old_session = SIZE_MAX;
    for (size_t i = 0; i < old_file_count; ++i) {
        if (old_files[i].kind == NightCatalogFileKind::Str ||
            !current_path(current, previous->path(old_files[i]))) {
            continue;
        }
        if (old_files[i].session_index == NIGHT_CATALOG_NO_SESSION ||
            old_files[i].session_index >= old_session_count) {
            continue;
        }
        if (active_old_session == SIZE_MAX) {
            active_old_session = old_files[i].session_index;
        } else if (active_old_session != old_files[i].session_index) {
            return false;
        }
    }

    const int64_t common_start_ms = day_start_ms;
    const int64_t common_end_ms = day_end_ms;
    if (common_start_ms <= 0 || common_end_ms <= common_start_ms ||
        publication_end_ms <= common_start_ms ||
        publication_end_ms > common_end_ms) {
        return false;
    }

    for (size_t session_index = 0;
         session_index < old_session_count;
         ++session_index) {
        if (session_index == active_old_session) continue;

        const size_t file_offset = workspace.files.size();
        for (size_t file_index = 0; file_index < old_file_count; ++file_index) {
            const NightCatalogSourceFile &file = old_files[file_index];
            if (file.kind == NightCatalogFileKind::Str ||
                file.session_index != session_index) {
                continue;
            }
            if (!append_existing_file(*previous, file, workspace)) return false;
        }
        const size_t file_count = workspace.files.size() - file_offset;
        if (!append_session(workspace,
                            metadata.canonical_sleep_day,
                            common_start_ms,
                            common_end_ms,
                            old_sessions[session_index],
                            file_offset,
                            file_count,
                            false)) {
            return false;
        }
    }

    const size_t active_file_offset = workspace.files.size();
    if (active_old_session != SIZE_MAX) {
        for (size_t file_index = 0; file_index < old_file_count; ++file_index) {
            const NightCatalogSourceFile &file = old_files[file_index];
            if (file.kind == NightCatalogFileKind::Str ||
                file.session_index != active_old_session ||
                current_path(current, previous->path(file))) {
                continue;
            }
            if (!append_existing_file(*previous, file, workspace)) return false;
        }
    }
    for (size_t file_index = 0; file_index < current.file_count; ++file_index) {
        if (!append_current_file(current.files[file_index], workspace)) {
            return false;
        }
    }

    const size_t active_file_count = workspace.files.size() - active_file_offset;
    int64_t active_start_ms = 0;
    if (!session_bounds_start(workspace.files.data() + active_file_offset,
                              active_file_count,
                              active_start_ms)) {
        return false;
    }
    return append_session(workspace,
                           metadata.canonical_sleep_day,
                           common_start_ms,
                           common_end_ms,
                           {active_start_ms, publication_end_ms},
                           active_file_offset,
                           active_file_count,
                           true);
}

}  // namespace

int64_t NightCatalogCapture::closed_end(
    const EdfSessionMetadata &metadata,
    const EdfStorageProgress &progress) {
    CaptureFilesPtr files = make_capture_files();
    if (!files || !capture_files(metadata, progress, *files)) {
        return 0;
    }
    return closed_end_for_capture(metadata, *files);
}

std::shared_ptr<const NightCatalog> NightCatalogCapture::build(
    const std::shared_ptr<const NightCatalog> &previous,
    const EdfSessionMetadata &metadata,
    const EdfStorageProgress &progress,
    int64_t publication_end_ms) {
    CaptureFilesPtr files = make_capture_files();
    if (!files ||
        !capture_files(metadata, progress, *files) ||
        publication_end_ms <= 0 ||
        publication_end_ms % CAPTURE_QUARTER_MS != 0 ||
        publication_end_ms > closed_end_for_capture(metadata, *files) ||
        !clip_capture_files(*files, publication_end_ms) ||
        files->file_count == 0 ||
        files->latest_primary_end_ms <= 0 ||
        files->earliest_start_ms <= 0) {
        return {};
    }

    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
    if (!day_boundaries(metadata.canonical_sleep_day,
                        metadata.timezone_offset_minutes,
                        day_start_ms,
                        day_end_ms)) {
        return {};
    }

    CaptureWorkspace workspace;
    size_t old_session_count = 0;
    size_t old_file_count = 0;
    size_t old_fallback_count = 0;
    size_t old_fallback_section_count = 0;
    if (previous) {
        const NightCatalogRecord *old_night =
            previous->find(metadata.canonical_sleep_day);
        if (old_night && old_night->sources_external) return {};

        if (old_night) {
            size_t count = 0;
            (void)previous->sessions(*old_night, count);
            old_session_count = count;
            (void)previous->files(*old_night, old_file_count);
            size_t fallback_count = 0;
            const NightCatalogFallbackFile *fallbacks =
                previous->fallback_files(*old_night, fallback_count);
            old_fallback_count = fallback_count;
            for (size_t i = 0; fallbacks && i < fallback_count; ++i) {
                if (old_fallback_section_count >
                    SIZE_MAX - fallbacks[i].section_count) {
                    return {};
                }
                old_fallback_section_count += fallbacks[i].section_count;
            }
        }
    }
    if (old_session_count == SIZE_MAX ||
        old_file_count > SIZE_MAX - files->file_count ||
        (old_session_count != 0 &&
         old_fallback_count > SIZE_MAX / old_session_count)) {
        return {};
    }
    if (!workspace.sessions.allocate(old_session_count + 1) ||
        !workspace.files.allocate(old_file_count + files->file_count) ||
        !workspace.fallbacks.allocate(old_fallback_count) ||
        !workspace.fallback_sections.allocate(old_fallback_section_count) ||
        !workspace.fallback_sessions.allocate(
            old_session_count * old_fallback_count)) {
        return {};
    }

    if (!build_capture_input(previous,
                             metadata,
                             *files,
                             day_start_ms,
                             day_end_ms,
                             publication_end_ms,
                             workspace)) {
        return {};
    }

    NightCatalogBuildInput input;
    input.edf_sessions = workspace.sessions.data();
    input.edf_session_count = workspace.sessions.size();
    input.fallback_records = workspace.fallbacks.data();
    input.fallback_record_count = workspace.fallbacks.size();
    const std::shared_ptr<const NightCatalog> rebuilt =
        NightCatalogBuilder::build(input);
    if (!rebuilt) return {};

    if (!previous) return rebuilt;
    return NightCatalogBuilder::upsert_night(
        *previous,
        *rebuilt,
        metadata.canonical_sleep_day);
}

}  // namespace aircannect
