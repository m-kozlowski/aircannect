#include "edf_day_statistics.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edf_report_event_reader.h"
#include "edf_report_session.h"
#include "edf_report_series_reader.h"
#include "edf_file_writer.h"
#include "edf_session_metadata.h"
#include "large_object.h"
#include "memory_manager.h"
#include "string_util.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {
namespace {

constexpr int64_t MS_PER_DAY = INT64_C(86400000);
constexpr int64_t MS_PER_HOUR = INT64_C(3600000);
constexpr size_t HEADER_MAX_BYTES = 8192;
constexpr size_t RECORD_MAX_BYTES = 64 * 1024;
constexpr size_t METADATA_CACHE_MAX = 24;
constexpr uint32_t FINALIZE_BINS_PER_POLL = 2048;

enum class ReadKind : uint8_t {
    None,
    Header,
    Metadata,
    Record,
};

bool supported_file_kind(EdfInventoryFileKind kind) {
    return kind == EdfInventoryFileKind::Brp ||
           kind == EdfInventoryFileKind::Pld ||
           kind == EdfInventoryFileKind::Sa2 ||
           kind == EdfInventoryFileKind::Eve ||
           kind == EdfInventoryFileKind::Csl;
}

bool annotation_kind(EdfInventoryFileKind kind) {
    return kind == EdfInventoryFileKind::Eve ||
           kind == EdfInventoryFileKind::Csl;
}

bool statistics_signal_used_by_airmini_str(ReportSignalId signal) {
    switch (signal) {
        case ReportSignalId::InspiratoryPressure:
        case ReportSignalId::ExpiratoryPressure:
        case ReportSignalId::MaskPressure:
        case ReportSignalId::Flow:
        case ReportSignalId::Leak:
        case ReportSignalId::SpO2:
            return true;
        default:
            return false;
    }
}

size_t event_coverage_index(EdfInventoryFileKind kind) {
    if (kind == EdfInventoryFileKind::Eve) return 0;
    if (kind == EdfInventoryFileKind::Csl) return 1;
    return 2;
}

bool day_window(SleepDayId day,
                int32_t timezone_offset_minutes,
                int64_t &start_ms,
                int64_t &end_ms) {
    if (!day.valid() || timezone_offset_minutes < -24 * 60 ||
        timezone_offset_minutes > 24 * 60) {
        return false;
    }

    const int64_t local_start =
        static_cast<int64_t>(day.epoch_days()) * MS_PER_DAY +
        12 * MS_PER_HOUR;
    const int64_t offset =
        static_cast<int64_t>(timezone_offset_minutes) * 60 * 1000;
    if ((offset > 0 && local_start < INT64_MIN + offset) ||
        (offset < 0 && local_start > INT64_MAX + offset)) {
        return false;
    }

    start_ms = local_start - offset;
    if (start_ms > INT64_MAX - MS_PER_DAY) return false;
    end_ms = start_ms + MS_PER_DAY;
    return true;
}

int32_t saturate_milli(float value) {
    const long scaled = lroundf(value * 1000.0f);
    if (scaled < INT32_MIN) return INT32_MIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(scaled);
}

int32_t physical_milli(const EdfSignalScale &scale, int16_t raw) {
    return saturate_milli(scale.offset + raw * scale.scale);
}

uint16_t source_bit(ReportSourceId source) {
    const uint8_t index = static_cast<uint8_t>(source);
    return index < 16 ? static_cast<uint16_t>(1u << index) : 0;
}

int32_t canonical_value_milli(const EdfDaySignalStatistics &statistics,
                              int16_t raw) {
    const ReportSeriesDescriptor descriptor{
        statistics.signal,
        statistics.source,
        0,
        statistics.primary,
    };
    return report_series_canonical_value_milli(
        descriptor,
        physical_milli(statistics.scale, raw));
}

struct Histogram {
    uint32_t *counts = nullptr;
    int32_t raw_min = 0;
    uint32_t bin_count = 0;
};

bool initialize_histogram(const EdfSignalScale &scale,
                          Histogram &histogram) {
    const int32_t raw_min = scale.digital_min;
    const int32_t raw_max = scale.digital_max;
    if (raw_max < raw_min) return false;

    histogram.raw_min = raw_min;
    histogram.bin_count = static_cast<uint32_t>(raw_max - raw_min + 1);
    histogram.counts = static_cast<uint32_t *>(
        Memory::calloc_large(histogram.bin_count,
                             sizeof(uint32_t),
                             false));
    return histogram.counts != nullptr;
}

bool fallback_signal_scale(ReportSignalId signal,
                           ReportSourceId &source,
                           EdfSignalScale &scale) {
    const char *label = nullptr;
    switch (signal) {
        case ReportSignalId::InspiratoryPressure:
            label = "Press.2s";
            source = ReportSourceId::InspiratoryPressure0p5Hz;
            break;
        case ReportSignalId::Leak:
            label = "Leak.2s";
            source = ReportSourceId::Leak0p5Hz;
            break;
        default:
            return false;
    }

    const EdfFileSchema &schema = edf_numeric_schema(EdfFileKind::Pld);
    for (size_t i = 0; i < schema.source_signal_count; ++i) {
        const EdfSignalSpec &spec = schema.signals[i];
        if (!spec.label || strcmp(spec.label, label) != 0 ||
            spec.digital_max_value <= spec.digital_min_value) {
            continue;
        }
        const float physical_min = strtof(spec.physical_min, nullptr);
        const float physical_max = strtof(spec.physical_max, nullptr);
        const float digital_span = static_cast<float>(
            spec.digital_max_value - spec.digital_min_value);
        if (!isfinite(physical_min) || !isfinite(physical_max) ||
            !isfinite(digital_span) || digital_span <= 0.0f) {
            return false;
        }
        scale.digital_min = spec.digital_min_value;
        scale.digital_max = spec.digital_max_value;
        scale.physical_min = physical_min;
        scale.physical_max = physical_max;
        scale.scale = (physical_max - physical_min) / digital_span;
        scale.offset = physical_min -
            static_cast<float>(spec.digital_min_value) * scale.scale;
        return isfinite(scale.scale) && scale.scale > 0.0f;
    }
    return false;
}

void add_histogram_weight(Histogram &histogram,
                          int32_t raw,
                          uint32_t weight) {
    if (!histogram.counts || histogram.bin_count == 0 || weight == 0) {
        return;
    }

    if (raw < histogram.raw_min) raw = histogram.raw_min;
    const int32_t raw_max = histogram.raw_min +
        static_cast<int32_t>(histogram.bin_count - 1);
    if (raw > raw_max) raw = raw_max;
    const size_t index = static_cast<size_t>(raw - histogram.raw_min);
    const uint32_t current = histogram.counts[index];
    histogram.counts[index] = UINT32_MAX - current < weight
        ? UINT32_MAX
        : current + weight;
}

void merge_coverage(EdfDayCoverage &coverage,
                    int64_t start_ms,
                    int64_t end_ms,
                    uint32_t &warnings) {
    if (end_ms <= start_ms) return;

    size_t first = 0;
    while (first < coverage.count &&
           coverage.intervals[first].end_ms < start_ms) {
        ++first;
    }

    int64_t merged_start = start_ms;
    int64_t merged_end = end_ms;
    size_t last = first;
    while (last < coverage.count &&
           coverage.intervals[last].start_ms <= merged_end) {
        merged_start = std::min(merged_start,
                                coverage.intervals[last].start_ms);
        merged_end = std::max(merged_end,
                              coverage.intervals[last].end_ms);
        ++last;
    }

    if (first == last && coverage.count >= AC_EDF_DAY_STATISTICS_COVERAGE_MAX) {
        coverage.truncated = true;
        warnings |= EDF_DAY_STATISTICS_WARNING_COVERAGE_TRUNCATED;
        return;
    }

    const size_t removed = last - first;
    if (removed == 0) {
        for (size_t i = coverage.count; i > first; --i) {
            coverage.intervals[i] = coverage.intervals[i - 1];
        }
        ++coverage.count;
    } else if (removed > 1) {
        const size_t tail = coverage.count - last;
        memmove(coverage.intervals + first + 1,
                coverage.intervals + last,
                tail * sizeof(coverage.intervals[0]));
        coverage.count -= removed - 1;
    }

    coverage.intervals[first] = {merged_start, merged_end};
}

uint64_t uncovered_duration_ms(const EdfDayCoverage &coverage,
                               int64_t start_ms,
                               int64_t end_ms) {
    if (end_ms <= start_ms) return 0;

    int64_t cursor = start_ms;
    uint64_t uncovered = 0;
    for (size_t i = 0; i < coverage.count; ++i) {
        const EdfDayCoverageInterval &interval = coverage.intervals[i];
        if (interval.end_ms <= cursor) continue;
        if (interval.start_ms >= end_ms) break;

        if (interval.start_ms > cursor) {
            const int64_t gap_end = std::min(interval.start_ms, end_ms);
            uncovered += static_cast<uint64_t>(gap_end - cursor);
        }
        cursor = std::max(cursor, interval.end_ms);
        if (cursor >= end_ms) return uncovered;
    }

    if (cursor < end_ms) {
        uncovered += static_cast<uint64_t>(end_ms - cursor);
    }
    return uncovered;
}

void clear_histograms(Histogram *histograms, size_t count) {
    if (!histograms) return;
    for (size_t i = 0; i < count; ++i) {
        if (histograms[i].counts) Memory::free(histograms[i].counts);
        histograms[i] = {};
    }
}

bool canonical_to_raw_time(const EdfSessionMetadata &metadata,
                           int64_t canonical_ms,
                           int64_t &raw_ms) {
    if (!metadata.externally_corrected) {
        raw_ms = canonical_ms;
        return true;
    }

    if ((metadata.device_minus_utc_ms > 0 &&
         canonical_ms > INT64_MAX - metadata.device_minus_utc_ms) ||
        (metadata.device_minus_utc_ms < 0 &&
         canonical_ms < INT64_MIN - metadata.device_minus_utc_ms)) {
        return false;
    }

    raw_ms = canonical_ms + metadata.device_minus_utc_ms;
    return true;
}

}  // namespace

