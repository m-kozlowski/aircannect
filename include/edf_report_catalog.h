#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "edf_file_inventory.h"
#include "edf_file_reader.h"
#include "report_sources.h"

namespace aircannect {

static constexpr size_t AC_EDF_REPORT_FILE_SIGNAL_MAX = 16;
static constexpr size_t AC_EDF_REPORT_PATH_MAX = 96;
static constexpr size_t AC_EDF_REPORT_SESSION_FILE_MAX = 5;

enum class EdfReportFileStatus : uint8_t {
    Ok,
    UnsupportedKind,
    InventoryError,
    FileTooLarge,
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
    char path[AC_EDF_REPORT_PATH_MAX] = {};
    uint64_t file_size = 0;
    time_t last_write = 0;
    uint32_t signal_count = 0;
    EdfReportSignalDescriptor signals[AC_EDF_REPORT_FILE_SIGNAL_MAX] = {};
};

struct EdfReportSignalMapping {
    ReportSignalId signal = ReportSignalId::Flow;
    uint32_t sample_interval_ms = 0;
    bool primary = false;
};

struct EdfReportSessionFileDescriptor {
    EdfInventoryFileKind kind = EdfInventoryFileKind::Unknown;
    char path[AC_EDF_REPORT_PATH_MAX] = {};
    uint64_t file_size = 0;
    time_t last_write = 0;
    uint32_t complete_records = 0;
    uint32_t signal_count = 0;
};

struct EdfReportSessionDescriptor {
    char sleep_day[9] = {};
    char session_stamp[16] = {};
    uint32_t file_mask = 0;
    uint32_t primary_signal_mask = 0;
    uint32_t fallback_signal_mask = 0;
    uint32_t warnings = 0;
    size_t file_count = 0;
    uint64_t total_size = 0;
    time_t latest_write = 0;
    EdfReportSessionFileDescriptor files[AC_EDF_REPORT_SESSION_FILE_MAX] = {};
};

const char *edf_report_file_status_name(EdfReportFileStatus status);
bool edf_report_file_kind_supported(EdfInventoryFileKind kind);
uint32_t edf_report_file_kind_mask(EdfInventoryFileKind kind);

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

bool edf_report_signal_mapping(EdfInventoryFileKind kind,
                               const char *label,
                               EdfReportSignalMapping &out);

void edf_report_session_init(EdfReportSessionDescriptor &session);
bool edf_report_session_add_file(EdfReportSessionDescriptor &session,
                                 const EdfReportFileDescriptor &file);
bool edf_report_session_has_primary_signal(
    const EdfReportSessionDescriptor &session,
    ReportSignalId signal);
bool edf_report_session_has_signal(const EdfReportSessionDescriptor &session,
                                   ReportSignalId signal);

}  // namespace aircannect
