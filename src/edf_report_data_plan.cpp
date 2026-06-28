#include "edf_report_data_plan.h"

#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t EDF_REPORT_PLAN_TARGET_BYTES = 512 * 1024;

const EdfReportSessionFileDescriptor *session_file(
    const EdfReportSessionDescriptor &session,
    EdfInventoryFileKind kind,
    uint8_t &slot_out) {
    const size_t slot = edf_report_session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return nullptr;
    const uint32_t mask = edf_report_file_kind_mask(kind);
    if ((session.file_mask & mask) == 0) return nullptr;
    const EdfReportSessionFileDescriptor &file = session.files[slot];
    if (file.kind != kind || !file.path[0]) return nullptr;
    slot_out = static_cast<uint8_t>(slot);
    return &file;
}

bool numeric_record_window(const EdfReportSessionFileDescriptor &file,
                           int64_t range_start_ms,
                           int64_t range_end_ms,
                           uint32_t &first_record,
                           uint32_t &end_record) {
    first_record = 0;
    end_record = 0;
    if (range_end_ms <= range_start_ms || file.header_start_ms <= 0 ||
        file.record_duration_ms == 0 || file.complete_records == 0) {
        return false;
    }

    const int64_t duration = static_cast<int64_t>(file.record_duration_ms);
    int64_t first = 0;
    if (range_start_ms > file.header_start_ms) {
        first = (range_start_ms - file.header_start_ms) / duration;
    }
    int64_t end = static_cast<int64_t>(file.complete_records);
    if (range_end_ms > file.header_start_ms) {
        const int64_t delta = range_end_ms - file.header_start_ms;
        end = (delta + duration - 1) / duration;
    } else {
        end = 0;
    }

    if (first < 0) first = 0;
    if (end < 0) end = 0;
    if (first > static_cast<int64_t>(file.complete_records)) {
        first = static_cast<int64_t>(file.complete_records);
    }
    if (end > static_cast<int64_t>(file.complete_records)) {
        end = static_cast<int64_t>(file.complete_records);
    }
    if (end <= first) return false;
    first_record = static_cast<uint32_t>(first);
    end_record = static_cast<uint32_t>(end);
    return true;
}

uint32_t records_per_plan_entry(const EdfReportSessionFileDescriptor &file) {
    if (file.record_size == 0) return 1;
    uint32_t records = EDF_REPORT_PLAN_TARGET_BYTES / file.record_size;
    if (records == 0) records = 1;
    return records;
}

uint32_t samples_per_record(const EdfReportSessionFileDescriptor &file,
                            uint32_t sample_interval_ms) {
    if (file.record_duration_ms == 0 || sample_interval_ms == 0) return 0;
    return file.record_duration_ms / sample_interval_ms;
}

uint32_t clamp_u64_to_u32(uint64_t value) {
    return value > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(value);
}

int64_t ceil_sample_index(int64_t base_ms,
                          uint32_t interval_ms,
                          int64_t timestamp_ms) {
    if (interval_ms == 0 || timestamp_ms <= base_ms) return 0;
    const int64_t delta = timestamp_ms - base_ms;
    return (delta + static_cast<int64_t>(interval_ms) - 1) /
           static_cast<int64_t>(interval_ms);
}