struct EdfDayStatisticsReader::Runtime {
    struct MetadataEntry {
        bool attempted = false;
        bool available = false;
        char sleep_day[9] = {};
        char session_stamp[16] = {};
        EdfSessionMetadata metadata;
    };

    SleepDayId day;
    int32_t timezone_offset_minutes = 0;
    int64_t window_start_ms = 0;
    int64_t window_end_ms = 0;
    char scan_root_paths[2][64] = {};
    char metadata_path[96] = {};

    OperationTicket scan_ticket;
    OperationTicket read_ticket;
    StoragePreparedRead prepared;
    ReadKind read_kind = ReadKind::None;
    bool cancel_requested = false;

    std::shared_ptr<const StorageScanSnapshot> snapshot;
    size_t scan_index = 0;
    uint8_t scan_pass = 0;

    EdfReportSessionDescriptor numeric_sessions[
        AC_EDF_DAY_STATISTICS_SESSION_MAX] = {};
    size_t numeric_session_count = 0;
    char metadata_lookup_sleep_day[9] = {};
    char metadata_lookup_session_stamp[16] = {};

    char current_path[AC_EDF_REPORT_PATH_MAX] = {};
    uint64_t current_file_size = 0;
    uint64_t current_modified = 0;
    EdfReportFileDescriptor current_file;
    EdfReportSignalLayout layouts[AC_EDF_REPORT_FILE_SIGNAL_MAX] = {};
    EdfReportSeriesDecoder decoders[AC_EDF_REPORT_FILE_SIGNAL_MAX];
    bool layout_has_sample[AC_EDF_REPORT_FILE_SIGNAL_MAX] = {};
    size_t layout_count = 0;
    uint32_t record_index = 0;
    int64_t current_header_start_ms = 0;
    int64_t current_range_start_ms = 0;
    int64_t current_range_end_ms = 0;
    int64_t current_actual_end_ms = 0;
    EdfReportEventDecodeContext event_context;
    size_t active_layout_index = 0;
    size_t finalize_signal_index = 0;
    uint32_t finalize_bin_index = 0;
    uint64_t finalize_cumulative = 0;
    uint64_t finalize_ranks[6] = {};
    uint64_t finalize_upper_ranks[6] = {};
    uint32_t finalize_remainders[6] = {};
    int32_t finalize_lower_raw[6] = {};
    int32_t finalize_upper_raw[6] = {};
    bool finalize_lower_found[6] = {};
    bool finalize_upper_found[6] = {};
    bool finalize_signal_initialized = false;
    int metadata_slot = -1;
    bool metadata_lookup_complete = false;

    MetadataEntry metadata[METADATA_CACHE_MAX];
    Histogram histograms[AC_EDF_DAY_STATISTICS_SIGNAL_MAX] = {};
    EdfDayStatisticsResult result;
};

EdfDayStatisticsReader::EdfDayStatisticsReader() {
    runtime_ = LargeObject::create<Runtime>();
}

EdfDayStatisticsReader::~EdfDayStatisticsReader() {
    reset();
    LargeObject::destroy(runtime_);
    runtime_ = nullptr;
}

void EdfDayStatisticsReader::begin(StorageReadPort &read_port,
                                    StorageScanPort &scan_port) {
    if (status_.active()) return;
    read_port_ = &read_port;
    scan_port_ = &scan_port;
    reset();
}

OperationAdmission EdfDayStatisticsReader::start(
    SleepDayId day,
    int32_t timezone_offset_minutes) {
    if (!runtime_ || !read_port_ || !scan_port_ || status_.active() ||
        !day_window(day,
                    timezone_offset_minutes,
                    runtime_->window_start_ms,
                    runtime_->window_end_ms)) {
        return OperationAdmission::Rejected;
    }

    if (status_.state != EdfDayStatisticsState::Idle &&
        !status_.terminal()) {
        return OperationAdmission::Busy;
    }
    reset();
    if (!runtime_) return OperationAdmission::Rejected;

    runtime_->day = day;
    runtime_->timezone_offset_minutes = timezone_offset_minutes;
    if (!day_window(day,
                    timezone_offset_minutes,
                    runtime_->window_start_ms,
                    runtime_->window_end_ms)) {
        return OperationAdmission::Rejected;
    }

    SleepDayId next_day;
    char day_text[9] = {};
    char next_day_text[9] = {};
    if (!SleepDayId::from_epoch_days(day.epoch_days() + 1, next_day) ||
        !day.format_yyyymmdd(day_text, sizeof(day_text)) ||
        !next_day.format_yyyymmdd(next_day_text, sizeof(next_day_text))) {
        return OperationAdmission::Rejected;
    }
    snprintf(runtime_->scan_root_paths[0],
             sizeof(runtime_->scan_root_paths[0]),
             "/DATALOG/%s",
             day_text);
    snprintf(runtime_->scan_root_paths[1],
             sizeof(runtime_->scan_root_paths[1]),
             "/DATALOG/%s",
             next_day_text);

    runtime_->result.day = day;
    runtime_->result.window_start_ms = runtime_->window_start_ms;
    runtime_->result.window_end_ms = runtime_->window_end_ms;
    runtime_->result.event_coverage[0].kind = EdfInventoryFileKind::Eve;
    runtime_->result.event_coverage[1].kind = EdfInventoryFileKind::Csl;

    ++status_.generation;
    if (status_.generation == 0) ++status_.generation;
    status_.error = EdfDayStatisticsError::None;
    status_.error_text[0] = '\0';
    status_.files_seen = 0;
    status_.files_processed = 0;
    status_.records_processed = 0;
    status_.state = EdfDayStatisticsState::SubmitScan;
    return OperationAdmission::Accepted;
}

