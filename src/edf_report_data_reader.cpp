#include "edf_report_data_reader.h"

#include <FS.h>
#include <stddef.h>
#include <string.h>

#include "edf_report_event_reader.h"
#include "edf_report_series_reader.h"
#include "memory_manager.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

struct AppendSeriesContext {
    ReportSpoolBuffer *payload = nullptr;
    bool full = false;
};

struct AppendEventContext {
    ReportSpoolBuffer *payload = nullptr;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;
    bool full = false;
};

bool append_series_sample(void *context, const ReportSeriesSample &sample) {
    AppendSeriesContext *ctx = static_cast<AppendSeriesContext *>(context);
    if (!ctx || !ctx->payload) return false;
    if (!report_append_series_sample(*ctx->payload, sample)) {
        ctx->full = true;
        return false;
    }
    return true;
}

bool append_event_record(void *context, const ReportEventRecord &event) {
    AppendEventContext *ctx = static_cast<AppendEventContext *>(context);
    if (!ctx || !ctx->payload) return false;
    const int64_t duration_ms = event.duration_ms > 0
                                    ? static_cast<int64_t>(event.duration_ms)
                                    : 0;
    const int64_t event_end_ms = event.start_ms + duration_ms;
    const bool in_range = duration_ms > 0
                              ? event.start_ms < ctx->range_end_ms &&
                                    event_end_ms > ctx->range_start_ms
                              : event.start_ms >= ctx->range_start_ms &&
                                    event.start_ms < ctx->range_end_ms;
    if (!in_range) {
        return true;
    }
    if (!report_append_event_record(*ctx->payload, event)) {
        ctx->full = true;
        return false;
    }
    return true;
}