bool numeric_sample_window(const EdfReportSessionFileDescriptor &file,
                           uint32_t sample_interval_ms,
                           uint32_t first_record,
                           uint32_t record_count,
                           int64_t range_start_ms,
                           int64_t range_end_ms,
                           int64_t &sample_start_ms,
                           int64_t &sample_end_ms,
                           uint32_t &sample_count) {
    sample_start_ms = 0;
    sample_end_ms = 0;
    sample_count = 0;
    if (range_end_ms <= range_start_ms || file.header_start_ms <= 0 ||
        sample_interval_ms == 0 || record_count == 0) {
        return false;
    }

    const uint32_t samples = samples_per_record(file, sample_interval_ms);
    if (samples == 0) return false;
    const int64_t samples_i = static_cast<int64_t>(samples);
    if (static_cast<uint64_t>(first_record) >
        static_cast<uint64_t>(INT64_MAX / samples_i)) {
        return false;
    }

    const int64_t record_first =
        static_cast<int64_t>(first_record) * samples_i;
    if (static_cast<uint64_t>(record_count) >
        static_cast<uint64_t>((INT64_MAX - record_first) / samples_i)) {
        return false;
    }
    const int64_t record_end =
        record_first + static_cast<int64_t>(record_count) * samples_i;
    int64_t first =
        ceil_sample_index(file.header_start_ms,
                          sample_interval_ms,
                          range_start_ms);
    int64_t end =
        ceil_sample_index(file.header_start_ms,
                          sample_interval_ms,
                          range_end_ms);
    if (first < record_first) first = record_first;
    if (end > record_end) end = record_end;
    if (end <= first) return false;

    const int64_t interval = static_cast<int64_t>(sample_interval_ms);
    if (first > (INT64_MAX - file.header_start_ms) / interval ||
        end > (INT64_MAX - file.header_start_ms) / interval) {
        return false;
    }
    const int64_t count = end - first;
    if (count <= 0 || count > UINT32_MAX) return false;
    sample_start_ms = file.header_start_ms + first * interval;
    sample_end_ms = file.header_start_ms + end * interval;
    sample_count = static_cast<uint32_t>(count);
    return sample_end_ms > sample_start_ms;
}

bool emit_numeric_entries(const EdfReportSessionFileDescriptor &file,
                          uint8_t file_slot,
                          const EdfReportSignalMappingDef &mapping,
                          int64_t range_start_ms,
                          int64_t range_end_ms,
                          EdfReportDataPlanCallback callback,
                          void *context) {
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    if (!numeric_record_window(file,
                               range_start_ms,
                               range_end_ms,
                               first_record,
                               end_record)) {
        return true;
    }

    const uint32_t per_entry = records_per_plan_entry(file);
    int64_t range_sample_start_ms = 0;
    int64_t range_sample_end_ms = 0;
    uint32_t range_sample_count = 0;
    if (!numeric_sample_window(file,
                               mapping.sample_interval_ms,
                               first_record,
                               end_record - first_record,
                               range_start_ms,
                               range_end_ms,
                               range_sample_start_ms,
                               range_sample_end_ms,
                               range_sample_count)) {
        return true;
    }
    const char *name = report_signal_store_name(mapping.signal);
    for (uint32_t record = first_record; record < end_record;) {
        uint32_t count = end_record - record;
        if (count > per_entry) count = per_entry;

        int64_t sample_start_ms = 0;
        int64_t sample_end_ms = 0;
        uint32_t sample_count = 0;
        if (!numeric_sample_window(file,
                                   mapping.sample_interval_ms,
                                   record,
                                   count,
                                   range_start_ms,
                                   range_end_ms,
                                   sample_start_ms,
                                   sample_end_ms,
                                   sample_count)) {
            record += count;
            continue;
        }

        EdfReportDataPlanEntry entry;
        entry.kind = EdfReportDataKind::Series;
        entry.signal = mapping.signal;
        entry.source = mapping.source;
        entry.name = name;
        entry.file_kind = mapping.kind;
        entry.file_slot = file_slot;
        copy_cstr(entry.signal_label,
                  sizeof(entry.signal_label),
                  edf_report_signal_mapping_label(mapping));
        entry.first_record = record;
        entry.record_count = count;
        entry.start_ms = sample_start_ms;
        entry.end_ms = sample_end_ms;
        entry.sample_interval_ms = mapping.sample_interval_ms;
        entry.record_count_estimate = sample_count;
        const size_t max_missing_bitmap_bytes =
            (static_cast<size_t>(sample_count) + 7u) / 8u;
        entry.payload_len_estimate = clamp_u64_to_u32(
            report_series_v2_uniform_wire_size(sample_count,
                                               max_missing_bitmap_bytes));
        entry.primary = mapping.primary;
        entry.trim_leading_padding = sample_start_ms == range_sample_start_ms;
        entry.trim_trailing_padding = sample_end_ms == range_sample_end_ms;
        if (!callback(context, entry)) return false;
        record += count;
    }
    return true;
}

struct PlanCountContext {
    uint32_t entries = 0;
};

struct PlanCoverageContext {
    int64_t covered_until_ms = 0;
    bool advanced = false;
};

