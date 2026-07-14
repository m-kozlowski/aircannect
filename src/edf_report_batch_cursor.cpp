#include "edf_report_batch_reader.h"

#include <limits.h>

#include <Arduino.h>

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
};

struct BatchPlotDecoder {
    EdfReportSeriesDecoder decoder;
    EdfReportBatchPlotState plot;
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    uint32_t interval_ms = 0;
};

bool emit_batch_series_sample(void *context,
                              const ReportSeriesSample &sample) {
    BatchSeriesEmitter *emitter =
        static_cast<BatchSeriesEmitter *>(context);
    if (!emitter || !emitter->callback) return false;

    return emitter->callback(emitter->context, emitter->item_index, sample);
}

bool validate_decoder(const EdfReportSessionFileDescriptor &session_file,
                      const EdfReportSeriesDecoder &decoder,
                      uint32_t &interval_ms) {
    if (decoder.signal_header.samples_per_record == 0 ||
        decoder.record_duration_ms %
                decoder.signal_header.samples_per_record !=
            0) {
        return false;
    }

    interval_ms = decoder.record_duration_ms /
                  decoder.signal_header.samples_per_record;
    if (interval_ms == 0 ||
        decoder.signal_header.samples_per_record > UINT32_MAX / 2u) {
        return false;
    }

    const uint32_t signal_bytes =
        decoder.signal_header.samples_per_record * 2u;
    const uint32_t signal_end =
        decoder.signal_header.byte_offset_in_record + signal_bytes;
    return signal_end >= decoder.signal_header.byte_offset_in_record &&
           signal_end <= decoder.record_size &&
           decoder.record_size == session_file.record_size;
}

bool entry_end_record(const EdfReportDataPlanEntry &entry,
                      uint32_t &end_record) {
    const uint64_t end = static_cast<uint64_t>(entry.first_record) +
                         static_cast<uint64_t>(entry.record_count);
    if (end > UINT32_MAX) return false;

    end_record = static_cast<uint32_t>(end);
    return true;
}

}  // namespace

EdfReportBatchReaderCursor::~EdfReportBatchReaderCursor() {
    reset();
}

bool EdfReportBatchReaderCursor::start_common(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry *entries,
    size_t entry_count,
    EdfReportFileDescriptor &file_desc,
    File &file,
    EdfReportDataReadStats &stats) {
    reset();
    stats = {};
    if (!entries || entry_count == 0 || !file || session_file.record_size == 0) {
        status_ = EdfReportDataReadStatus::InvalidArgument;
        state_ = State::Failed;
        return false;
    }

    session_file_ = &session_file;
    entries_ = entries;
    entry_count_ = entry_count;
    file_desc_ = &file_desc;
    file_ = &file;
    stats_ = &stats;
    current_record_ = UINT32_MAX;
    end_record_ = 0;
    return true;
}

bool EdfReportBatchReaderCursor::prepare_sample_items(
    ReportStoreChunkMeta *metas,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) {
    if (!callback || entry_count_ > SIZE_MAX / sizeof(BatchSeriesDecoder)) {
        return false;
    }

    BatchSeriesDecoder *items = static_cast<BatchSeriesDecoder *>(
        Memory::calloc_large(entry_count_, sizeof(BatchSeriesDecoder), false));
    if (!items) return false;
    items_ = items;

    for (size_t i = 0; i < entry_count_; ++i) {
        const EdfReportDataPlanEntry &entry = entries_[i];
        if (entry.kind != EdfReportDataKind::Series ||
            entry.record_count == 0 || entry.end_ms <= entry.start_ms ||
            entry.file_kind != entries_[0].file_kind ||
            entry.file_slot != entries_[0].file_slot) {
            return false;
        }

        EdfReportSeriesDecoder decoder;
        const EdfReportSeriesStatus init_status =
            edf_report_series_decoder_init(*file_desc_,
                                           header_,
                                           header_size_,
                                           entry.signal,
                                           entry.primary,
                                           decoder);
        uint32_t interval_ms = 0;
        if (init_status != EdfReportSeriesStatus::Ok ||
            !validate_decoder(*session_file_, decoder, interval_ms)) {
            return false;
        }

        uint32_t end_record = 0;
        if (!entry_end_record(entry, end_record)) return false;

        BatchSeriesDecoder &item = items[i];
        item.decoder = decoder;
        item.first_record = entry.first_record;
        item.end_record = end_record;
        item.interval_ms = interval_ms;
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
            metas[i].record_count = entry.record_count_estimate
                                        ? entry.record_count_estimate
                                        : entry.record_count;
        }

        if (entry.first_record < current_record_) {
            current_record_ = entry.first_record;
        }
        if (end_record > end_record_) end_record_ = end_record;
    }

    return current_record_ != UINT32_MAX && end_record_ > current_record_;
}

