#include "edf_report_data_file.h"

#include <limits.h>

#include "memory_manager.h"
#include "report_legacy_storage.h"

namespace aircannect {

const EdfReportSessionFileDescriptor *edf_report_data_entry_file(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry) {
    if (entry.file_slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return nullptr;

    const EdfReportSessionFileDescriptor &file =
        session.files[entry.file_slot];
    if (file.kind != entry.file_kind || !file.path[0] ||
        file.header_size == 0 || file.record_size == 0 ||
        file.complete_records == 0) {
        return nullptr;
    }

    if (entry.first_record > file.complete_records ||
        entry.record_count > file.complete_records - entry.first_record) {
        return nullptr;
    }

    return &file;
}

EdfReportDataReadStatus edf_report_data_read_exact(ReportLegacyFile &file,
                                                   uint8_t *buffer,
                                                   size_t len) {
    if (!buffer && len > 0) return EdfReportDataReadStatus::InvalidArgument;

    size_t done = 0;
    while (done < len) {
        int read = 0;
        {
            ReportLegacyStorageGuard guard;
            read = file.read(buffer + done, len - done);
        }
        if (read <= 0) return EdfReportDataReadStatus::RecordReadFailed;
        done += static_cast<size_t>(read);
    }

    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus edf_report_data_open_file(
    const EdfReportSessionFileDescriptor &session_file,
    ReportLegacyFile &file) {
    {
        ReportLegacyStorageGuard guard;
        file = ReportLegacyStorage::open(session_file.path, "r");
    }
    if (!file || file.isDirectory()) {
        if (file) {
            ReportLegacyStorageGuard guard;
            file.close();
        }
        return EdfReportDataReadStatus::FileOpenFailed;
    }

    return EdfReportDataReadStatus::Ok;
}

void edf_report_data_close_file(ReportLegacyFile &file) {
    if (!file) return;

    ReportLegacyStorageGuard guard;
    file.close();
}

EdfReportDataReadStatus edf_report_data_read_header(
    const EdfReportSessionFileDescriptor &session_file,
    EdfReportFileDescriptor &file_desc,
    uint8_t *&header,
    size_t &header_size,
    ReportLegacyFile &file) {
    header = nullptr;
    header_size = session_file.header_size;
    if (header_size == 0) return EdfReportDataReadStatus::HeaderReadFailed;

    EdfReportDataReadStatus open_status =
        edf_report_data_open_file(session_file, file);
    if (open_status != EdfReportDataReadStatus::Ok) return open_status;

    header = static_cast<uint8_t *>(Memory::alloc_large(header_size, false));
    if (!header) {
        ReportLegacyStorageGuard guard;
        file.close();
        return EdfReportDataReadStatus::HeaderReadFailed;
    }

    {
        ReportLegacyStorageGuard guard;
        if (!file.seek(0)) {
            Memory::free(header);
            header = nullptr;
            file.close();
            return EdfReportDataReadStatus::HeaderReadFailed;
        }
    }

    size_t done = 0;
    while (done < header_size) {
        int read = 0;
        {
            ReportLegacyStorageGuard guard;
            read = file.read(header + done, header_size - done);
        }
        if (read <= 0) {
            Memory::free(header);
            header = nullptr;
            ReportLegacyStorageGuard guard;
            file.close();
            return EdfReportDataReadStatus::HeaderReadFailed;
        }
        done += static_cast<size_t>(read);
    }

    const EdfReportFileStatus desc_status = edf_report_describe_file(
        session_file.path,
        header,
        header_size,
        session_file.file_size,
        session_file.last_write,
        0,
        file_desc);
    if (desc_status != EdfReportFileStatus::Ok) {
        Memory::free(header);
        header = nullptr;
        ReportLegacyStorageGuard guard;
        file.close();
        return EdfReportDataReadStatus::HeaderParseFailed;
    }

    file_desc.header_start_ms = session_file.header_start_ms;
    file_desc.header_end_ms = session_file.header_end_ms;
    file_desc.inventory.complete_records_from_size =
        session_file.complete_records;
    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus edf_report_data_seek_record(
    ReportLegacyFile &file,
    const EdfReportSessionFileDescriptor &session_file,
    uint32_t record_index) {
    const uint64_t offset =
        static_cast<uint64_t>(session_file.header_size) +
        static_cast<uint64_t>(record_index) *
            static_cast<uint64_t>(session_file.record_size);
    if (offset > UINT32_MAX) {
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    ReportLegacyStorageGuard guard;
    return file.seek(static_cast<uint32_t>(offset))
               ? EdfReportDataReadStatus::Ok
               : EdfReportDataReadStatus::RecordReadFailed;
}

}  // namespace aircannect