bool count_plan_entry(void *context, const EdfReportDataPlanEntry &entry) {
    PlanCountContext *ctx = static_cast<PlanCountContext *>(context);
    if (!ctx || !entry.name || !entry.name[0]) return false;
    ctx->entries++;
    return true;
}

bool extend_plan_coverage(void *context, const EdfReportDataPlanEntry &entry) {
    PlanCoverageContext *ctx = static_cast<PlanCoverageContext *>(context);
    if (!ctx || !entry.name || !entry.name[0] ||
        entry.end_ms <= entry.start_ms) {
        return false;
    }
    if (entry.end_ms <= ctx->covered_until_ms) return true;
    if (entry.start_ms >
        ctx->covered_until_ms + AC_EDF_REPORT_COVERAGE_TOLERANCE_MS) {
        return true;
    }
    ctx->covered_until_ms = entry.end_ms;
    ctx->advanced = true;
    return true;
}

bool signal_covers_range(const EdfReportSessionDescriptor *sessions,
                         size_t session_count,
                         ReportSignalId signal,
                         const EdfReportRequiredRange &range) {
    if (!sessions || session_count == 0 || range.end_ms <= range.start_ms) {
        return false;
    }

    int64_t covered_until = range.start_ms;
    while (covered_until + AC_EDF_REPORT_COVERAGE_TOLERANCE_MS <
           range.end_ms) {
        PlanCoverageContext ctx;
        ctx.covered_until_ms = covered_until;
        for (size_t i = 0; i < session_count; ++i) {
            if (!edf_report_plan_signal(sessions[i],
                                        signal,
                                        range.start_ms,
                                        range.end_ms,
                                        extend_plan_coverage,
                                        &ctx)) {
                return false;
            }
        }
        if (!ctx.advanced) return false;
        covered_until = ctx.covered_until_ms;
    }
    return true;
}

bool emit_event_file_entry(const EdfReportSessionDescriptor &session,
                           EdfInventoryFileKind kind,
                           const char *name,
                           int64_t range_start_ms,
                           int64_t range_end_ms,
                           EdfReportDataPlanCallback callback,
                           void *context) {
    uint8_t slot = 0;
    const EdfReportSessionFileDescriptor *file =
        session_file(session, kind, slot);
    if (!file || file->complete_records == 0) return true;

    EdfReportDataPlanEntry entry;
    entry.kind = EdfReportDataKind::Events;
    entry.signal = ReportSignalId::Flow;
    entry.source = ReportSourceId::RespiratoryEvents;
    entry.name = name;
    entry.file_kind = kind;
    entry.file_slot = slot;
    entry.first_record = 0;
    entry.record_count = file->complete_records;
    entry.start_ms = range_start_ms;
    entry.end_ms = range_end_ms;
    entry.record_count_estimate = file->complete_records;
    entry.payload_len_estimate =
        clamp_u64_to_u32(static_cast<uint64_t>(file->record_size) *
                         file->complete_records);
    return callback(context, entry);
}

}  // namespace

bool edf_report_session_has_file(const EdfReportSessionDescriptor &session,
                                 EdfInventoryFileKind kind) {
    uint8_t slot = 0;
    return session_file(session, kind, slot) != nullptr;
}

bool edf_report_plan_events(const EdfReportSessionDescriptor &session,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context) {
    if (!callback || range_end_ms <= range_start_ms) return false;
    if (!emit_event_file_entry(
            session,
            EdfInventoryFileKind::Eve,
            report_source_spool_type(ReportSourceId::RespiratoryEvents),
            range_start_ms,
            range_end_ms,
            callback,
            context)) {
        return false;
    }
    return emit_event_file_entry(session,
                                 EdfInventoryFileKind::Csl,
                                 "TherapyEvents-CSR",
                                 range_start_ms,
                                 range_end_ms,
                                 callback,
                                 context);
}

bool edf_report_plan_signal(const EdfReportSessionDescriptor &session,
                            ReportSignalId signal,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context) {
    if (!callback || range_end_ms <= range_start_ms) return false;
    const EdfReportSignalMappingDef *mapping =
        edf_report_session_select_signal_mapping(session, signal);
    if (!mapping) return true;

    uint8_t slot = 0;
    const EdfReportSessionFileDescriptor *file =
        session_file(session, mapping->kind, slot);
    if (!file) return true;
    return emit_numeric_entries(*file,
                                slot,
                                *mapping,
                                range_start_ms,
                                range_end_ms,
                                callback,
                                context);
}