const EdfDayStatisticsResult &EdfDayStatisticsReader::result() const {
    static const EdfDayStatisticsResult empty;
    return runtime_ ? runtime_->result : empty;
}

bool EdfDayStatisticsReader::submit_scan() {
    if (!runtime_ || !scan_port_) return false;

    StorageScanRoot roots[2] = {
        {runtime_->scan_root_paths[0], false},
        {runtime_->scan_root_paths[1], false},
    };
    StorageScanCommand command;
    command.roots = roots;
    command.root_count = 2;
    command.generation = status_.generation;
    const OperationSubmission submission = scan_port_->request_scan(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail(EdfDayStatisticsError::ScanRejected, "scan_rejected");
        return true;
    }

    runtime_->scan_ticket = submission.ticket;
    status_.state = EdfDayStatisticsState::WaitScan;
    return true;
}

bool EdfDayStatisticsReader::take_scan() {
    if (!runtime_ || !runtime_->scan_ticket.valid() || !scan_port_) {
        return false;
    }

    StorageScanCompletion completion;
    if (!scan_port_->take_completion(runtime_->scan_ticket, completion)) {
        return false;
    }
    runtime_->scan_ticket = {};
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.snapshot) {
        fail(EdfDayStatisticsError::ScanFailed,
             completion.error[0] ? completion.error : "scan_failed");
        return true;
    }

    runtime_->snapshot = std::move(completion.snapshot);
    runtime_->scan_index = 0;
    status_.state = EdfDayStatisticsState::SelectFile;
    return true;
}

bool EdfDayStatisticsReader::select_file() {
    if (!runtime_ || !runtime_->snapshot) {
        fail(EdfDayStatisticsError::ScanFailed, "scan_snapshot_missing");
        return true;
    }
    if (runtime_->scan_index >= runtime_->snapshot->size()) {
        if (runtime_->scan_pass == 0) {
            runtime_->scan_pass = 1;
            runtime_->scan_index = 0;
            return true;
        }
        complete();
        return true;
    }

    StorageScanEntryView entry;
    if (!runtime_->snapshot->entry(runtime_->scan_index++, entry) ||
        entry.directory || !entry.path || !entry.path[0]) {
        status_.state = EdfDayStatisticsState::SelectFile;
        return true;
    }

    EdfInventoryEntry inventory;
    if (!edf_inventory_describe_path(entry.path, inventory) ||
        !supported_file_kind(inventory.kind)) {
        status_.state = EdfDayStatisticsState::SelectFile;
        return true;
    }

    if (annotation_kind(inventory.kind) != (runtime_->scan_pass == 1)) {
        status_.state = EdfDayStatisticsState::SelectFile;
        return true;
    }

    copy_cstr(runtime_->current_path,
              sizeof(runtime_->current_path),
              entry.path);
    runtime_->current_file_size = entry.size;
    runtime_->current_modified = entry.modified;
    status_.files_seen++;
    status_.state = EdfDayStatisticsState::SubmitHeader;
    return true;
}

bool EdfDayStatisticsReader::submit_header() {
    if (!runtime_ || !read_port_ || runtime_->current_file_size == 0) {
        runtime_->result.warnings |= EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
        status_.state = EdfDayStatisticsState::SelectFile;
        return true;
    }

    StorageReadCommand command;
    command.path = runtime_->current_path;
    command.offset = 0;
    command.length = std::min<uint64_t>(runtime_->current_file_size,
                                        HEADER_MAX_BYTES);
    command.lane = StorageReadLane::Report;
    command.generation = status_.generation;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail(EdfDayStatisticsError::ReadRejected, "header_read_rejected");
        return true;
    }

    runtime_->read_ticket = submission.ticket;
    runtime_->read_kind = ReadKind::Header;
    status_.state = EdfDayStatisticsState::WaitHeader;
    return true;
}