const EdfReportSessionFileDescriptor *entry_file(
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

EdfReportDataReadStatus read_exact(File &file,
                                   uint8_t *buffer,
                                   size_t len) {
    if (!buffer && len > 0) return EdfReportDataReadStatus::InvalidArgument;
    size_t done = 0;
    while (done < len) {
        int read = 0;
        {
            Storage::Guard guard;
            read = file.read(buffer + done, len - done);
        }
        if (read <= 0) return EdfReportDataReadStatus::RecordReadFailed;
        done += static_cast<size_t>(read);
    }
    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus read_header(
    const EdfReportSessionFileDescriptor &session_file,
    EdfReportFileDescriptor &file_desc,
    uint8_t *&header,
    size_t &header_size,
    File &file) {
    header = nullptr;
    header_size = session_file.header_size;
    if (header_size == 0) return EdfReportDataReadStatus::HeaderReadFailed;

    {
        Storage::Guard guard;
        file = Storage::open(session_file.path, "r");
    }
    if (!file || file.isDirectory()) {
        if (file) {
            Storage::Guard guard;
            file.close();
        }
        return EdfReportDataReadStatus::FileOpenFailed;
    }

    header = static_cast<uint8_t *>(Memory::alloc_large(header_size, false));
    if (!header) {
        Storage::Guard guard;
        file.close();
        return EdfReportDataReadStatus::HeaderReadFailed;
    }

    {
        Storage::Guard guard;
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
            Storage::Guard guard;
            read = file.read(header + done, header_size - done);
        }
        if (read <= 0) {
            Memory::free(header);
            header = nullptr;
            Storage::Guard guard;
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
        file_desc);
    if (desc_status != EdfReportFileStatus::Ok) {
        Memory::free(header);
        header = nullptr;
        Storage::Guard guard;
        file.close();
        return EdfReportDataReadStatus::HeaderParseFailed;
    }
    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus seek_record(File &file,
                                    const EdfReportSessionFileDescriptor &sf,
                                    uint32_t record_index) {
    const uint64_t offset =
        static_cast<uint64_t>(sf.header_size) +
        static_cast<uint64_t>(record_index) *
            static_cast<uint64_t>(sf.record_size);
    if (offset > UINT32_MAX) return EdfReportDataReadStatus::RecordReadFailed;
    Storage::Guard guard;
    return file.seek(static_cast<uint32_t>(offset))
               ? EdfReportDataReadStatus::Ok
               : EdfReportDataReadStatus::RecordReadFailed;
}

EdfReportDataReadStatus read_series_entry(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats) {
    EdfReportSeriesDecoder decoder;
    const EdfReportSeriesStatus init_status =
        edf_report_series_decoder_init(file_desc,
                                       header,
                                       header_size,
                                       entry.signal,
                                       entry.primary,
                                       decoder);
    if (init_status != EdfReportSeriesStatus::Ok) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    uint8_t *record = static_cast<uint8_t *>(
        Memory::alloc_large(session_file.record_size, false));
    if (!record) return EdfReportDataReadStatus::RecordReadFailed;

    EdfReportDataReadStatus status =
        seek_record(file, session_file, entry.first_record);
    if (status != EdfReportDataReadStatus::Ok) {
        Memory::free(record);
        return status;
    }

    AppendSeriesContext ctx;
    ctx.payload = &payload;
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        status = read_exact(file, record, session_file.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        EdfReportSeriesDecodeStats record_stats;
        const EdfReportSeriesStatus decode_status =
            edf_report_decode_series_record(decoder,
                                            record,
                                            session_file.record_size,
                                            entry.first_record + i,
                                            entry.start_ms,
                                            entry.end_ms,
                                            append_series_sample,
                                            &ctx,
                                            record_stats);
        stats.samples_seen += record_stats.samples_seen;
        stats.samples_missing += record_stats.samples_missing;
        stats.samples_out_of_range += record_stats.samples_out_of_range;
        stats.samples_emitted += record_stats.samples_emitted;
        if (decode_status != EdfReportSeriesStatus::Ok) {
            status = ctx.full ? EdfReportDataReadStatus::PayloadFull
                              : EdfReportDataReadStatus::DecodeFailed;
            break;
        }
    }
    Memory::free(record);
    if (status != EdfReportDataReadStatus::Ok) return status;
    meta.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V1;
    meta.record_count = stats.samples_emitted;
    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus read_event_entry(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    File &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats) {
    uint8_t *record = static_cast<uint8_t *>(
        Memory::alloc_large(session_file.record_size, false));
    if (!record) return EdfReportDataReadStatus::RecordReadFailed;

    EdfReportDataReadStatus status =
        seek_record(file, session_file, entry.first_record);
    if (status != EdfReportDataReadStatus::Ok) {
        Memory::free(record);
        return status;
    }

    AppendEventContext ctx;
    ctx.payload = &payload;
    ctx.range_start_ms = entry.start_ms;
    ctx.range_end_ms = entry.end_ms;
    EdfReportEventDecodeContext decode_context;
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        status = read_exact(file, record, session_file.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        EdfReportEventDecodeStats record_stats;
        const EdfReportEventStatus decode_status =
            edf_report_decode_annotation_record(file_desc,
                                                record,
                                                session_file.record_size,
                                                true,
                                                append_event_record,
                                                &ctx,
                                                record_stats,
                                                &decode_context);
        stats.annotations_seen += record_stats.annotations_seen;
        stats.events_emitted += record_stats.events_emitted;
        stats.unsupported_event_labels += record_stats.unsupported_labels;
        if (decode_status != EdfReportEventStatus::Ok) {
            status = ctx.full ? EdfReportDataReadStatus::PayloadFull
                              : EdfReportDataReadStatus::DecodeFailed;
            break;
        }
    }
    Memory::free(record);
    if (status != EdfReportDataReadStatus::Ok) return status;
    meta.payload_schema = REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1;
    meta.record_count =
        static_cast<uint32_t>(payload.size() / report_event_record_wire_size());
    return EdfReportDataReadStatus::Ok;
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
        default:
            return "unknown";
    }
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
        entry_file(session, entry);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;

    payload.set_max_size(entry.payload_len_estimate + 4096u);
    if (entry.payload_len_estimate &&
        !payload.reserve_capacity(entry.payload_len_estimate)) {
        return EdfReportDataReadStatus::PayloadFull;
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        read_header(*session_file, file_desc, header, header_size, file);
    if (status == EdfReportDataReadStatus::Ok) {
        if (entry.kind == EdfReportDataKind::Series) {
            status = read_series_entry(*session_file,
                                       entry,
                                       file_desc,
                                       header,
                                       header_size,
                                       file,
                                       meta,
                                       payload,
                                       stats);
        } else if (entry.kind == EdfReportDataKind::Events) {
            status = read_event_entry(*session_file,
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
    if (file) {
        Storage::Guard guard;
        file.close();
    }
    if (status != EdfReportDataReadStatus::Ok) {
        payload.clear();
        meta = {};
    }
    return status;
}

}  // namespace aircannect
