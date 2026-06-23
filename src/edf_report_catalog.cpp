#include "edf_report_catalog.h"

#include <string.h>

#include "string_util.h"

namespace aircannect {
namespace {

bool report_kind(EdfInventoryFileKind kind) {
    return kind == EdfInventoryFileKind::Brp ||
           kind == EdfInventoryFileKind::Pld ||
           kind == EdfInventoryFileKind::Sa2 ||
           kind == EdfInventoryFileKind::Eve ||
           kind == EdfInventoryFileKind::Csl;
}

void copy_signal_descriptor(const EdfSignalHeader &src,
                            EdfReportSignalDescriptor &dst) {
    copy_cstr(dst.label, sizeof(dst.label), src.label);
    copy_cstr(dst.physical_dimension,
              sizeof(dst.physical_dimension),
              src.physical_dimension);
    dst.samples_per_record = src.samples_per_record;
    dst.sample_offset_in_record = src.sample_offset_in_record;
    dst.byte_offset_in_record = src.byte_offset_in_record;
}

}  // namespace

const char *edf_report_file_status_name(EdfReportFileStatus status) {
    switch (status) {
        case EdfReportFileStatus::Ok: return "ok";
        case EdfReportFileStatus::UnsupportedKind: return "unsupported_kind";
        case EdfReportFileStatus::InventoryError: return "inventory_error";
        case EdfReportFileStatus::TooManySignals: return "too_many_signals";
        case EdfReportFileStatus::SignalHeaderError:
            return "signal_header_error";
        default:
            return "unknown";
    }
}

bool edf_report_file_kind_supported(EdfInventoryFileKind kind) {
    return report_kind(kind);
}

EdfReportFileStatus edf_report_describe_file(
    const char *path,
    const uint8_t *header,
    size_t header_size,
    uint64_t file_size,
    time_t last_write,
    EdfReportFileDescriptor &out) {
    out = {};
    out.file_size = file_size;
    out.last_write = last_write;

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

    out.signal_count = inventory.header.signal_count;
    for (uint32_t i = 0; i < inventory.header.signal_count; ++i) {
        EdfSignalHeader signal;
        if (!edf_parse_signal_header(header, header_size, i, signal)) {
            out.status = EdfReportFileStatus::SignalHeaderError;
            return out.status;
        }
        copy_signal_descriptor(signal, out.signals[i]);
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

}  // namespace aircannect