bool EdfReportBatchReaderCursor::prepare_plot_items(
    const EdfReportSeriesPlotConfig *configs,
    ReportStoreChunkMeta *metas,
    uint32_t *interval_ms_out,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) {
    if (!configs || !callback ||
        entry_count_ > SIZE_MAX / sizeof(BatchPlotDecoder)) {
        return false;
    }

    BatchPlotDecoder *items = static_cast<BatchPlotDecoder *>(
        Memory::calloc_large(entry_count_, sizeof(BatchPlotDecoder), false));
    if (!items) return false;
    items_ = items;

    for (size_t i = 0; i < entry_count_; ++i) {
        const EdfReportDataPlanEntry &entry = entries_[i];
        const EdfReportSeriesPlotConfig &config = configs[i];
        if (entry.kind != EdfReportDataKind::Series ||
            entry.record_count == 0 || entry.end_ms <= entry.start_ms ||
            entry.file_kind != entries_[0].file_kind ||
            entry.file_slot != entries_[0].file_slot ||
            !config.ranges || config.range_count == 0 ||
            config.bucket_ms == 0 || config.gap_threshold_ms == 0 ||
            edf_report_signal_uses_edge_zero_padding(entry.signal)) {
            return false;
        }

        EdfReportSeriesDecoder decoder;
        const EdfReportSeriesStatus init_status =
            edf_report_series_decoder_init(*file_desc_,
                                           header_,
                                           header_size_,
                                           entry.signal,
                                           entry.primary,
                                           decoder);
        uint32_t interval_ms = 0;
        if (init_status != EdfReportSeriesStatus::Ok ||
            decoder.signal_scale.scale <= 0.0f ||
            !validate_decoder(*session_file_, decoder, interval_ms)) {
            return false;
        }

        uint32_t end_record = 0;
        if (!entry_end_record(entry, end_record)) return false;

        BatchPlotDecoder &item = items[i];
        item.decoder = decoder;
        item.first_record = entry.first_record;
        item.end_record = end_record;
        item.interval_ms = interval_ms;
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
            metas[i].record_count = entry.record_count_estimate
                                        ? entry.record_count_estimate
                                        : entry.record_count;
        }

        if (entry.first_record < current_record_) {
            current_record_ = entry.first_record;
        }
        if (end_record > end_record_) end_record_ = end_record;
    }

    return current_record_ != UINT32_MAX && end_record_ > current_record_;
}

