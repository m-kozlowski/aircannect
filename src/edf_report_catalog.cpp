#include "edf_report_catalog.h"

#include <limits.h>
#include <string.h>

#include "string_util.h"

namespace aircannect {
namespace {

const EdfReportSignalMappingDef REPORT_SIGNAL_MAP[] = {
    {EdfInventoryFileKind::Brp,
     "Flow.40ms",
     ReportSignalId::Flow,
     ReportSourceId::RespiratoryFlow6p25Hz,
     40,
     true},
    {EdfInventoryFileKind::Brp,
     "Press.40ms",
     ReportSignalId::MaskPressure,
     ReportSourceId::MaskPressure6p25Hz,
     40,
     true},
    {EdfInventoryFileKind::Pld,
     "MaskPress.2s",
     ReportSignalId::MaskPressure,
     ReportSourceId::TherapyOneMinute,
     2000,
     false},
    {EdfInventoryFileKind::Pld,
     "Press.2s",
     ReportSignalId::InspiratoryPressure,
     ReportSourceId::InspiratoryPressure0p5Hz,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "EprPress.2s",
     ReportSignalId::ExpiratoryPressure,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "Leak.2s",
     ReportSignalId::Leak,
     ReportSourceId::Leak0p5Hz,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "RespRate.2s",
     ReportSignalId::RespiratoryRate,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "MinVent.2s",
     ReportSignalId::MinuteVentilation,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "IERatio.2s",
     ReportSignalId::IeRatio,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "FlowLim.2s",
     ReportSignalId::FlowLimitation,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
    {EdfInventoryFileKind::Pld,
     "Ti.2s",
     ReportSignalId::InspiratoryDuration,
     ReportSourceId::TherapyOneMinute,
     2000,
     true},
};

bool report_kind(EdfInventoryFileKind kind) {
    return kind == EdfInventoryFileKind::Brp ||
           kind == EdfInventoryFileKind::Pld ||
           kind == EdfInventoryFileKind::Sa2 ||
           kind == EdfInventoryFileKind::Eve ||
           kind == EdfInventoryFileKind::Csl;
}

}  // namespace

size_t edf_report_session_file_slot(EdfInventoryFileKind kind) {
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

uint32_t edf_report_signal_bit(ReportSignalId signal) {
    const uint8_t index = static_cast<uint8_t>(signal);
    if (index >= 32) return 0;
    return 1u << index;
}

namespace {

bool copy_signal_descriptor(const EdfSignalHeader &src,
                            EdfReportSignalDescriptor &dst) {
    copy_cstr(dst.label, sizeof(dst.label), src.label);
    copy_cstr(dst.physical_dimension,
              sizeof(dst.physical_dimension),
              src.physical_dimension);
    dst.samples_per_record = src.samples_per_record;
    dst.sample_offset_in_record = src.sample_offset_in_record;
    dst.byte_offset_in_record = src.byte_offset_in_record;
    return edf_parse_signal_scale(src, dst.scale);
}

bool session_mapping_available(const EdfReportSessionDescriptor &session,
                               const EdfReportSignalMappingDef &mapping,
                               uint32_t bit) {
    if (bit == 0) return false;
    const size_t slot = edf_report_session_file_slot(mapping.kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return false;
    const uint32_t file_mask = edf_report_file_kind_mask(mapping.kind);
    if ((session.file_mask & file_mask) == 0) return false;
    const EdfReportSessionFileDescriptor &file = session.files[slot];
    if (file.kind != mapping.kind || !file.path[0]) return false;
    const uint32_t signal_mask = mapping.primary ? session.primary_signal_mask
                                                 : session.fallback_signal_mask;
    return (signal_mask & bit) != 0;
}

}  // namespace

const char *edf_report_file_status_name(EdfReportFileStatus status) {
    switch (status) {
        case EdfReportFileStatus::Ok: return "ok";
        case EdfReportFileStatus::UnsupportedKind: return "unsupported_kind";
        case EdfReportFileStatus::InventoryError: return "inventory_error";
        case EdfReportFileStatus::FileTooLarge: return "file_too_large";
        case EdfReportFileStatus::TooManySignals: return "too_many_signals";
        case EdfReportFileStatus::SignalHeaderError:
            return "signal_header_error";
        case EdfReportFileStatus::TimingError: return "timing_error";
        default:
            return "unknown";
    }
}

bool edf_report_file_kind_supported(EdfInventoryFileKind kind) {
    return report_kind(kind);
}

uint32_t edf_report_file_kind_mask(EdfInventoryFileKind kind) {
    const size_t slot = edf_report_session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return 0;
    return 1u << slot;
}

EdfReportFileStatus edf_report_describe_file(
    const char *path,
    const uint8_t *header,
    size_t header_size,
    uint64_t file_size,
    time_t last_write,
    int32_t timezone_offset_minutes,
    EdfReportFileDescriptor &out) {
    out = EdfReportFileDescriptor();
    out.file_size = file_size;
    out.last_write = last_write;
    copy_cstr(out.path, sizeof(out.path), path);
    if (file_size > static_cast<uint64_t>(SIZE_MAX)) {
        out.status = EdfReportFileStatus::FileTooLarge;
        return out.status;
    }

    EdfInventoryEntry inventory;
    const EdfInventoryStatus inventory_status =
        edf_inventory_describe_file(path,
                                    header,
                                    header_size,
                                    static_cast<size_t>(file_size),
                                    inventory);
    out.inventory = inventory;
    if (inventory_status != EdfInventoryStatus::Ok) {
        out.status = EdfReportFileStatus::InventoryError;
        return out.status;
    }
    if (!report_kind(inventory.kind)) {
        out.status = EdfReportFileStatus::UnsupportedKind;
        return out.status;
    }
    if (inventory.header.signal_count > AC_EDF_REPORT_FILE_SIGNAL_MAX) {
        out.status = EdfReportFileStatus::TooManySignals;
        return out.status;
    }
    if (!edf_parse_header_start_ms(inventory.header, out.header_start_ms) ||
        !edf_parse_header_record_duration_ms(inventory.header,
                                             out.record_duration_ms)) {
        out.status = EdfReportFileStatus::TimingError;
        return out.status;
    }
    out.header_start_ms -=
        static_cast<int64_t>(timezone_offset_minutes) * 60LL * 1000LL;
    out.header_size = inventory.header.header_size;
    out.record_size = inventory.header.record_size;
    const uint64_t duration_ms =
        static_cast<uint64_t>(inventory.complete_records_from_size) *
        static_cast<uint64_t>(out.record_duration_ms);
    if (out.header_start_ms < 0 ||
        duration_ms > static_cast<uint64_t>(INT64_MAX - out.header_start_ms)) {
        out.status = EdfReportFileStatus::TimingError;
        return out.status;
    }
    out.header_end_ms = out.header_start_ms + static_cast<int64_t>(duration_ms);

    out.signal_count = inventory.header.signal_count;
    for (uint32_t i = 0; i < inventory.header.signal_count; ++i) {
        EdfSignalHeader signal;
        if (!edf_parse_signal_header(header, header_size, i, signal)) {
            out.status = EdfReportFileStatus::SignalHeaderError;
            return out.status;
        }
        if (!copy_signal_descriptor(signal, out.signals[i])) {
            out.status = EdfReportFileStatus::SignalHeaderError;
            return out.status;
        }
    }

    out.status = EdfReportFileStatus::Ok;
    return out.status;
}

const EdfReportSignalDescriptor *edf_report_find_signal(
    const EdfReportFileDescriptor &file,
    const char *label) {
    if (!label || !label[0]) return nullptr;
    for (uint32_t i = 0; i < file.signal_count; ++i) {
        if (strcmp(file.signals[i].label, label) == 0) {
            return &file.signals[i];
        }
    }
    return nullptr;
}

const EdfReportSignalMappingDef *edf_report_signal_mapping_defs(
    size_t &count) {
    count = sizeof(REPORT_SIGNAL_MAP) / sizeof(REPORT_SIGNAL_MAP[0]);
    return REPORT_SIGNAL_MAP;
}

const char *edf_report_signal_mapping_label(
    const EdfReportSignalMappingDef &mapping) {
    return mapping.label ? mapping.label : "";
}

bool edf_report_signal_mapping(EdfInventoryFileKind kind,
                               const char *label,
                               EdfReportSignalMapping &out) {
    out = EdfReportSignalMapping();
    if (!label || !label[0]) return false;
    for (const EdfReportSignalMappingDef &row : REPORT_SIGNAL_MAP) {
        if (row.kind != kind) continue;
        const char *row_label = edf_report_signal_mapping_label(row);
        if (!row_label[0] || strcmp(row_label, label) != 0) continue;
        out.signal = row.signal;
        out.source = row.source;
        out.sample_interval_ms = row.sample_interval_ms;
        out.primary = row.primary;
        return true;
    }
    return false;
}

const EdfReportSignalMappingDef *edf_report_session_select_signal_mapping(
    const EdfReportSessionDescriptor &session,
    ReportSignalId signal) {
    size_t count = 0;
    const EdfReportSignalMappingDef *mappings =
        edf_report_signal_mapping_defs(count);
    const uint32_t bit = edf_report_signal_bit(signal);
    for (size_t pass = 0; pass < 2; ++pass) {
        const bool want_primary = pass == 0;
        for (size_t i = 0; i < count; ++i) {
            const EdfReportSignalMappingDef &mapping = mappings[i];
            if (mapping.signal != signal || mapping.primary != want_primary) {
                continue;
            }
            if (session_mapping_available(session, mapping, bit)) {
                return &mapping;
            }
        }
    }
    return nullptr;
}

bool edf_report_file_find_signal_mapping(const EdfReportFileDescriptor &file,
                                         ReportSignalId signal,
                                         bool require_primary,
                                         uint32_t &signal_index,
                                         EdfReportSignalMapping &mapping) {
    signal_index = 0;
    mapping = EdfReportSignalMapping();
    if (file.status != EdfReportFileStatus::Ok) return false;
    const size_t pass_count = require_primary ? 1 : 2;
    for (size_t pass = 0; pass < pass_count; ++pass) {
        const bool want_primary = pass == 0;
        for (uint32_t i = 0; i < file.signal_count; ++i) {
            EdfReportSignalMapping candidate;
            if (!edf_report_signal_mapping(file.inventory.kind,
                                           file.signals[i].label,
                                           candidate)) {
                continue;
            }
            if (candidate.signal != signal ||
                candidate.primary != want_primary) {
                continue;
            }
            signal_index = i;
            mapping = candidate;
            return true;
        }
    }
    return false;
}

void edf_report_session_init(EdfReportSessionDescriptor &session) {
    session = EdfReportSessionDescriptor();
}

bool edf_report_session_add_file(EdfReportSessionDescriptor &session,
                                 const EdfReportFileDescriptor &file) {
    if (file.status != EdfReportFileStatus::Ok ||
        !report_kind(file.inventory.kind)) {
        return false;
    }
    const uint32_t file_mask = edf_report_file_kind_mask(file.inventory.kind);
    if (file_mask == 0 || (session.file_mask & file_mask) != 0) {
        return false;
    }
    if (file.file_size > UINT64_MAX - session.total_size) {
        return false;
    }
    if (session.file_count == 0) {
        copy_cstr(session.sleep_day,
                  sizeof(session.sleep_day),
                  file.inventory.sleep_day);
        copy_cstr(session.session_stamp,
                  sizeof(session.session_stamp),
                  file.inventory.session_stamp);
    } else if (strcmp(session.sleep_day, file.inventory.sleep_day) != 0 ||
               strcmp(session.session_stamp,
                      file.inventory.session_stamp) != 0) {
        return false;
    }

    const size_t slot = edf_report_session_file_slot(file.inventory.kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return false;
    EdfReportSessionFileDescriptor &dst = session.files[slot];
    dst = EdfReportSessionFileDescriptor();
    dst.kind = file.inventory.kind;
    copy_cstr(dst.path, sizeof(dst.path), file.path);
    dst.file_size = file.file_size;
    dst.last_write = file.last_write;
    dst.header_start_ms = file.header_start_ms;
    dst.header_end_ms = file.header_end_ms;
    dst.header_size = file.header_size;
    dst.record_size = file.record_size;
    dst.record_duration_ms = file.record_duration_ms;
    dst.complete_records = file.inventory.complete_records_from_size;

    session.file_mask |= file_mask;
    session.file_count++;
    session.total_size += file.file_size;
    if (file.last_write > session.latest_write) {
        session.latest_write = file.last_write;
    }
    if (session.earliest_header_start_ms == 0 ||
        file.header_start_ms < session.earliest_header_start_ms) {
        session.earliest_header_start_ms = file.header_start_ms;
    }
    if (file.header_end_ms > session.latest_header_end_ms) {
        session.latest_header_end_ms = file.header_end_ms;
    }
    session.warnings |= file.inventory.warnings;

    for (uint32_t i = 0; i < file.signal_count; ++i) {
        EdfReportSignalMapping mapping;
        if (!edf_report_signal_mapping(file.inventory.kind,
                                       file.signals[i].label,
                                       mapping)) {
            continue;
        }
        const uint32_t bit = edf_report_signal_bit(mapping.signal);
        if (bit == 0) continue;
        if (mapping.primary) {
            session.primary_signal_mask |= bit;
        } else {
            session.fallback_signal_mask |= bit;
        }
    }
    return true;
}

bool edf_report_session_has_primary_signal(
    const EdfReportSessionDescriptor &session,
    ReportSignalId signal) {
    const uint32_t bit = edf_report_signal_bit(signal);
    return bit != 0 && (session.primary_signal_mask & bit) != 0;
}

bool edf_report_session_has_signal(const EdfReportSessionDescriptor &session,
                                   ReportSignalId signal) {
    const uint32_t bit = edf_report_signal_bit(signal);
    if (bit == 0) return false;
    return ((session.primary_signal_mask | session.fallback_signal_mask) &
            bit) != 0;
}

}  // namespace aircannect
