#include "edf_report_data_plan.h"

#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t EDF_REPORT_PLAN_TARGET_BYTES = 64 * 1024;

size_t session_file_slot(EdfInventoryFileKind kind) {
    switch (kind) {
        case EdfInventoryFileKind::Brp: return 0;
        case EdfInventoryFileKind::Pld: return 1;
        case EdfInventoryFileKind::Sa2: return 2;
        case EdfInventoryFileKind::Eve: return 3;
        case EdfInventoryFileKind::Csl: return 4;
        default:
            return AC_EDF_REPORT_SESSION_FILE_MAX;
    }
}

uint32_t signal_bit(ReportSignalId signal) {
    const uint8_t index = static_cast<uint8_t>(signal);
    if (index >= 32) return 0;
    return 1u << index;
}

const EdfReportSessionFileDescriptor *session_file(
    const EdfReportSessionDescriptor &session,
    EdfInventoryFileKind kind,
    uint8_t &slot_out) {
    const size_t slot = session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return nullptr;
    const uint32_t mask = edf_report_file_kind_mask(kind);
    if ((session.file_mask & mask) == 0) return nullptr;
    const EdfReportSessionFileDescriptor &file = session.files[slot];
    if (file.kind != kind || !file.path[0]) return nullptr;
    slot_out = static_cast<uint8_t>(slot);
    return &file;
}

bool mapping_available(const EdfReportSessionDescriptor &session,
                       const EdfReportSignalMappingDef &mapping,
                       uint32_t bit) {
    if (bit == 0) return false;
    uint8_t slot = 0;
    if (!session_file(session, mapping.kind, slot)) return false;
    const uint32_t mask = mapping.primary ? session.primary_signal_mask
                                          : session.fallback_signal_mask;
    return (mask & bit) != 0;
}

const EdfReportSignalMappingDef *select_signal_mapping(
    const EdfReportSessionDescriptor &session,
    ReportSignalId signal) {
    size_t count = 0;
    const EdfReportSignalMappingDef *mappings =
        edf_report_signal_mapping_defs(count);
    const uint32_t bit = signal_bit(signal);
    for (size_t pass = 0; pass < 2; ++pass) {
        const bool want_primary = pass == 0;
        for (size_t i = 0; i < count; ++i) {
            const EdfReportSignalMappingDef &mapping = mappings[i];
            if (mapping.signal != signal || mapping.primary != want_primary) {
                continue;
            }
            if (mapping_available(session, mapping, bit)) return &mapping;
        }
    }
    return nullptr;
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
    const uint32_t samples = samples_per_record(file,
                                                mapping.sample_interval_ms);
    const char *name = report_signal_store_name(mapping.signal);
    for (uint32_t record = first_record; record < end_record;) {
        uint32_t count = end_record - record;
        if (count > per_entry) count = per_entry;

        EdfReportDataPlanEntry entry;
        entry.kind = EdfReportDataKind::Series;
        entry.signal = mapping.signal;
        entry.source = mapping.source;
        entry.name = name;
        entry.file_kind = mapping.kind;
        entry.file_slot = file_slot;
        copy_cstr(entry.signal_label,
                  sizeof(entry.signal_label),
                  mapping.label);
        entry.first_record = record;
        entry.record_count = count;
        entry.start_ms = file.header_start_ms +
                         static_cast<int64_t>(record) *
                             static_cast<int64_t>(file.record_duration_ms);
        entry.end_ms = entry.start_ms +
                       static_cast<int64_t>(count) *
                           static_cast<int64_t>(file.record_duration_ms);
        entry.sample_interval_ms = mapping.sample_interval_ms;
        entry.record_count_estimate =
            clamp_u64_to_u32(static_cast<uint64_t>(count) * samples);
        entry.payload_len_estimate = clamp_u64_to_u32(
            static_cast<uint64_t>(entry.record_count_estimate) *
            report_series_sample_wire_size());
        entry.primary = mapping.primary;
        if (!callback(context, entry)) return false;
        record += count;
    }
    return true;
}

struct PlanCountContext {
    uint32_t entries = 0;
};

bool count_plan_entry(void *context, const EdfReportDataPlanEntry &entry) {
    PlanCountContext *ctx = static_cast<PlanCountContext *>(context);
    if (!ctx || !entry.name || !entry.name[0]) return false;
    ctx->entries++;
    return true;
}

}  // namespace

bool edf_report_plan_events(const EdfReportSessionDescriptor &session,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context) {
    if (!callback || range_end_ms <= range_start_ms) return false;
    uint8_t slot = 0;
    const EdfReportSessionFileDescriptor *file =
        session_file(session, EdfInventoryFileKind::Eve, slot);
    if (!file || file->complete_records == 0) return true;

    EdfReportDataPlanEntry entry;
    entry.kind = EdfReportDataKind::Events;
    entry.signal = ReportSignalId::Flow;
    entry.source = ReportSourceId::RespiratoryEvents;
    entry.name = report_source_spool_type(ReportSourceId::RespiratoryEvents);
    entry.file_kind = EdfInventoryFileKind::Eve;
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

bool edf_report_plan_signal(const EdfReportSessionDescriptor &session,
                            ReportSignalId signal,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context) {
    if (!callback || range_end_ms <= range_start_ms) return false;
    const EdfReportSignalMappingDef *mapping =
        select_signal_mapping(session, signal);
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

bool edf_report_plan_covers_report(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportDataCoverage *coverage_out) {
    if (coverage_out) *coverage_out = EdfReportDataCoverage();
    if (!sessions || session_count == 0 || range_end_ms <= range_start_ms) {
        return false;
    }

    EdfReportDataCoverage coverage;
    for (size_t i = 0; i < session_count; ++i) {
        PlanCountContext ctx;
        if (!edf_report_plan_events(sessions[i],
                                    range_start_ms,
                                    range_end_ms,
                                    count_plan_entry,
                                    &ctx)) {
            return false;
        }
        coverage.event_entries += ctx.entries;
    }
    if (coverage.event_entries == 0) {
        if (coverage_out) *coverage_out = coverage;
        return false;
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    coverage.signals_required = static_cast<uint32_t>(signal_count);
    for (size_t signal_index = 0; signal_index < signal_count; ++signal_index) {
        const ReportSignalDef &signal = signals[signal_index];
        uint32_t entries = 0;
        for (size_t i = 0; i < session_count; ++i) {
            PlanCountContext ctx;
            if (!edf_report_plan_signal(sessions[i],
                                        signal.id,
                                        range_start_ms,
                                        range_end_ms,
                                        count_plan_entry,
                                        &ctx)) {
                return false;
            }
            entries += ctx.entries;
        }
        if (entries == 0) {
            if (coverage_out) *coverage_out = coverage;
            return false;
        }
        coverage.signals_covered++;
    }

    if (coverage_out) *coverage_out = coverage;
    return true;
}

}  // namespace aircannect