int EdfDayStatisticsReader::metadata_slot_for_current() const {
    if (!runtime_) return -1;
    for (size_t i = 0; i < METADATA_CACHE_MAX; ++i) {
        const Runtime::MetadataEntry &entry = runtime_->metadata[i];
        if (entry.attempted &&
            strcmp(entry.sleep_day,
                   runtime_->metadata_lookup_sleep_day) == 0 &&
            strcmp(entry.session_stamp,
                   runtime_->metadata_lookup_session_stamp) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool EdfDayStatisticsReader::describe_current_file(const uint8_t *header,
                                                   size_t length) {
    if (!runtime_ ||
        edf_report_describe_file(runtime_->current_path,
                                 header,
                                 length,
                                 runtime_->current_file_size,
                                 static_cast<time_t>(runtime_->current_modified),
                                 runtime_->timezone_offset_minutes,
                                 runtime_->current_file) !=
            EdfReportFileStatus::Ok ||
        runtime_->current_file.record_size == 0 ||
        runtime_->current_file.record_size > RECORD_MAX_BYTES ||
        runtime_->current_file.inventory.complete_records_from_size >
            UINT32_MAX) {
        return false;
    }

    if (annotation_kind(runtime_->current_file.inventory.kind)) {
        return runtime_->current_file.header_start_ms <
               runtime_->window_end_ms;
    }

    return runtime_->current_file.header_end_ms >
               runtime_->window_start_ms &&
           runtime_->current_file.header_start_ms <
               runtime_->window_end_ms;
}

bool EdfDayStatisticsReader::register_current_numeric_session() {
    if (!runtime_) return false;

    for (size_t i = 0; i < runtime_->numeric_session_count; ++i) {
        EdfReportSessionDescriptor &session = runtime_->numeric_sessions[i];
        if (strcmp(session.sleep_day,
                   runtime_->current_file.inventory.sleep_day) != 0 ||
            strcmp(session.session_stamp,
                   runtime_->current_file.inventory.session_stamp) != 0) {
            continue;
        }

        const uint32_t mask = edf_report_file_kind_mask(
            runtime_->current_file.inventory.kind);
        return mask != 0 && (session.file_mask & mask) != 0
            ? true
            : edf_report_session_add_file(session, runtime_->current_file);
    }

    if (runtime_->numeric_session_count >=
        AC_EDF_DAY_STATISTICS_SESSION_MAX) {
        return false;
    }

    EdfReportSessionDescriptor &session =
        runtime_->numeric_sessions[runtime_->numeric_session_count];
    edf_report_session_init(session);
    if (!edf_report_session_add_file(session, runtime_->current_file)) {
        return false;
    }
    ++runtime_->numeric_session_count;
    return true;
}

bool EdfDayStatisticsReader::resolve_current_annotation_session() {
    if (!runtime_) return false;

    EdfReportSessionDescriptor annotation;
    edf_report_session_init(annotation);
    if (!edf_report_session_add_file(annotation, runtime_->current_file)) {
        return false;
    }

    size_t match = runtime_->numeric_session_count;
    for (size_t i = 0; i < runtime_->numeric_session_count; ++i) {
        if (!edf_session_annotation_matches_numeric(
                runtime_->numeric_sessions[i], annotation)) {
            continue;
        }
        if (match != runtime_->numeric_session_count) return false;
        match = i;
    }
    if (match == runtime_->numeric_session_count) return false;

    const EdfReportSessionDescriptor &numeric =
        runtime_->numeric_sessions[match];
    copy_cstr(runtime_->metadata_lookup_sleep_day,
              sizeof(runtime_->metadata_lookup_sleep_day),
              numeric.sleep_day);
    copy_cstr(runtime_->metadata_lookup_session_stamp,
              sizeof(runtime_->metadata_lookup_session_stamp),
              numeric.session_stamp);
    return true;
}

bool EdfDayStatisticsReader::submit_metadata() {
    if (!runtime_ || !read_port_ || runtime_->metadata_slot < 0) {
        return false;
    }

    StorageReadCommand command;
    command.path = runtime_->metadata_path;
    command.length = EdfSessionMetadataCodec::RecordBytes;
    command.lane = StorageReadLane::Report;
    command.generation = status_.generation;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        runtime_->metadata_slot = -1;
        runtime_->metadata_lookup_complete = true;
        status_.state = EdfDayStatisticsState::SubmitRecord;
        if (!prepare_current_file()) {
            runtime_->result.warnings |=
                EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
            status_.state = EdfDayStatisticsState::SelectFile;
        }
        return true;
    }

    runtime_->read_ticket = submission.ticket;
    runtime_->read_kind = ReadKind::Metadata;
    status_.state = EdfDayStatisticsState::WaitMetadata;
    return true;
}

bool EdfDayStatisticsReader::apply_current_metadata() {
    if (!runtime_) return false;

    runtime_->current_header_start_ms = runtime_->current_file.header_start_ms;
    const bool annotation = annotation_kind(
        runtime_->current_file.inventory.kind);
    int64_t file_end_ms = annotation
        ? runtime_->window_end_ms
        : runtime_->current_file.header_end_ms;
    const int slot = runtime_->metadata_slot;
    if (slot >= 0 && runtime_->metadata[slot].available) {
        const EdfSessionMetadata &metadata = runtime_->metadata[slot].metadata;
        const int64_t adjustment =
            metadata.canonical_segment_start_ms -
            metadata.raw_segment_start_ms;
        if ((adjustment > 0 &&
             runtime_->current_file.header_start_ms > INT64_MAX - adjustment) ||
            (adjustment < 0 &&
             runtime_->current_file.header_start_ms < INT64_MIN - adjustment)) {
            return false;
        }

        runtime_->current_header_start_ms += adjustment;
        if (!annotation) {
            if ((adjustment > 0 && file_end_ms > INT64_MAX - adjustment) ||
                (adjustment < 0 && file_end_ms < INT64_MIN - adjustment)) {
                return false;
            }
            file_end_ms += adjustment;
        }
        if (metadata.finalized &&
            metadata.canonical_segment_end_ms >
                metadata.canonical_segment_start_ms) {
            file_end_ms = std::min(file_end_ms,
                                   metadata.canonical_segment_end_ms);
        }
        runtime_->result.provenance_used = true;
    }

    runtime_->current_range_start_ms = std::max(
        runtime_->window_start_ms,
        runtime_->current_header_start_ms);
    runtime_->current_range_end_ms = std::min(runtime_->window_end_ms,
                                               file_end_ms);
    return runtime_->current_range_end_ms > runtime_->current_range_start_ms;
}

void EdfDayStatisticsReader::append_provenance(
    const EdfSessionMetadata &metadata) {
    if (!runtime_) return;

    for (size_t i = 0;
         i < runtime_->result.provenance_session_count;
         ++i) {
        const EdfSessionMetadata &current =
            runtime_->result.provenance_sessions[i];
        if (current.capture_session_id == metadata.capture_session_id &&
            strcmp(current.session_stamp, metadata.session_stamp) == 0) {
            return;
        }
    }

    if (runtime_->result.provenance_session_count >=
        AC_EDF_DAY_STATISTICS_SESSION_MAX) {
        runtime_->result.provenance_sessions_truncated = true;
        return;
    }

    runtime_->result.provenance_sessions[
        runtime_->result.provenance_session_count++] = metadata;
}

void EdfDayStatisticsReader::update_result_provenance_end() {
    if (!runtime_ || runtime_->metadata_slot < 0 ||
        runtime_->current_actual_end_ms <= 0) {
        return;
    }

    const Runtime::MetadataEntry &entry =
        runtime_->metadata[runtime_->metadata_slot];
    if (!entry.available || entry.metadata.finalized) return;

    EdfSessionMetadata *provenance = nullptr;
    for (size_t i = 0;
         i < runtime_->result.provenance_session_count;
         ++i) {
        EdfSessionMetadata &candidate =
            runtime_->result.provenance_sessions[i];
        if (candidate.capture_session_id == entry.metadata.capture_session_id &&
            strcmp(candidate.session_stamp, entry.metadata.session_stamp) == 0) {
            provenance = &candidate;
            break;
        }
    }
    if (!provenance) return;

    const int64_t canonical_start =
        entry.metadata.canonical_segment_start_ms;
    int64_t canonical_end = runtime_->current_actual_end_ms;
    if (provenance->canonical_segment_end_ms > canonical_end) {
        canonical_end = provenance->canonical_segment_end_ms;
    }
    if (canonical_end <= canonical_start) return;

    int64_t raw_end = 0;
    if (!canonical_to_raw_time(entry.metadata, canonical_end, raw_end)) {
        return;
    }
    if (raw_end <= entry.metadata.raw_segment_start_ms ||
        raw_end <= entry.metadata.raw_therapy_start_ms) {
        return;
    }

    EdfSessionMetadata observed = entry.metadata;
    if (!edf_session_metadata_finalize(observed, raw_end, raw_end)) return;

    *provenance = observed;
}

int find_signal(const EdfDayStatisticsResult &result,
                ReportSignalId signal,
                ReportSourceId source,
                bool primary) {
    for (size_t i = 0; i < result.signal_count; ++i) {
        const EdfDaySignalStatistics &current = result.signals[i];
        if (current.signal == signal && current.source == source &&
            current.primary == primary) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int find_fallback_signal(const EdfDayStatisticsResult &result,
                         ReportSignalId signal) {
    for (size_t i = 0; i < result.signal_count; ++i) {
        const EdfDaySignalStatistics &current = result.signals[i];
        if (current.signal == signal && current.primary) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < result.signal_count; ++i) {
        if (result.signals[i].signal == signal) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int ensure_fallback_signal(EdfDayStatisticsResult &result,
                           Histogram *histograms,
                           ReportSignalId signal) {
    if (!histograms) return -1;

    const int existing = find_fallback_signal(result, signal);
    if (existing >= 0) return existing;

    if (result.signal_count >= AC_EDF_DAY_STATISTICS_SIGNAL_MAX) return -1;

    ReportSourceId source = ReportSourceId::Summary;
    EdfSignalScale scale;
    if (!fallback_signal_scale(signal, source, scale)) return -1;

    const size_t signal_index = result.signal_count++;
    EdfDaySignalStatistics &statistics = result.signals[signal_index];
    statistics = {};
    statistics.signal = signal;
    statistics.source = source;
    statistics.primary = true;
    statistics.scale = scale;
    statistics.source_mask = source_bit(source);

    if (!initialize_histogram(scale, histograms[signal_index])) {
        --result.signal_count;
        return -1;
    }
    return static_cast<int>(signal_index);
}

bool EdfDayStatisticsReader::prepare_current_file() {
    if (!runtime_ || !apply_current_metadata()) return false;

    runtime_->layout_count = 0;
    memset(runtime_->layout_has_sample,
           0,
           sizeof(runtime_->layout_has_sample));
    runtime_->current_actual_end_ms = 0;
    runtime_->event_context = {};

    if (!annotation_kind(runtime_->current_file.inventory.kind)) {
        size_t layout_count = 0;
        if (!edf_report_file_signal_layouts(
                runtime_->current_file,
                runtime_->layouts,
                AC_EDF_REPORT_FILE_SIGNAL_MAX,
                layout_count)) {
            return false;
        }

        size_t selected_count = 0;
        for (size_t i = 0; i < layout_count; ++i) {
            if (!statistics_signal_used_by_airmini_str(
                    runtime_->layouts[i].signal)) {
                continue;
            }
            runtime_->layouts[selected_count++] = runtime_->layouts[i];
        }
        runtime_->layout_count = selected_count;
        if (runtime_->layout_count == 0) return false;
    }

    for (size_t i = 0; i < runtime_->layout_count; ++i) {
        const EdfReportSignalLayout &layout = runtime_->layouts[i];
        int signal_index = find_signal(runtime_->result,
                                       layout.signal,
                                       layout.source,
                                       layout.primary);
        if (signal_index < 0) {
            if (runtime_->result.signal_count >=
                AC_EDF_DAY_STATISTICS_SIGNAL_MAX) {
                return false;
            }
            signal_index = static_cast<int>(runtime_->result.signal_count++);
            EdfDaySignalStatistics &statistics =
                runtime_->result.signals[signal_index];
            statistics = {};
            statistics.signal = layout.signal;
            statistics.source = layout.source;
            statistics.primary = layout.primary;
            statistics.scale = layout.scale;
            statistics.source_mask = source_bit(layout.source);

            Histogram &histogram = runtime_->histograms[signal_index];
            if (!initialize_histogram(layout.scale, histogram)) return false;
        }

        if (edf_report_series_decoder_init(
                layout,
                runtime_->current_header_start_ms,
                runtime_->current_file.record_duration_ms,
                runtime_->current_file.record_size,
                static_cast<uint32_t>(
                    runtime_->current_file.inventory.complete_records_from_size),
                runtime_->current_range_start_ms,
                runtime_->current_range_end_ms,
                runtime_->decoders[i]) != EdfReportSeriesStatus::Ok) {
            return false;
        }
    }

    runtime_->record_index = 0;
    return true;
}

bool EdfDayStatisticsReader::submit_record() {
    if (!runtime_ || !read_port_) return false;
    if (runtime_->record_index >=
        runtime_->current_file.inventory.complete_records_from_size) {
        return finish_current_file();
    }

    const uint64_t offset =
        static_cast<uint64_t>(runtime_->current_file.header_size) +
        static_cast<uint64_t>(runtime_->record_index) *
            runtime_->current_file.record_size;
    if (offset > UINT64_MAX - runtime_->current_file.record_size) {
        runtime_->result.warnings |= EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
        status_.state = EdfDayStatisticsState::SelectFile;
        return true;
    }

    StorageReadCommand command;
    command.path = runtime_->current_path;
    command.offset = offset;
    command.length = runtime_->current_file.record_size;
    command.lane = StorageReadLane::Report;
    command.generation = status_.generation;
    const OperationSubmission submission = read_port_->request_read(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail(EdfDayStatisticsError::ReadRejected, "record_read_rejected");
        return true;
    }

    runtime_->read_ticket = submission.ticket;
    runtime_->read_kind = ReadKind::Record;
    status_.state = EdfDayStatisticsState::WaitRecord;
    return true;
}

bool EdfDayStatisticsReader::decode_current_record(const uint8_t *record,
                                                   size_t length) {
    if (!runtime_ || !record || length < runtime_->current_file.record_size) {
        return false;
    }

    if (annotation_kind(runtime_->current_file.inventory.kind)) {
        const EdfReportEventSource source{
            runtime_->current_file.inventory.kind,
            runtime_->current_header_start_ms,
        };
        return edf_report_decode_annotation_record(
                   source,
                   record,
                   length,
                   true,
                   &EdfDayStatisticsReader::accept_event,
                   this,
                   &runtime_->event_context) == EdfReportEventStatus::Ok;
    }

    for (size_t i = 0; i < runtime_->layout_count; ++i) {
        runtime_->active_layout_index = i;
        if (edf_report_decode_series_record_spans(
                runtime_->decoders[i],
                record,
                length,
                runtime_->record_index,
                &EdfDayStatisticsReader::accept_series_span,
                this) != EdfReportSeriesStatus::Ok) {
            return false;
        }
    }
    return true;
}

bool EdfDayStatisticsReader::accept_series_span(
    void *context,
    const EdfReportSeriesSpan &span) {
    EdfDayStatisticsReader *reader =
        static_cast<EdfDayStatisticsReader *>(context);
    if (!reader || !reader->runtime_ ||
        reader->runtime_->active_layout_index >= reader->runtime_->layout_count ||
        !span.valid()) {
        return false;
    }

    Runtime *runtime = reader->runtime_;
    const size_t layout_index = runtime->active_layout_index;
    const EdfReportSignalLayout &layout = runtime->layouts[layout_index];
    const int signal_index = find_signal(runtime->result,
                                         layout.signal,
                                         layout.source,
                                         layout.primary);
    if (signal_index < 0 ||
        !runtime->histograms[signal_index].counts) {
        return false;
    }

    Histogram &histogram = runtime->histograms[signal_index];
    EdfDaySignalStatistics &statistics =
        runtime->result.signals[signal_index];

    const uint32_t sample_interval_ms =
        std::max<uint32_t>(1,
                           span.record_duration_ms /
                               span.samples_per_record);
    int64_t run_start_ms = 0;
    int64_t run_end_ms = 0;

    const auto flush_coverage = [&]() {
        if (run_end_ms > run_start_ms) {
            merge_coverage(statistics.coverage,
                           run_start_ms,
                           run_end_ms,
                           runtime->result.warnings);
        }
        run_start_ms = 0;
        run_end_ms = 0;
    };

    for (uint32_t i = 0; i < span.sample_count; ++i) {
        if (span.missing_at(i)) {
            flush_coverage();
            continue;
        }

        int64_t sample_start_ms = span.timestamp_at(i);
        if (sample_start_ms < runtime->current_range_start_ms) {
            sample_start_ms = runtime->current_range_start_ms;
        }
        if (sample_start_ms >= runtime->current_range_end_ms) {
            flush_coverage();
            continue;
        }
        if (sample_start_ms > INT64_MAX - sample_interval_ms) {
            flush_coverage();
            continue;
        }
        const int64_t sample_end_ms = std::min(
            runtime->current_range_end_ms,
            sample_start_ms + static_cast<int64_t>(sample_interval_ms));
        if (sample_end_ms <= sample_start_ms) {
            flush_coverage();
            continue;
        }

        runtime->current_actual_end_ms = std::max(
            runtime->current_actual_end_ms,
            sample_end_ms);

        const uint64_t weight = uncovered_duration_ms(
            statistics.coverage, sample_start_ms, sample_end_ms);
        if (weight > 0) {
            const int16_t raw = span.raw_at(i);
            add_histogram_weight(histogram,
                                 raw,
                                 static_cast<uint32_t>(weight));
            if (statistics.sample_count != UINT64_MAX) {
                ++statistics.sample_count;
            }
            statistics.weighted_duration_ms =
                UINT64_MAX - statistics.weighted_duration_ms < weight
                    ? UINT64_MAX
                    : statistics.weighted_duration_ms + weight;
        }

        if (run_start_ms == 0) run_start_ms = sample_start_ms;
        run_end_ms = sample_end_ms;
        runtime->layout_has_sample[layout_index] = true;
    }
    flush_coverage();
    return true;
}

bool EdfDayStatisticsReader::accept_event(void *context,
                                          const ReportEventRecord &event) {
    EdfDayStatisticsReader *reader =
        static_cast<EdfDayStatisticsReader *>(context);
    if (!reader || !reader->runtime_) return false;

    Runtime *runtime = reader->runtime_;
    const int64_t event_end = event.duration_ms > 0 &&
            event.start_ms <= INT64_MAX - event.duration_ms
        ? event.start_ms + event.duration_ms
        : event.start_ms;
    if (event_end <= runtime->current_range_start_ms ||
        event.start_ms >= runtime->current_range_end_ms) {
        return true;
    }

    for (size_t i = 0; i < runtime->result.event_count; ++i) {
        const ReportEventRecord &existing = runtime->result.events[i];
        if (existing.start_ms == event.start_ms &&
            existing.code == event.code &&
            existing.duration_ms == event.duration_ms) {
            return true;
        }
    }

    if (event.code < 8 && runtime->result.event_counts[event.code] <
                              UINT32_MAX) {
        ++runtime->result.event_counts[event.code];
    }
    if (runtime->result.event_count < AC_EDF_DAY_STATISTICS_EVENT_MAX) {
        runtime->result.events[runtime->result.event_count++] = event;
    } else {
        runtime->result.events_truncated = true;
        runtime->result.warnings |= EDF_DAY_STATISTICS_WARNING_EVENTS_TRUNCATED;
    }
    return true;
}

bool EdfDayStatisticsReader::finish_current_file() {
    if (!runtime_) return false;

    if (!annotation_kind(runtime_->current_file.inventory.kind)) {
        update_result_provenance_end();
    }

    if (annotation_kind(runtime_->current_file.inventory.kind)) {
        const int slot = runtime_->metadata_slot;
        if (slot >= 0 && runtime_->metadata[slot].available &&
            runtime_->metadata[slot].metadata.finalized &&
            runtime_->current_range_end_ms > runtime_->current_range_start_ms) {
            const size_t index = event_coverage_index(
                runtime_->current_file.inventory.kind);
            merge_coverage(runtime_->result.event_coverage[index].coverage,
                           runtime_->current_range_start_ms,
                           runtime_->current_range_end_ms,
                           runtime_->result.warnings);
        }
    }

    ++status_.files_processed;
    status_.state = EdfDayStatisticsState::SelectFile;
    return true;
}

bool EdfDayStatisticsReader::take_read() {
    if (!runtime_ || !read_port_) return false;

    if (runtime_->prepared.valid()) {
        const StoragePreparedReadView view =
            read_port_->view_prepared(runtime_->prepared);
        if (view.state == PreparedByteReadState::Retry) return false;

        const ReadKind kind = runtime_->read_kind;
        if (view.state != PreparedByteReadState::Data || !view.data) {
            release_read_prepared();
            if (kind == ReadKind::Metadata) {
                runtime_->metadata_slot = -1;
                runtime_->metadata_lookup_complete = true;
                status_.state = EdfDayStatisticsState::SubmitRecord;
                return true;
            }
            runtime_->result.warnings |=
                EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
            status_.state = EdfDayStatisticsState::SelectFile;
            return true;
        }

        bool ok = true;
        if (kind == ReadKind::Header) {
            ok = describe_current_file(view.data, view.length);
            release_read_prepared();
            if (!ok) {
                runtime_->result.warnings |=
                    EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                status_.state = EdfDayStatisticsState::SelectFile;
                return true;
            }

            const bool annotation = annotation_kind(
                runtime_->current_file.inventory.kind);
            if (annotation) {
                if (!resolve_current_annotation_session()) {
                    runtime_->result.warnings |=
                        EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                    status_.state = EdfDayStatisticsState::SelectFile;
                    return true;
                }
            } else {
                if (!register_current_numeric_session()) {
                    runtime_->result.warnings |=
                        EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                    status_.state = EdfDayStatisticsState::SelectFile;
                    return true;
                }
                copy_cstr(runtime_->metadata_lookup_sleep_day,
                          sizeof(runtime_->metadata_lookup_sleep_day),
                          runtime_->current_file.inventory.sleep_day);
                copy_cstr(runtime_->metadata_lookup_session_stamp,
                          sizeof(runtime_->metadata_lookup_session_stamp),
                          runtime_->current_file.inventory.session_stamp);
            }

            runtime_->metadata_slot = metadata_slot_for_current();
            runtime_->metadata_lookup_complete =
                runtime_->metadata_slot >= 0;
            if (runtime_->metadata_lookup_complete) {
                status_.state = EdfDayStatisticsState::SubmitRecord;
                if (!prepare_current_file()) {
                    runtime_->result.warnings |=
                        EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                    status_.state = EdfDayStatisticsState::SelectFile;
                }
                return true;
            }

            size_t free_slot = METADATA_CACHE_MAX;
            for (size_t i = 0; i < METADATA_CACHE_MAX; ++i) {
                if (!runtime_->metadata[i].attempted) {
                    free_slot = i;
                    break;
                }
            }
            if (free_slot == METADATA_CACHE_MAX) {
                runtime_->metadata_lookup_complete = true;
                status_.state = EdfDayStatisticsState::SubmitRecord;
                if (!prepare_current_file()) {
                    runtime_->result.warnings |=
                        EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                    status_.state = EdfDayStatisticsState::SelectFile;
                }
                return true;
            }

            Runtime::MetadataEntry &entry = runtime_->metadata[free_slot];
            entry = {};
            entry.attempted = true;
            copy_cstr(entry.sleep_day,
                      sizeof(entry.sleep_day),
                      runtime_->metadata_lookup_sleep_day);
            copy_cstr(entry.session_stamp,
                      sizeof(entry.session_stamp),
                      runtime_->metadata_lookup_session_stamp);
            runtime_->metadata_slot = static_cast<int>(free_slot);
            snprintf(runtime_->metadata_path,
                     sizeof(runtime_->metadata_path),
                     "%s/%s/%s.bin",
                     EDF_SESSION_METADATA_ROOT,
                     entry.sleep_day,
                     entry.session_stamp);
            runtime_->metadata_lookup_complete = false;
            status_.state = EdfDayStatisticsState::SubmitMetadata;
            return true;
        }

        if (kind == ReadKind::Metadata) {
            const int slot = runtime_->metadata_slot;
            if (slot >= 0 &&
                EdfSessionMetadataCodec::decode(view.data,
                                                view.length,
                                                runtime_->metadata[slot].metadata)) {
                runtime_->metadata[slot].available = true;
                runtime_->result.provenance_used = true;
                append_provenance(runtime_->metadata[slot].metadata);
            } else {
                runtime_->result.warnings |=
                    EDF_DAY_STATISTICS_WARNING_METADATA_INVALID;
            }
            release_read_prepared();
            runtime_->metadata_lookup_complete = true;
            status_.state = EdfDayStatisticsState::SubmitRecord;
            if (!prepare_current_file()) {
                runtime_->result.warnings |=
                    EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                status_.state = EdfDayStatisticsState::SelectFile;
            }
            return true;
        }

        ok = decode_current_record(view.data, view.length);
        release_read_prepared();
        if (!ok) {
            runtime_->result.warnings |= EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
            status_.state = EdfDayStatisticsState::SelectFile;
            return true;
        }

        ++runtime_->record_index;
        ++runtime_->result.records_processed;
        status_.records_processed = runtime_->result.records_processed;
        if (runtime_->record_index >=
            runtime_->current_file.inventory.complete_records_from_size) {
            return finish_current_file();
        }
        status_.state = EdfDayStatisticsState::SubmitRecord;
        return true;
    }

    if (!runtime_->read_ticket.valid()) return false;
    StorageReadCompletion completion;
    if (!read_port_->take_completion(runtime_->read_ticket, completion)) {
        return false;
    }
    runtime_->read_ticket = {};
    if (completion.outcome.disposition == OperationDisposition::Cancelled) {
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        status_.state = EdfDayStatisticsState::Cancelled;
        return true;
    }
    if (completion.outcome.disposition != OperationDisposition::Succeeded ||
        !completion.prepared.valid()) {
        const ReadKind kind = runtime_->read_kind;
        if (kind == ReadKind::Metadata &&
            strcmp(completion.error, "read_not_found") == 0) {
            runtime_->metadata_lookup_complete = true;
            status_.state = EdfDayStatisticsState::SubmitRecord;
            if (!prepare_current_file()) {
                runtime_->result.warnings |=
                    EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
                status_.state = EdfDayStatisticsState::SelectFile;
            }
            return true;
        }
        if (completion.prepared.valid()) {
            read_port_->release_prepared(completion.prepared);
        }
        if (kind == ReadKind::Header || kind == ReadKind::Record) {
            runtime_->result.warnings |=
                EDF_DAY_STATISTICS_WARNING_FILE_SKIPPED;
            status_.state = EdfDayStatisticsState::SelectFile;
            return true;
        }
        fail(EdfDayStatisticsError::ReadFailed,
             completion.error[0] ? completion.error : "read_failed");
        return true;
    }

    runtime_->prepared = completion.prepared;
    return true;
}

bool EdfDayStatisticsReader::finish_cancel() {
    if (!runtime_) return false;
    if (runtime_->scan_ticket.valid()) {
        if (scan_port_ && scan_port_->abandon(runtime_->scan_ticket)) {
            runtime_->scan_ticket = {};
        }
        return true;
    }
    if (runtime_->read_ticket.valid()) {
        if (read_port_ && read_port_->abandon(runtime_->read_ticket)) {
            runtime_->read_ticket = {};
        } else if (read_port_) {
            (void)read_port_->cancel(runtime_->read_ticket);
        }
        return true;
    }
    if (runtime_->prepared.valid()) release_read_prepared();
    status_.state = EdfDayStatisticsState::Cancelled;
    return true;
}

void EdfDayStatisticsReader::complete() {
    if (!runtime_) return;
    runtime_->finalize_signal_index = 0;
    runtime_->finalize_bin_index = 0;
    runtime_->finalize_signal_initialized = false;
    status_.state = EdfDayStatisticsState::Finalize;
}

bool EdfDayStatisticsReader::finish_statistics_step() {
    if (!runtime_ || status_.state != EdfDayStatisticsState::Finalize) {
        return false;
    }
    if (runtime_->finalize_signal_index >= runtime_->result.signal_count) {
        status_.state = EdfDayStatisticsState::Complete;
        return true;
    }

    const size_t signal_index = runtime_->finalize_signal_index;
    EdfDaySignalStatistics &statistics =
        runtime_->result.signals[signal_index];
    const Histogram &histogram = runtime_->histograms[signal_index];

    if (!runtime_->finalize_signal_initialized) {
        runtime_->finalize_signal_initialized = true;
        runtime_->finalize_bin_index = 0;
        runtime_->finalize_cumulative = 0;
        memset(runtime_->finalize_ranks,
               0,
               sizeof(runtime_->finalize_ranks));
        memset(runtime_->finalize_upper_ranks,
               0,
               sizeof(runtime_->finalize_upper_ranks));
        memset(runtime_->finalize_remainders,
               0,
               sizeof(runtime_->finalize_remainders));
        memset(runtime_->finalize_lower_raw,
               0,
               sizeof(runtime_->finalize_lower_raw));
        memset(runtime_->finalize_upper_raw,
               0,
               sizeof(runtime_->finalize_upper_raw));
        memset(runtime_->finalize_lower_found,
               0,
               sizeof(runtime_->finalize_lower_found));
        memset(runtime_->finalize_upper_found,
               0,
               sizeof(runtime_->finalize_upper_found));

        if (!histogram.counts || histogram.bin_count == 0 ||
            statistics.weighted_duration_ms == 0) {
            statistics.valid = false;
            ++runtime_->finalize_signal_index;
            runtime_->finalize_signal_initialized = false;
            if (runtime_->finalize_signal_index >=
                runtime_->result.signal_count) {
                status_.state = EdfDayStatisticsState::Complete;
            }
            return true;
        }

        constexpr uint32_t percentiles[] = {0, 5, 50, 70, 95, 100};
        for (size_t i = 0; i < 6; ++i) {
            const uint64_t numerator =
                (statistics.weighted_duration_ms - 1) *
                static_cast<uint64_t>(percentiles[i]);
            runtime_->finalize_ranks[i] = numerator / 100;
            runtime_->finalize_remainders[i] =
                static_cast<uint32_t>(numerator % 100);
            runtime_->finalize_upper_ranks[i] =
                runtime_->finalize_ranks[i] +
                (runtime_->finalize_remainders[i] != 0 ? 1 : 0);
        }
    }

    const uint32_t end_bin = std::min<uint32_t>(
        histogram.bin_count,
        runtime_->finalize_bin_index + FINALIZE_BINS_PER_POLL);
    for (uint32_t bin = runtime_->finalize_bin_index;
         bin < end_bin;
         ++bin) {
        const uint32_t weight = histogram.counts[bin];
        if (weight == 0) continue;

        const uint64_t next = runtime_->finalize_cumulative + weight;
        for (size_t i = 0; i < 6; ++i) {
            const int32_t raw = histogram.raw_min +
                    static_cast<int32_t>(bin);
            if (!runtime_->finalize_lower_found[i] &&
                runtime_->finalize_ranks[i] < next) {
                runtime_->finalize_lower_raw[i] = raw;
                runtime_->finalize_lower_found[i] = true;
            }
            if (!runtime_->finalize_upper_found[i] &&
                runtime_->finalize_upper_ranks[i] < next) {
                runtime_->finalize_upper_raw[i] = raw;
                runtime_->finalize_upper_found[i] = true;
            }
        }
        runtime_->finalize_cumulative = next;
    }
    runtime_->finalize_bin_index = end_bin;
    if (end_bin < histogram.bin_count) return true;

    bool complete = true;
    for (size_t i = 0; i < 6; ++i) {
        complete = complete && runtime_->finalize_lower_found[i] &&
            runtime_->finalize_upper_found[i];
    }
    if (complete) {
        const auto interpolate = [&](size_t percentile_index) {
            const int32_t lower = canonical_value_milli(
                statistics,
                static_cast<int16_t>(
                    runtime_->finalize_lower_raw[percentile_index]));
            if (runtime_->finalize_remainders[percentile_index] == 0) {
                return lower;
            }

            const int32_t upper = canonical_value_milli(
                statistics,
                static_cast<int16_t>(
                    runtime_->finalize_upper_raw[percentile_index]));
            return lower + static_cast<int32_t>(
                (static_cast<int64_t>(upper - lower) *
                 runtime_->finalize_remainders[percentile_index]) / 100);
        };

        statistics.min_milli = canonical_value_milli(
            statistics,
            static_cast<int16_t>(runtime_->finalize_lower_raw[0]));
        statistics.p5_milli = interpolate(1);
        statistics.p50_milli = interpolate(2);
        statistics.p70_milli = interpolate(3);
        statistics.p95_milli = interpolate(4);
        statistics.max_milli = canonical_value_milli(
            statistics,
            static_cast<int16_t>(runtime_->finalize_lower_raw[5]));
    }
    statistics.valid = complete;

    ++runtime_->finalize_signal_index;
    runtime_->finalize_signal_initialized = false;
    if (runtime_->finalize_signal_index >= runtime_->result.signal_count) {
        status_.state = EdfDayStatisticsState::Complete;
    }
    return true;
}

bool EdfDayStatisticsReader::finish_statistics() {
    if (!runtime_) return false;
    if (status_.state == EdfDayStatisticsState::Complete) return true;
    return finish_statistics_step();
}

bool EdfDayStatisticsReader::add_fallback_segment(
    EdfDaySignalStatistics &statistics,
    void *histogram_pointer,
    int64_t start_ms,
    int64_t end_ms,
    float original_physical) {
    if (end_ms <= start_ms || !isfinite(original_physical) ||
        !histogram_pointer || !isfinite(statistics.scale.scale) ||
        statistics.scale.scale <= 0.0f) {
        return false;
    }

    Histogram &histogram = *static_cast<Histogram *>(histogram_pointer);
    const float raw_float = (original_physical - statistics.scale.offset) /
        statistics.scale.scale;
    if (!isfinite(raw_float)) return false;

    const float clamped = std::max<float>(INT16_MIN,
                                          std::min<float>(INT16_MAX,
                                                          raw_float));
    const int32_t raw = static_cast<int32_t>(lroundf(clamped));
    const uint64_t duration = static_cast<uint64_t>(end_ms - start_ms);
    const uint32_t weight = static_cast<uint32_t>(std::min<uint64_t>(
        duration,
        UINT32_MAX));
    add_histogram_weight(histogram, raw, weight);

    if (statistics.sample_count != UINT64_MAX) ++statistics.sample_count;
    statistics.weighted_duration_ms =
        UINT64_MAX - statistics.weighted_duration_ms < duration
            ? UINT64_MAX
            : statistics.weighted_duration_ms + duration;
    statistics.includes_fallback = true;
    statistics.source_mask |= source_bit(ReportSourceId::TherapyOneMinute);
    merge_coverage(statistics.coverage,
                   start_ms,
                   end_ms,
                   runtime_->result.warnings);
    statistics.valid = false;
    return !statistics.coverage.truncated;
}

bool EdfDayStatisticsReader::add_fallback_sample(
    ReportSignalId signal,
    float original_physical,
    int64_t start_ms,
    uint32_t duration_ms) {
    if (!runtime_ ||
        (status_.state != EdfDayStatisticsState::Complete &&
         status_.state != EdfDayStatisticsState::Finalize) ||
        duration_ms == 0 || !isfinite(original_physical) ||
        start_ms > INT64_MAX - static_cast<int64_t>(duration_ms)) {
        return false;
    }

    const int signal_index = ensure_fallback_signal(runtime_->result,
                                                    runtime_->histograms,
                                                    signal);
    if (signal_index < 0) return false;
    EdfDaySignalStatistics &statistics =
        runtime_->result.signals[signal_index];
    if (statistics.coverage.truncated) return false;

    const int64_t requested_end = start_ms + duration_ms;
    const int64_t clipped_start = std::max(
        start_ms,
        runtime_->result.window_start_ms);
    const int64_t clipped_end = std::min(
        requested_end,
        runtime_->result.window_end_ms);
    if (clipped_end <= clipped_start) return true;

    const EdfDayCoverage coverage_snapshot = statistics.coverage;
    int64_t cursor = clipped_start;
    for (size_t i = 0; i < coverage_snapshot.count; ++i) {
        const EdfDayCoverageInterval &interval =
            coverage_snapshot.intervals[i];
        if (interval.end_ms <= cursor) continue;
        if (interval.start_ms >= clipped_end) break;

        if (interval.start_ms > cursor &&
            !add_fallback_segment(statistics,
                                  &runtime_->histograms[signal_index],
                                  cursor,
                                  std::min(interval.start_ms, clipped_end),
                                  original_physical)) {
            return false;
        }
        cursor = std::max(cursor, interval.end_ms);
        if (cursor >= clipped_end) break;
    }
    if (cursor < clipped_end &&
        !add_fallback_segment(statistics,
                              &runtime_->histograms[signal_index],
                              cursor,
                              clipped_end,
                              original_physical)) {
        return false;
    }

    if (statistics.includes_fallback) {
        runtime_->finalize_signal_index = 0;
        status_.state = EdfDayStatisticsState::Finalize;
    }
    return true;
}

void EdfDayStatisticsReader::fail(EdfDayStatisticsError error,
                                  const char *text) {
    status_.error = error;
    copy_cstr(status_.error_text, sizeof(status_.error_text), text);
    status_.state = EdfDayStatisticsState::Failed;
}

void EdfDayStatisticsReader::release_read_prepared() {
    if (!runtime_ || !runtime_->prepared.valid() || !read_port_) return;
    read_port_->release_prepared(runtime_->prepared);
    runtime_->prepared = {};
}

void EdfDayStatisticsReader::clear_read_ticket() {
    if (!runtime_) return;
    if (runtime_->prepared.valid()) release_read_prepared();
    if (!runtime_->read_ticket.valid() || !read_port_) return;
    if (read_port_->abandon(runtime_->read_ticket)) {
        runtime_->read_ticket = {};
    }
}

void EdfDayStatisticsReader::cancel() {
    if (!runtime_ || !status_.active()) return;
    runtime_->cancel_requested = true;
}

void EdfDayStatisticsReader::reset() {
    if (!runtime_) {
        status_ = {};
        return;
    }
    clear_read_ticket();
    if (runtime_->scan_ticket.valid() && scan_port_) {
        if (scan_port_->abandon(runtime_->scan_ticket)) {
            runtime_->scan_ticket = {};
        }
    }
    clear_histograms(runtime_->histograms,
                     AC_EDF_DAY_STATISTICS_SIGNAL_MAX);
    runtime_->~Runtime();
    new (runtime_) Runtime();
    status_ = {};
}

bool EdfDayStatisticsReader::poll() {
    if (!runtime_ || !status_.active()) return false;
    if (runtime_->cancel_requested) return finish_cancel();

    switch (status_.state) {
        case EdfDayStatisticsState::SubmitScan: return submit_scan();
        case EdfDayStatisticsState::WaitScan: return take_scan();
        case EdfDayStatisticsState::SelectFile: return select_file();
        case EdfDayStatisticsState::SubmitHeader: return submit_header();
        case EdfDayStatisticsState::WaitHeader:
        case EdfDayStatisticsState::WaitMetadata:
        case EdfDayStatisticsState::WaitRecord:
            return take_read();
        case EdfDayStatisticsState::SubmitMetadata: return submit_metadata();
        case EdfDayStatisticsState::SubmitRecord: return submit_record();
        case EdfDayStatisticsState::Finalize: return finish_statistics();
        case EdfDayStatisticsState::Idle:
        case EdfDayStatisticsState::Complete:
        case EdfDayStatisticsState::Failed:
        case EdfDayStatisticsState::Cancelled:
            return false;
    }
    return false;
}

bool edf_day_statistics_map_raw_time(const EdfDayStatisticsResult &result,
                                     int64_t raw_ms,
                                     int64_t &canonical_ms) {
    const EdfSessionMetadata *match = nullptr;
    for (size_t i = 0; i < result.provenance_session_count; ++i) {
        const EdfSessionMetadata &metadata = result.provenance_sessions[i];
        if (raw_ms < metadata.raw_segment_start_ms ||
            (metadata.raw_segment_end_ms != 0 &&
             raw_ms > metadata.raw_segment_end_ms)) {
            continue;
        }
        if (!match || metadata.raw_segment_start_ms >
                          match->raw_segment_start_ms) {
            match = &metadata;
        }
    }
    if (!match) return false;

    const int64_t correction = match->canonical_segment_start_ms -
        match->raw_segment_start_ms;
    if ((correction > 0 && raw_ms > INT64_MAX - correction) ||
        (correction < 0 && raw_ms < INT64_MIN - correction)) {
        return false;
    }
    canonical_ms = raw_ms + correction;
    return true;
}

}  // namespace aircannect
