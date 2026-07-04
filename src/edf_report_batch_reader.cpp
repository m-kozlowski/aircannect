#include "edf_report_batch_reader.h"

#include <limits.h>

#include "edf_file_reader.h"
#include "edf_report_batch_plot.h"
#include "edf_report_data_file.h"
#include "edf_report_series_reader.h"
#include "edf_report_stream_series.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

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

bool emit_batch_series_sample(void *context,
                              const ReportSeriesSample &sample) {
    BatchSeriesEmitter *emitter =
        static_cast<BatchSeriesEmitter *>(context);
    if (!emitter || !emitter->callback) return false;

    return emitter->callback(emitter->context, emitter->item_index, sample);
}

}  // namespace

EdfReportDataReadStatus edf_report_stream_series_batch_from_header(
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

        if (edf_report_signal_uses_edge_zero_padding(entry.signal)) {
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
        status = edf_report_data_read_exact(file,
                                            record,
                                            session_file.record_size);
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

EdfReportDataReadStatus edf_report_stream_series_batch_plot_from_header(
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
            edf_report_signal_uses_edge_zero_padding(entry.signal)) {
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
        status = edf_report_data_read_exact(file,
                                            record,
                                            session_file.record_size);
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

}  // namespace aircannect
