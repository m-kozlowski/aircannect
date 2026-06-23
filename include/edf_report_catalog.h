#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "edf_file_inventory.h"
#include "edf_file_reader.h"

namespace aircannect {

static constexpr size_t AC_EDF_REPORT_FILE_SIGNAL_MAX = 16;

enum class EdfReportFileStatus : uint8_t {
    Ok,
    UnsupportedKind,
    InventoryError,
    TooManySignals,
    SignalHeaderError,
};

struct EdfReportSignalDescriptor {
    char label[AC_EDF_SIGNAL_LABEL_TEXT_SIZE] = {};
    char physical_dimension[AC_EDF_SIGNAL_PHYSICAL_DIMENSION_TEXT_SIZE] = {};
    uint32_t samples_per_record = 0;
    uint32_t sample_offset_in_record = 0;
    uint32_t byte_offset_in_record = 0;
};

struct EdfReportFileDescriptor {
    EdfReportFileStatus status = EdfReportFileStatus::InventoryError;
    EdfInventoryEntry inventory;
    uint64_t file_size = 0;
    time_t last_write = 0;
    uint32_t signal_count = 0;
    EdfReportSignalDescriptor signals[AC_EDF_REPORT_FILE_SIGNAL_MAX] = {};
};

const char *edf_report_file_status_name(EdfReportFileStatus status);
bool edf_report_file_kind_supported(EdfInventoryFileKind kind);

EdfReportFileStatus edf_report_describe_file(
    const char *path,
    const uint8_t *header,
    size_t header_size,
    uint64_t file_size,
    time_t last_write,
    EdfReportFileDescriptor &out);

const EdfReportSignalDescriptor *edf_report_find_signal(
    const EdfReportFileDescriptor &file,
    const char *label);

}  // namespace aircannect
