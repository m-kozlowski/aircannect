#include "edf_report_series_entry_reader.h"

#include <limits.h>
#include <string.h>

#include "board_report.h"
#include "edf_report_data_file.h"
#include "edf_report_series_reader.h"
#include "edf_report_stream_series.h"
#include "edf_report_uniform_series.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

EdfReportDataReadStatus collect_series_entry_decoded(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    const EdfReportSeriesDecoder &decoder,
    File &file,
    EdfReportUniformSeriesData &series,
    EdfReportDataReadStats &stats) {
    series.clear();

    if (decoder.signal_header.samples_per_record == 0) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    if (decoder.record_duration_ms %
            decoder.signal_header.samples_per_record !=
        0) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    const uint32_t interval_ms =
        decoder.record_duration_ms /
        decoder.signal_header.samples_per_record;
    if (interval_ms == 0) return EdfReportDataReadStatus::DecodeFailed;

    const int64_t duration_ms = entry.end_ms - entry.start_ms;
    if (duration_ms <= 0 ||
        duration_ms % static_cast<int64_t>(interval_ms) != 0) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    const int64_t sample_count64 =
        duration_ms / static_cast<int64_t>(interval_ms);
    if (sample_count64 <= 0 || sample_count64 > UINT32_MAX) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    const uint32_t sample_count = static_cast<uint32_t>(sample_count64);
    if (static_cast<size_t>(sample_count) >
        SIZE_MAX / sizeof(int32_t)) {
        return EdfReportDataReadStatus::PayloadFull;
    }

    int32_t *values = static_cast<int32_t *>(
        Memory::calloc_large(sample_count, sizeof(int32_t), false));
    if (!values) return EdfReportDataReadStatus::PayloadFull;

    const size_t missing_bitmap_bytes =
        (static_cast<size_t>(sample_count) + 7u) / 8u;
    uint8_t *missing_bitmap = static_cast<uint8_t *>(
        Memory::alloc_large(missing_bitmap_bytes, false));
    if (!missing_bitmap) {
        Memory::free(values);
        return EdfReportDataReadStatus::PayloadFull;
    }
    memset(missing_bitmap, 0xFF, missing_bitmap_bytes);

    if (decoder.signal_header.samples_per_record > UINT32_MAX / 2u) {
        Memory::free(missing_bitmap);
        Memory::free(values);
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    const uint32_t signal_bytes =
        decoder.signal_header.samples_per_record * 2u;
    const uint32_t signal_end =
        decoder.signal_header.byte_offset_in_record + signal_bytes;
    if (signal_end < decoder.signal_header.byte_offset_in_record ||
        signal_end > decoder.record_size ||
        decoder.record_size != session_file.record_size) {
        Memory::free(missing_bitmap);
        Memory::free(values);
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    uint8_t *record = static_cast<uint8_t *>(
        Memory::alloc_large(decoder.record_size, false));
    if (!record) {
        Memory::free(missing_bitmap);
        Memory::free(values);
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    EdfReportUniformSeriesBuildContext ctx;
    ctx.start_ms = entry.start_ms;
    ctx.interval_ms = interval_ms;
    ctx.sample_count = sample_count;
    ctx.values = values;
    ctx.missing_bitmap = missing_bitmap;
    ctx.missing_bitmap_bytes = missing_bitmap_bytes;

    EdfReportDataReadStatus status =
        edf_report_data_seek_record(file, session_file, entry.first_record);
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        if (status != EdfReportDataReadStatus::Ok) break;

        status = edf_report_data_read_exact(file, record, decoder.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;

        stats.records_read++;

        EdfReportSeriesDecodeStats record_stats;
        const EdfReportSeriesStatus decode_status =
            edf_report_decode_series_record(
                decoder,
                record,
                decoder.record_size,
                entry.first_record + i,
                entry.start_ms,
                entry.end_ms,
                edf_report_uniform_series_record_sample,
                &ctx,
                record_stats);
        stats.samples_seen += record_stats.samples_seen;
        stats.samples_missing += record_stats.samples_missing;
        stats.samples_out_of_range += record_stats.samples_out_of_range;
        stats.samples_emitted += record_stats.samples_emitted;

        if (decode_status != EdfReportSeriesStatus::Ok) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
    }
    Memory::free(record);

    if (status == EdfReportDataReadStatus::Ok &&
        edf_report_signal_uses_edge_zero_padding(entry.signal)) {
        const uint32_t trimmed =
            edf_report_uniform_series_trim_edge_zero_padding(
                ctx,
                entry.trim_leading_padding,
                entry.trim_trailing_padding);
        if (trimmed > 0) {
            stats.samples_missing += trimmed;
            stats.samples_emitted =
                trimmed > stats.samples_emitted
                    ? 0
                    : stats.samples_emitted - trimmed;
        }
    }

    if (status != EdfReportDataReadStatus::Ok) {
        Memory::free(missing_bitmap);
        Memory::free(values);
        return status;
    }

    series.interval_ms = interval_ms;
    series.sample_count = sample_count;
    series.values = values;
    series.missing_bitmap = missing_bitmap;
    series.missing_bitmap_bytes = missing_bitmap_bytes;
    return EdfReportDataReadStatus::Ok;
}

EdfReportDataReadStatus collect_series_entry_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    EdfReportUniformSeriesData &series,
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

    return collect_series_entry_decoded(session_file,
                                        entry,
                                        decoder,
                                        file,
                                        series,
                                        stats);
}

EdfReportDataReadStatus stream_series_entry_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta &meta,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    ReportSeriesSampleCallback callback,
    void *context) {
    if (!callback) return EdfReportDataReadStatus::InvalidArgument;

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

    if (decoder.signal_header.samples_per_record == 0 ||
        decoder.record_duration_ms %
                decoder.signal_header.samples_per_record !=
            0) {
        return EdfReportDataReadStatus::DecodeFailed;
    }

    const uint32_t interval_ms =
        decoder.record_duration_ms /
        decoder.signal_header.samples_per_record;
    if (interval_ms == 0) return EdfReportDataReadStatus::DecodeFailed;

    if (decoder.signal_header.samples_per_record > UINT32_MAX / 2u) {
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    const uint32_t signal_bytes =
        decoder.signal_header.samples_per_record * 2u;
    const uint32_t signal_end =
        decoder.signal_header.byte_offset_in_record + signal_bytes;
    if (signal_end < decoder.signal_header.byte_offset_in_record ||
        signal_end > decoder.record_size ||
        decoder.record_size != session_file.record_size) {
        return EdfReportDataReadStatus::RecordReadFailed;
    }

    uint8_t *record = static_cast<uint8_t *>(
        Memory::alloc_large(decoder.record_size, false));
    if (!record) return EdfReportDataReadStatus::RecordReadFailed;

    EdfReportStreamSeriesContext ctx;
    ctx.callback = callback;
    ctx.context = context;
    ctx.interval_ms = interval_ms;

    if (edf_report_signal_uses_edge_zero_padding(entry.signal)) {
        ctx.trim_leading = entry.trim_leading_padding;
        ctx.trim_trailing = entry.trim_trailing_padding;
        ctx.leading_open = ctx.trim_leading;
    }

    EdfReportDataReadStatus status =
        edf_report_data_seek_record(file, session_file, entry.first_record);
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        if (status != EdfReportDataReadStatus::Ok) break;

        status = edf_report_data_read_exact(file, record, decoder.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;

        stats.records_read++;

        EdfReportSeriesDecodeStats record_stats;
        const EdfReportSeriesStatus decode_status =
            edf_report_decode_series_record(
                decoder,
                record,
                decoder.record_size,
                entry.first_record + i,
                entry.start_ms,
                entry.end_ms,
                edf_report_stream_series_record_sample,
                &ctx,
                record_stats);
        stats.samples_seen += record_stats.samples_seen;
        stats.samples_missing += record_stats.samples_missing;
        stats.samples_out_of_range += record_stats.samples_out_of_range;

        if (decode_status == EdfReportSeriesStatus::CallbackRejected) {
            status = EdfReportDataReadStatus::CallbackRejected;
            break;
        }

        if (decode_status != EdfReportSeriesStatus::Ok) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
    }
    Memory::free(record);

    if (status == EdfReportDataReadStatus::Ok) {
        edf_report_stream_series_clear_zero_run(ctx);
        stats.samples_emitted = ctx.samples_emitted;
        stats.samples_missing += ctx.samples_trimmed;
    }

    if (status != EdfReportDataReadStatus::Ok) return status;

    if (interval_ms_out) *interval_ms_out = interval_ms;

    meta.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    meta.record_count =
        entry.record_count_estimate ? entry.record_count_estimate
                                    : entry.record_count;
    return EdfReportDataReadStatus::Ok;
}

}  // namespace

EdfReportDataReadStatus edf_report_read_series_entry_payload(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload,
    EdfReportDataReadStats &stats) {
    EdfReportUniformSeriesData series;
    EdfReportDataReadStatus status =
        collect_series_entry_from_header(session_file,
                                         entry,
                                         file_desc,
                                         header,
                                         header_size,
                                         file,
                                         series,
                                         stats);
    if (status != EdfReportDataReadStatus::Ok) return status;

    const bool include_missing_bitmap =
        stats.samples_emitted < series.sample_count;
    if (!report_build_series_payload_v2_uniform(
            payload,
            series.interval_ms,
            series.values,
            series.sample_count,
            include_missing_bitmap ? series.missing_bitmap : nullptr,
            include_missing_bitmap ? series.missing_bitmap_bytes : 0)) {
        series.clear();
        return EdfReportDataReadStatus::PayloadFull;
    }

    meta.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    meta.record_count = series.sample_count;
    series.clear();
    return status;
}

EdfReportDataReadStatus edf_report_emit_series_entry_samples(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    File &file,
    ReportStoreChunkMeta &meta,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    ReportSeriesSampleCallback callback,
    void *context) {
    if (!callback) return EdfReportDataReadStatus::InvalidArgument;

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    EdfReportDataReadStatus status =
        edf_report_data_read_header(session_file,
                                    file_desc,
                                    header,
                                    header_size,
                                    file);
    if (status != EdfReportDataReadStatus::Ok) {
        if (header) Memory::free(header);
        return status;
    }

    status = stream_series_entry_from_header(session_file,
                                             entry,
                                             file_desc,
                                             header,
                                             header_size,
                                             file,
                                             meta,
                                             stats,
                                             interval_ms_out,
                                             callback,
                                             context);

    if (header) Memory::free(header);
    return status;
}

}  // namespace aircannect
