#include "edf_report_data_reader.h"

#include <FS.h>
#include <stddef.h>

#include "board_report.h"
#include "edf_report_batch_reader.h"
#include "edf_report_data_file.h"
#include "edf_report_event_entry_reader.h"
#include "edf_report_series_entry_reader.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

bool derived_metric_edge_zero_padding(ReportSignalId signal) {
    switch (signal) {
        case ReportSignalId::MinuteVentilation:
        case ReportSignalId::RespiratoryRate:
        case ReportSignalId::IeRatio:
        case ReportSignalId::InspiratoryDuration:
            return true;
        default:
            return false;
    }
}

}  // namespace

const char *edf_report_data_read_status_name(
    EdfReportDataReadStatus status) {
    switch (status) {
        case EdfReportDataReadStatus::Ok: return "ok";
        case EdfReportDataReadStatus::InvalidArgument:
            return "invalid_argument";
        case EdfReportDataReadStatus::FileOpenFailed:
            return "file_open_failed";
        case EdfReportDataReadStatus::HeaderReadFailed:
            return "header_read_failed";
        case EdfReportDataReadStatus::HeaderParseFailed:
            return "header_parse_failed";
        case EdfReportDataReadStatus::RecordReadFailed:
            return "record_read_failed";
        case EdfReportDataReadStatus::DecodeFailed:
            return "decode_failed";
        case EdfReportDataReadStatus::PayloadFull:
            return "payload_full";
        case EdfReportDataReadStatus::CallbackRejected:
            return "callback_rejected";
        default:
            return "unknown";
    }
}

bool edf_report_signal_uses_edge_zero_padding(ReportSignalId signal) {
    return derived_metric_edge_zero_padding(signal);
}

EdfReportDataReadStatus edf_report_read_entry_payload(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats) {
    meta = {};
    payload.clear();
    stats = {};
    if (entry.record_count == 0 || entry.end_ms <= entry.start_ms) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    const EdfReportSessionFileDescriptor *session_file =
        edf_report_data_entry_file(session, entry);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;

    payload.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);
    if (entry.payload_len_estimate &&
        !payload.reserve_capacity(entry.payload_len_estimate)) {
        return EdfReportDataReadStatus::PayloadFull;
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        edf_report_data_read_header(*session_file,
                                    file_desc,
                                    header,
                                    header_size,
                                    file);
    if (status == EdfReportDataReadStatus::Ok) {
        if (entry.kind == EdfReportDataKind::Series) {
            status = edf_report_read_series_entry_payload(*session_file,
                                                          entry,
                                                          file_desc,
                                                          header,
                                                          header_size,
                                                          file,
                                                          meta,
                                                          payload,
                                                          stats);
        } else if (entry.kind == EdfReportDataKind::Events) {
            status = edf_report_read_event_entry_payload(*session_file,
                                                         entry,
                                                         file_desc,
                                                         file,
                                                         meta,
                                                         payload,
                                                         stats);
        } else {
            status = EdfReportDataReadStatus::InvalidArgument;
        }
    }

    if (header) Memory::free(header);
    edf_report_data_close_file(file);
    if (status != EdfReportDataReadStatus::Ok) {
        payload.clear();
        meta = {};
    }
    return status;
}

EdfReportDataReadStatus edf_report_for_each_entry_series_sample(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry &entry,
    ReportStoreChunkMeta &meta,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    ReportSeriesSampleCallback callback,
    void *context) {
    meta = {};
    stats = {};
    if (interval_ms_out) *interval_ms_out = 0;
    if (entry.kind != EdfReportDataKind::Series ||
        entry.record_count == 0 || entry.end_ms <= entry.start_ms ||
        !callback) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    const EdfReportSessionFileDescriptor *session_file =
        edf_report_data_entry_file(session, entry);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;

    File file;
    EdfReportDataReadStatus status =
        edf_report_emit_series_entry_samples(*session_file,
                                             entry,
                                             file,
                                             meta,
                                             stats,
                                             interval_ms_out,
                                             callback,
                                             context);

    edf_report_data_close_file(file);
    if (status != EdfReportDataReadStatus::Ok) {
        meta = {};
    }
    return status;
}

EdfReportDataReadStatus edf_report_for_each_series_batch_sample(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) {
    stats = {};
    if (metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    if (interval_ms_out) {
        for (size_t i = 0; i < entry_count; ++i) interval_ms_out[i] = 0;
    }
    if (!entries || entry_count == 0 || !callback) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    const EdfReportSessionFileDescriptor *session_file =
        edf_report_data_entry_file(session, entries[0]);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;
    for (size_t i = 1; i < entry_count; ++i) {
        const EdfReportSessionFileDescriptor *other =
            edf_report_data_entry_file(session, entries[i]);
        if (other != session_file) {
            return EdfReportDataReadStatus::InvalidArgument;
        }
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        edf_report_data_read_header(*session_file,
                                    file_desc,
                                    header,
                                    header_size,
                                    file);
    if (status == EdfReportDataReadStatus::Ok) {
        status = edf_report_stream_series_batch_from_header(*session_file,
                                                            entries,
                                                            entry_count,
                                                            file_desc,
                                                            header,
                                                            header_size,
                                                            file,
                                                            metas,
                                                            stats,
                                                            interval_ms_out,
                                                            callback,
                                                            context);
    }
    if (header) Memory::free(header);
    edf_report_data_close_file(file);
    if (status != EdfReportDataReadStatus::Ok && metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    return status;
}

EdfReportDataReadStatus edf_report_for_each_series_batch_plot(
    const EdfReportSessionDescriptor &session,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    const EdfReportSeriesPlotConfig *configs,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) {
    stats = {};
    if (metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    if (interval_ms_out) {
        for (size_t i = 0; i < entry_count; ++i) interval_ms_out[i] = 0;
    }
    if (!entries || entry_count == 0 || !configs || !callback) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    const EdfReportSessionFileDescriptor *session_file =
        edf_report_data_entry_file(session, entries[0]);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;
    for (size_t i = 1; i < entry_count; ++i) {
        const EdfReportSessionFileDescriptor *other =
            edf_report_data_entry_file(session, entries[i]);
        if (other != session_file) {
            return EdfReportDataReadStatus::InvalidArgument;
        }
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        edf_report_data_read_header(*session_file,
                                    file_desc,
                                    header,
                                    header_size,
                                    file);
    if (status == EdfReportDataReadStatus::Ok) {
        status = edf_report_stream_series_batch_plot_from_header(
            *session_file,
            entries,
            entry_count,
            configs,
            file_desc,
            header,
            header_size,
            file,
            metas,
            stats,
            interval_ms_out,
            callback,
            context);
    }
    if (header) Memory::free(header);
    edf_report_data_close_file(file);
    if (status != EdfReportDataReadStatus::Ok && metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    return status;
}

}  // namespace aircannect