namespace {

struct FindSignalEntryContext {
    EdfInventoryFileKind file_kind = EdfInventoryFileKind::Unknown;
    uint8_t file_slot = 0;
    uint32_t first_record = 0;
    uint32_t record_count = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    EdfReportDataPlanEntry *out = nullptr;
    bool found = false;
};

bool find_signal_entry_callback(void *context,
                                const EdfReportDataPlanEntry &entry) {
    FindSignalEntryContext *ctx =
        static_cast<FindSignalEntryContext *>(context);
    if (!ctx || !ctx->out) return false;
    if (entry.kind == EdfReportDataKind::Series &&
        entry.file_kind == ctx->file_kind &&
        entry.file_slot == ctx->file_slot &&
        entry.first_record == ctx->first_record &&
        entry.record_count == ctx->record_count &&
        entry.start_ms == ctx->start_ms &&
        entry.end_ms == ctx->end_ms) {
        *ctx->out = entry;
        ctx->found = true;
        return false;
    }
    return true;
}

}  // namespace

bool edf_report_find_signal_entry_for_chunk(
    const EdfReportSessionDescriptor &session,
    ReportSignalId signal,
    EdfInventoryFileKind file_kind,
    uint8_t file_slot,
    uint32_t first_record,
    uint32_t record_count,
    int64_t start_ms,
    int64_t end_ms,
    EdfReportDataPlanEntry &out) {
    out = EdfReportDataPlanEntry();
    FindSignalEntryContext ctx;
    ctx.file_kind = file_kind;
    ctx.file_slot = file_slot;
    ctx.first_record = first_record;
    ctx.record_count = record_count;
    ctx.start_ms = start_ms;
    ctx.end_ms = end_ms;
    ctx.out = &out;
    (void)edf_report_plan_signal(session,
                                 signal,
                                 start_ms,
                                 end_ms,
                                 find_signal_entry_callback,
                                 &ctx);
    return ctx.found;
}

bool edf_report_plan_signal_covers_ranges(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportSignalId signal,
    const EdfReportRequiredRange *ranges,
    size_t range_count) {
    if (!sessions || session_count == 0 || !ranges || range_count == 0) {
        return false;
    }
    for (size_t i = 0; i < range_count; ++i) {
        if (!signal_covers_range(sessions, session_count, signal, ranges[i])) {
            return false;
        }
    }
    return true;
}

bool edf_report_plan_covers_report(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    const EdfReportRequiredRange *ranges,
    size_t range_count,
    EdfReportDataCoverage *coverage_out) {
    if (coverage_out) *coverage_out = EdfReportDataCoverage();
    if (!sessions || session_count == 0 || !ranges || range_count == 0) {
        return false;
    }

    EdfReportDataCoverage coverage;
    for (size_t session_index = 0; session_index < session_count;
         ++session_index) {
        if (edf_report_session_has_file(sessions[session_index],
                                        EdfInventoryFileKind::Eve)) {
            coverage.scored_event_sources++;
        }
        for (size_t range_index = 0; range_index < range_count; ++range_index) {
            const EdfReportRequiredRange &range = ranges[range_index];
            if (range.end_ms <= range.start_ms) continue;
            PlanCountContext ctx;
            if (!edf_report_plan_events(sessions[session_index],
                                        range.start_ms,
                                        range.end_ms,
                                        count_plan_entry,
                                        &ctx)) {
                return false;
            }
            coverage.event_entries += ctx.entries;
        }
    }
    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t signal_index = 0; signal_index < signal_count; ++signal_index) {
        const ReportSignalDef &signal = signals[signal_index];
        if (!report_signal_required_for_result(signal)) continue;
        coverage.signals_required++;
        if (!edf_report_plan_signal_covers_ranges(sessions,
                                                  session_count,
                                                  signal.id,
                                                  ranges,
                                                  range_count)) {
            if (coverage_out) *coverage_out = coverage;
            return false;
        }
        coverage.signals_covered++;
    }

    if (coverage_out) *coverage_out = coverage;
    return true;
}

}  // namespace aircannect
