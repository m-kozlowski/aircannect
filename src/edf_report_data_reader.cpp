#include "edf_report_data_reader.h"

#include <FS.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

#include "board_report.h"
#include "edf_report_batch_plot.h"
#include "edf_report_data_file.h"
#include "edf_report_event_reader.h"
#include "edf_report_series_reader.h"
#include "edf_report_stream_series.h"
#include "edf_report_uniform_series.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

struct AppendEventContext {
    ReportSpoolBuffer *payload = nullptr;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;
    bool full = false;
};

struct BatchSeriesEmitter {
    EdfReportSeriesBatchSampleCallback callback = nullptr;
    void *context = nullptr;
    size_t item_index = 0;
};

struct BatchSeriesDecoder {
    EdfReportSeriesDecoder decoder;
    EdfReportStreamSeriesContext stream;
    BatchSeriesEmitter emitter;
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    uint32_t interval_ms = 0;
    bool active = false;
};

struct BatchPlotDecoder {
    EdfReportSeriesDecoder decoder;
    EdfReportBatchPlotState plot;
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    uint32_t interval_ms = 0;
    bool active = false;
};

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

bool emit_batch_series_sample(void *context,
                              const ReportSeriesSample &sample) {
    BatchSeriesEmitter *emitter =
        static_cast<BatchSeriesEmitter *>(context);
    if (!emitter || !emitter->callback) return false;
    return emitter->callback(emitter->context, emitter->item_index, sample);
}

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

    if (decoder.signal_header.samples_per_record >
        UINT32_MAX / 2u) {
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
    if (status == EdfReportDataReadStatus::Ok) {
        if (derived_metric_edge_zero_padding(entry.signal)) {
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
    if (derived_metric_edge_zero_padding(entry.signal)) {
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

EdfReportDataReadStatus stream_series_batch_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) {
    if (!entries || entry_count == 0 || !callback) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    if (entry_count > SIZE_MAX / sizeof(BatchSeriesDecoder)) {
        return EdfReportDataReadStatus::RecordReadFailed;
    }
    BatchSeriesDecoder *items = static_cast<BatchSeriesDecoder *>(
        Memory::calloc_large(entry_count, sizeof(BatchSeriesDecoder), false));
    if (!items) return EdfReportDataReadStatus::RecordReadFailed;

    uint32_t first_record = UINT32_MAX;
    uint32_t end_record = 0;
    EdfReportDataReadStatus status = EdfReportDataReadStatus::Ok;
    for (size_t i = 0; i < entry_count; ++i) {
        const EdfReportDataPlanEntry &entry = entries[i];
        if (entry.kind != EdfReportDataKind::Series ||
            entry.record_count == 0 || entry.end_ms <= entry.start_ms) {
            status = EdfReportDataReadStatus::InvalidArgument;
            break;
        }
        if (entry.file_kind != entries[0].file_kind ||
            entry.file_slot != entries[0].file_slot) {
            status = EdfReportDataReadStatus::InvalidArgument;
            break;
        }

        EdfReportSeriesDecoder decoder;
        const EdfReportSeriesStatus init_status =
            edf_report_series_decoder_init(file_desc,
                                           header,
                                           header_size,
                                           entry.signal,
                                           entry.primary,
                                           decoder);
        if (init_status != EdfReportSeriesStatus::Ok ||
            decoder.signal_header.samples_per_record == 0 ||
            decoder.record_duration_ms %
                    decoder.signal_header.samples_per_record !=
                0) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
        const uint32_t interval_ms =
            decoder.record_duration_ms /
            decoder.signal_header.samples_per_record;
        if (interval_ms == 0 ||
            decoder.signal_header.samples_per_record > UINT32_MAX / 2u) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
        const uint32_t signal_bytes =
            decoder.signal_header.samples_per_record * 2u;
        const uint32_t signal_end =
            decoder.signal_header.byte_offset_in_record + signal_bytes;
        if (signal_end < decoder.signal_header.byte_offset_in_record ||
            signal_end > decoder.record_size ||
            decoder.record_size != session_file.record_size) {
            status = EdfReportDataReadStatus::RecordReadFailed;
            break;
        }
        const uint64_t item_end64 =
            static_cast<uint64_t>(entry.first_record) +
            static_cast<uint64_t>(entry.record_count);
        if (item_end64 > UINT32_MAX) {
            status = EdfReportDataReadStatus::RecordReadFailed;
            break;
        }

        BatchSeriesDecoder &item = items[i];
        item.decoder = decoder;
        item.first_record = entry.first_record;
        item.end_record = static_cast<uint32_t>(item_end64);
        item.interval_ms = interval_ms;
        item.active = true;
        item.emitter.callback = callback;
        item.emitter.context = context;
        item.emitter.item_index = i;
        item.stream.callback = emit_batch_series_sample;
        item.stream.context = &item.emitter;
        item.stream.interval_ms = interval_ms;
        if (derived_metric_edge_zero_padding(entry.signal)) {
            item.stream.trim_leading = entry.trim_leading_padding;
            item.stream.trim_trailing = entry.trim_trailing_padding;
            item.stream.leading_open = item.stream.trim_leading;
        }
        if (interval_ms_out) interval_ms_out[i] = interval_ms;
        if (metas) {
            metas[i] = {};
            metas[i].payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
            metas[i].record_count =
                entry.record_count_estimate ? entry.record_count_estimate
                                            : entry.record_count;
        }
        if (entry.first_record < first_record) first_record = entry.first_record;
        if (item.end_record > end_record) end_record = item.end_record;
    }

    uint8_t *record = nullptr;
    if (status == EdfReportDataReadStatus::Ok) {
        if (first_record == UINT32_MAX || end_record <= first_record) {
            status = EdfReportDataReadStatus::InvalidArgument;
        } else {
            record = static_cast<uint8_t *>(
                Memory::alloc_large(session_file.record_size, false));
            if (!record) status = EdfReportDataReadStatus::RecordReadFailed;
        }
    }

    if (status == EdfReportDataReadStatus::Ok) {
        status = edf_report_data_seek_record(file, session_file, first_record);
    }
    for (uint32_t record_index = first_record;
         status == EdfReportDataReadStatus::Ok && record_index < end_record;
         ++record_index) {
        status = edf_report_data_read_exact(file, record, session_file.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        for (size_t item_index = 0; item_index < entry_count; ++item_index) {
            BatchSeriesDecoder &item = items[item_index];
            if (!item.active || record_index < item.first_record ||
                record_index >= item.end_record) {
                continue;
            }
            EdfReportSeriesDecodeStats record_stats;
            const EdfReportDataPlanEntry &entry = entries[item_index];
            const EdfReportSeriesStatus decode_status =
                edf_report_decode_series_record(
                    item.decoder,
                    record,
                    session_file.record_size,
                    record_index,
                    entry.start_ms,
                    entry.end_ms,
                    edf_report_stream_series_record_sample,
                    &item.stream,
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
    }

    if (record) Memory::free(record);
    if (status == EdfReportDataReadStatus::Ok) {
        for (size_t i = 0; i < entry_count; ++i) {
            edf_report_stream_series_clear_zero_run(items[i].stream);
            stats.samples_emitted += items[i].stream.samples_emitted;
            stats.samples_missing += items[i].stream.samples_trimmed;
        }
    }
    Memory::free(items);
    return status;
}

EdfReportDataReadStatus stream_series_batch_plot_from_header(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    const EdfReportSeriesPlotConfig *configs,
    EdfReportFileDescriptor &file_desc,
    const uint8_t *header,
    size_t header_size,
    File &file,
    ReportStoreChunkMeta *metas,
    EdfReportDataReadStats &stats,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) {
    if (!entries || entry_count == 0 || !configs || !callback) {
        return EdfReportDataReadStatus::InvalidArgument;
    }
    if (entry_count > SIZE_MAX / sizeof(BatchPlotDecoder)) {
        return EdfReportDataReadStatus::RecordReadFailed;
    }
    BatchPlotDecoder *items = static_cast<BatchPlotDecoder *>(
        Memory::calloc_large(entry_count, sizeof(BatchPlotDecoder), false));
    if (!items) return EdfReportDataReadStatus::RecordReadFailed;

    uint32_t first_record = UINT32_MAX;
    uint32_t end_record = 0;
    EdfReportDataReadStatus status = EdfReportDataReadStatus::Ok;
    for (size_t i = 0; i < entry_count; ++i) {
        const EdfReportDataPlanEntry &entry = entries[i];
        const EdfReportSeriesPlotConfig &config = configs[i];
        if (entry.kind != EdfReportDataKind::Series ||
            entry.record_count == 0 || entry.end_ms <= entry.start_ms ||
            !config.ranges || config.range_count == 0 ||
            config.bucket_ms == 0 || config.gap_threshold_ms == 0 ||
            derived_metric_edge_zero_padding(entry.signal)) {
            status = EdfReportDataReadStatus::InvalidArgument;
            break;
        }
        if (entry.file_kind != entries[0].file_kind ||
            entry.file_slot != entries[0].file_slot) {
            status = EdfReportDataReadStatus::InvalidArgument;
            break;
        }

        EdfReportSeriesDecoder decoder;
        const EdfReportSeriesStatus init_status =
            edf_report_series_decoder_init(file_desc,
                                           header,
                                           header_size,
                                           entry.signal,
                                           entry.primary,
                                           decoder);
        if (init_status != EdfReportSeriesStatus::Ok ||
            decoder.signal_header.samples_per_record == 0 ||
            decoder.record_duration_ms %
                    decoder.signal_header.samples_per_record !=
                0 ||
            decoder.signal_scale.scale <= 0.0f) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
        const uint32_t interval_ms =
            decoder.record_duration_ms /
            decoder.signal_header.samples_per_record;
        if (interval_ms == 0 ||
            decoder.signal_header.samples_per_record > UINT32_MAX / 2u) {
            status = EdfReportDataReadStatus::DecodeFailed;
            break;
        }
        const uint32_t signal_bytes =
            decoder.signal_header.samples_per_record * 2u;
        const uint32_t signal_end =
            decoder.signal_header.byte_offset_in_record + signal_bytes;
        if (signal_end < decoder.signal_header.byte_offset_in_record ||
            signal_end > decoder.record_size ||
            decoder.record_size != session_file.record_size) {
            status = EdfReportDataReadStatus::RecordReadFailed;
            break;
        }
        const uint64_t item_end64 =
            static_cast<uint64_t>(entry.first_record) +
            static_cast<uint64_t>(entry.record_count);
        if (item_end64 > UINT32_MAX) {
            status = EdfReportDataReadStatus::RecordReadFailed;
            break;
        }

        BatchPlotDecoder &item = items[i];
        item.decoder = decoder;
        item.first_record = entry.first_record;
        item.end_record = static_cast<uint32_t>(item_end64);
        item.interval_ms = interval_ms;
        item.active = true;
        item.plot.ranges = config.ranges;
        item.plot.range_count = config.range_count;
        item.plot.plot_start_ms = config.plot_start_ms;
        item.plot.bucket_ms = config.bucket_ms;
        item.plot.gap_threshold_ms = config.gap_threshold_ms;
        item.plot.value_multiplier =
            config.value_multiplier == 0 ? 1 : config.value_multiplier;
        item.plot.callback = callback;
        item.plot.context = context;
        item.plot.item_index = i;
        if (interval_ms_out) interval_ms_out[i] = interval_ms;
        if (metas) {
            metas[i] = {};
            metas[i].payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
            metas[i].record_count =
                entry.record_count_estimate ? entry.record_count_estimate
                                            : entry.record_count;
        }
        if (entry.first_record < first_record) first_record = entry.first_record;
        if (item.end_record > end_record) end_record = item.end_record;
    }

    uint8_t *record = nullptr;
    if (status == EdfReportDataReadStatus::Ok) {
        if (first_record == UINT32_MAX || end_record <= first_record) {
            status = EdfReportDataReadStatus::InvalidArgument;
        } else {
            record = static_cast<uint8_t *>(
                Memory::alloc_large(session_file.record_size, false));
            if (!record) status = EdfReportDataReadStatus::RecordReadFailed;
        }
    }

    if (status == EdfReportDataReadStatus::Ok) {
        status = edf_report_data_seek_record(file, session_file, first_record);
    }
    for (uint32_t record_index = first_record;
         status == EdfReportDataReadStatus::Ok && record_index < end_record;
         ++record_index) {
        status = edf_report_data_read_exact(file, record, session_file.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        for (size_t item_index = 0; item_index < entry_count; ++item_index) {
            BatchPlotDecoder &item = items[item_index];
            if (!item.active || record_index < item.first_record ||
                record_index >= item.end_record) {
                continue;
            }
            const EdfReportDataPlanEntry &entry = entries[item_index];
            const int64_t record_start_ms =
                item.decoder.header_start_ms +
                static_cast<int64_t>(record_index) *
                    static_cast<int64_t>(item.decoder.record_duration_ms);
            for (uint32_t sample_index = 0;
                 sample_index <
                 item.decoder.signal_header.samples_per_record;
                 ++sample_index) {
                stats.samples_seen++;
                const int64_t sample_ms =
                    record_start_ms +
                    (static_cast<int64_t>(sample_index) *
                     static_cast<int64_t>(item.decoder.record_duration_ms)) /
                        static_cast<int64_t>(
                            item.decoder.signal_header.samples_per_record);
                if (sample_ms < entry.start_ms || sample_ms >= entry.end_ms) {
                    stats.samples_out_of_range++;
                    continue;
                }
                const int range_index =
                    edf_report_batch_plot_find_range(item.plot, sample_ms);
                if (range_index < 0) {
                    stats.samples_out_of_range++;
                    continue;
                }

                int16_t digital = 0;
                if (!edf_decode_signal_digital_sample(
                        item.decoder.signal_header,
                        record,
                        session_file.record_size,
                        sample_index,
                        digital)) {
                    status = EdfReportDataReadStatus::RecordReadFailed;
                    break;
                }
                if (edf_digital_sample_is_missing(item.decoder.signal_scale,
                                                  digital)) {
                    stats.samples_missing++;
                    continue;
                }
                if (!edf_report_batch_plot_record_sample(
                        item.plot,
                        item.decoder.signal_scale,
                        sample_ms,
                        digital,
                        range_index)) {
                    status = EdfReportDataReadStatus::CallbackRejected;
                    break;
                }
                stats.samples_emitted++;
            }
            if (status != EdfReportDataReadStatus::Ok) break;
        }
    }

    if (status == EdfReportDataReadStatus::Ok) {
        for (size_t i = 0; i < entry_count; ++i) {
            if (!edf_report_batch_plot_flush(items[i].plot,
                                             items[i].decoder.signal_scale)) {
                status = EdfReportDataReadStatus::CallbackRejected;
                break;
            }
        }
    }

    if (record) Memory::free(record);
    Memory::free(items);
    return status;
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

EdfReportDataReadStatus emit_series_entry_samples(
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
        status = edf_report_data_read_exact(file, record, session_file.record_size);
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
        emit_series_entry_samples(*session_file,
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
        status = stream_series_batch_from_header(*session_file,
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
        status = stream_series_batch_plot_from_header(*session_file,
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
