#include "edf_report_event_entry_reader.h"

#include "board_report.h"
#include "edf_report_data_file.h"
#include "edf_report_event_reader.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

struct AppendEventContext {
    ReportSpoolBuffer *payload = nullptr;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;
    bool full = false;
};

bool append_event_record(void *context, const ReportEventRecord &event) {
    AppendEventContext *ctx = static_cast<AppendEventContext *>(context);
    if (!ctx || !ctx->payload) return false;

    if (!report_event_overlaps_window(event,
                                      ctx->range_start_ms,
                                      ctx->range_end_ms,
                                      AC_REPORT_EVENT_EDGE_TOLERANCE_MS)) {
        return true;
    }

    if (!report_append_event_record(*ctx->payload, event)) {
        ctx->full = true;
        return false;
    }

    return true;
}

}  // namespace

EdfReportDataReadStatus edf_report_read_event_entry_payload(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    ReportLegacyFile &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats) {
    uint8_t *record = static_cast<uint8_t *>(
        Memory::alloc_large(session_file.record_size, false));
    if (!record) return EdfReportDataReadStatus::RecordReadFailed;

    EdfReportDataReadStatus status =
        edf_report_data_seek_record(file, session_file, entry.first_record);
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
        status = edf_report_data_read_exact(file,
                                            record,
                                            session_file.record_size);
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

}  // namespace aircannect