bool EdfReportBatchReaderCursor::start_samples(
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
    if (metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    if (interval_ms_out) {
        for (size_t i = 0; i < entry_count; ++i) interval_ms_out[i] = 0;
    }
    if (!start_common(session_file,
                      entries,
                      entry_count,
                      file_desc,
                      file,
                      stats)) {
        return false;
    }

    mode_ = Mode::Samples;
    header_ = header;
    header_size_ = header_size;
    if (!header_ || header_size_ == 0 ||
        !prepare_sample_items(metas, interval_ms_out, callback, context)) {
        fail(EdfReportDataReadStatus::DecodeFailed);
        return false;
    }

    record_ = static_cast<uint8_t *>(
        Memory::alloc_large(session_file.record_size, false));
    if (!record_) {
        fail(EdfReportDataReadStatus::RecordReadFailed);
        return false;
    }

    status_ = edf_report_data_seek_record(file, session_file, current_record_);
    if (status_ != EdfReportDataReadStatus::Ok) {
        state_ = State::Failed;
        return false;
    }

    state_ = State::Active;
    return true;
}

bool EdfReportBatchReaderCursor::start_plot(
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
    if (metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    if (interval_ms_out) {
        for (size_t i = 0; i < entry_count; ++i) interval_ms_out[i] = 0;
    }
    if (!start_common(session_file,
                      entries,
                      entry_count,
                      file_desc,
                      file,
                      stats)) {
        return false;
    }

    mode_ = Mode::Plot;
    header_ = header;
    header_size_ = header_size;
    if (!header_ || header_size_ == 0 ||
        !prepare_plot_items(configs,
                            metas,
                            interval_ms_out,
                            callback,
                            context)) {
        fail(EdfReportDataReadStatus::DecodeFailed);
        return false;
    }

    record_ = static_cast<uint8_t *>(
        Memory::alloc_large(session_file.record_size, false));
    if (!record_) {
        fail(EdfReportDataReadStatus::RecordReadFailed);
        return false;
    }

    status_ = edf_report_data_seek_record(file, session_file, current_record_);
    if (status_ != EdfReportDataReadStatus::Ok) {
        state_ = State::Failed;
        return false;
    }

    state_ = State::Active;
    return true;
}

bool EdfReportBatchReaderCursor::process_record(uint32_t record_index) {
    status_ = edf_report_data_read_exact(*file_,
                                         record_,
                                         session_file_->record_size);
    if (status_ != EdfReportDataReadStatus::Ok) return false;

    stats_->records_read++;
    if (mode_ == Mode::Samples) {
        BatchSeriesDecoder *items = static_cast<BatchSeriesDecoder *>(items_);
        for (size_t i = 0; i < entry_count_; ++i) {
            BatchSeriesDecoder &item = items[i];
            if (record_index < item.first_record ||
                record_index >= item.end_record) {
                continue;
            }

            EdfReportSeriesDecodeStats record_stats;
            const EdfReportSeriesStatus decode_status =
                edf_report_decode_series_record(
                    item.decoder,
                    record_,
                    session_file_->record_size,
                    record_index,
                    entries_[i].start_ms,
                    entries_[i].end_ms,
                    edf_report_stream_series_record_sample,
                    &item.stream,
                    record_stats);
            stats_->samples_seen += record_stats.samples_seen;
            stats_->samples_missing += record_stats.samples_missing;
            stats_->samples_out_of_range +=
                record_stats.samples_out_of_range;

            if (decode_status == EdfReportSeriesStatus::CallbackRejected) {
                status_ = EdfReportDataReadStatus::CallbackRejected;
                return false;
            }
            if (decode_status != EdfReportSeriesStatus::Ok) {
                status_ = EdfReportDataReadStatus::DecodeFailed;
                return false;
            }
        }
        return true;
    }

    BatchPlotDecoder *items = static_cast<BatchPlotDecoder *>(items_);
    for (size_t i = 0; i < entry_count_; ++i) {
        BatchPlotDecoder &item = items[i];
        if (record_index < item.first_record ||
            record_index >= item.end_record) {
            continue;
        }

        const EdfReportDataPlanEntry &entry = entries_[i];
        const int64_t record_start_ms =
            item.decoder.header_start_ms +
            static_cast<int64_t>(record_index) *
                static_cast<int64_t>(item.decoder.record_duration_ms);

        for (uint32_t sample_index = 0;
             sample_index < item.decoder.signal_header.samples_per_record;
             ++sample_index) {
            stats_->samples_seen++;

            const int64_t sample_ms =
                record_start_ms +
                (static_cast<int64_t>(sample_index) *
                 static_cast<int64_t>(item.decoder.record_duration_ms)) /
                    static_cast<int64_t>(
                        item.decoder.signal_header.samples_per_record);
            if (sample_ms < entry.start_ms || sample_ms >= entry.end_ms) {
                stats_->samples_out_of_range++;
                continue;
            }

            const int range_index =
                edf_report_batch_plot_find_range(item.plot, sample_ms);
            if (range_index < 0) {
                stats_->samples_out_of_range++;
                continue;
            }

            int16_t digital = 0;
            if (!edf_decode_signal_digital_sample(
                    item.decoder.signal_header,
                    record_,
                    session_file_->record_size,
                    sample_index,
                    digital)) {
                status_ = EdfReportDataReadStatus::RecordReadFailed;
                return false;
            }

            if (edf_digital_sample_is_missing(item.decoder.signal_scale,
                                              digital)) {
                stats_->samples_missing++;
                continue;
            }

            if (!edf_report_batch_plot_record_sample(item.plot,
                                                     item.decoder.signal_scale,
                                                     sample_ms,
                                                     digital,
                                                     range_index)) {
                status_ = EdfReportDataReadStatus::CallbackRejected;
                return false;
            }
            stats_->samples_emitted++;
        }
    }

    return true;
}

bool EdfReportBatchReaderCursor::finish() {
    if (mode_ == Mode::Samples) {
        BatchSeriesDecoder *items = static_cast<BatchSeriesDecoder *>(items_);
        for (size_t i = 0; i < entry_count_; ++i) {
            edf_report_stream_series_clear_zero_run(items[i].stream);
            stats_->samples_emitted += items[i].stream.samples_emitted;
            stats_->samples_missing += items[i].stream.samples_trimmed;
        }
    } else if (mode_ == Mode::Plot) {
        BatchPlotDecoder *items = static_cast<BatchPlotDecoder *>(items_);
        for (size_t i = 0; i < entry_count_; ++i) {
            if (!edf_report_batch_plot_flush(items[i].plot,
                                             items[i].decoder.signal_scale)) {
                fail(EdfReportDataReadStatus::CallbackRejected);
                return false;
            }
        }
    }

    status_ = EdfReportDataReadStatus::Ok;
    state_ = State::Complete;
    return true;
}

EdfReportBatchPollResult EdfReportBatchReaderCursor::poll(
    uint32_t budget_ms) {
    if (state_ == State::Complete) return EdfReportBatchPollResult::Complete;
    if (state_ != State::Active) return EdfReportBatchPollResult::Failed;

    const uint32_t started_ms = millis();
    uint32_t records = 0;
    while (current_record_ < end_record_) {
        if (!process_record(current_record_)) {
            state_ = State::Failed;
            return EdfReportBatchPollResult::Failed;
        }

        ++current_record_;
        ++records;
        if (records > 0 &&
            static_cast<uint32_t>(millis() - started_ms) >= budget_ms) {
            break;
        }
    }

    if (current_record_ < end_record_) {
        return EdfReportBatchPollResult::Pending;
    }

    return finish() ? EdfReportBatchPollResult::Complete
                    : EdfReportBatchPollResult::Failed;
}

void EdfReportBatchReaderCursor::fail(EdfReportDataReadStatus status) {
    status_ = status;
    state_ = State::Failed;
}

void EdfReportBatchReaderCursor::reset() {
    if (record_) Memory::free(record_);
    if (items_) Memory::free(items_);

    mode_ = Mode::None;
    state_ = State::Idle;
    status_ = EdfReportDataReadStatus::Ok;
    session_file_ = nullptr;
    entries_ = nullptr;
    entry_count_ = 0;
    file_desc_ = nullptr;
    header_ = nullptr;
    header_size_ = 0;
    file_ = nullptr;
    stats_ = nullptr;
    items_ = nullptr;
    record_ = nullptr;
    current_record_ = 0;
    end_record_ = 0;
}

}  // namespace aircannect
