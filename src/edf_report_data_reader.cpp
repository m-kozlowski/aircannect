#include "edf_report_data_reader.h"

#include <FS.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "edf_report_event_reader.h"
#include "edf_report_series_reader.h"
#include "memory_manager.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

struct AppendEventContext {
    ReportSpoolBuffer *payload = nullptr;
    int64_t range_start_ms = 0;
    int64_t range_end_ms = 0;
    bool full = false;
};

struct BuildUniformSeriesContext {
    int64_t start_ms = 0;
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    int32_t *values = nullptr;
    uint8_t *missing_bitmap = nullptr;
    size_t missing_bitmap_bytes = 0;
    bool bad_sample = false;
};

struct UniformSeriesData {
    uint32_t interval_ms = 0;
    uint32_t sample_count = 0;
    int32_t *values = nullptr;
    uint8_t *missing_bitmap = nullptr;
    size_t missing_bitmap_bytes = 0;

    void clear() {
        if (missing_bitmap) {
            Memory::free(missing_bitmap);
            missing_bitmap = nullptr;
        }
        if (values) {
            Memory::free(values);
            values = nullptr;
        }
        interval_ms = 0;
        sample_count = 0;
        missing_bitmap_bytes = 0;
    }
};

struct StreamSeriesContext {
    ReportSeriesSampleCallback callback = nullptr;
    void *context = nullptr;
    uint32_t interval_ms = 0;
    bool trim_leading = false;
    bool trim_trailing = false;
    bool leading_open = false;
    bool pending_zero = false;
    int64_t pending_zero_start_ms = 0;
    int64_t pending_zero_next_ms = 0;
    uint32_t pending_zero_count = 0;
    uint32_t samples_emitted = 0;
    uint32_t samples_trimmed = 0;
};

struct BatchSeriesEmitter {
    EdfReportSeriesBatchSampleCallback callback = nullptr;
    void *context = nullptr;
    size_t item_index = 0;
};

struct BatchSeriesDecoder {
    EdfReportSeriesDecoder decoder;
    StreamSeriesContext stream;
    BatchSeriesEmitter emitter;
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    uint32_t interval_ms = 0;
    bool active = false;
};

struct BatchPlotBucket {
    bool have = false;
    int64_t start_t = 0;
    int64_t end_t = 0;
    int64_t min_t = 0;
    int64_t max_t = 0;
    int16_t start_digital = 0;
    int16_t end_digital = 0;
    int16_t min_digital = 0;
    int16_t max_digital = 0;

    void clear() {
        have = false;
        start_t = 0;
        end_t = 0;
        min_t = 0;
        max_t = 0;
        start_digital = 0;
        end_digital = 0;
        min_digital = 0;
        max_digital = 0;
    }
};

struct BatchPlotState {
    const EdfReportPlotRange *ranges = nullptr;
    size_t range_count = 0;
    int64_t plot_start_ms = 0;
    uint32_t bucket_ms = 1;
    uint32_t gap_threshold_ms = 5000;
    int32_t value_multiplier = 1;
    BatchPlotBucket bucket;
    bool have_last_sample = false;
    int64_t last_sample_ms = 0;
    int last_range_index = -1;
    int64_t current_bucket = -1;
    int64_t current_bucket_start_ms = 0;
    int64_t current_bucket_end_ms = 0;
    EdfReportSeriesBatchPlotCallback callback = nullptr;
    void *context = nullptr;
    size_t item_index = 0;
    uint32_t points_emitted = 0;
};

struct BatchPlotDecoder {
    EdfReportSeriesDecoder decoder;
    BatchPlotState plot;
    uint32_t first_record = 0;
    uint32_t end_record = 0;
    uint32_t interval_ms = 0;
    bool active = false;
};

void clear_missing_bit(uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return;
    bits[byte] &= static_cast<uint8_t>(~(1u << (index % 8u)));
}

void set_missing_bit(uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return;
    bits[byte] |= static_cast<uint8_t>(1u << (index % 8u));
}

bool missing_bit_set(const uint8_t *bits, size_t bytes, uint32_t index) {
    const size_t byte = static_cast<size_t>(index) / 8u;
    if (!bits || byte >= bytes) return true;
    return (bits[byte] & static_cast<uint8_t>(1u << (index % 8u))) != 0;
}

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

int32_t physical_to_milli(float value) {
    const long scaled = lroundf(value * 1000.0f);
    if (scaled < INT32_MIN) return INT32_MIN;
    if (scaled > INT32_MAX) return INT32_MAX;
    return static_cast<int32_t>(scaled);
}

int32_t scaled_digital_to_milli(const EdfSignalScale &scale,
                                int16_t digital,
                                int32_t multiplier) {
    int32_t value = physical_to_milli(edf_scale_digital_sample(scale, digital));
    if (multiplier != 1) {
        const int64_t scaled =
            static_cast<int64_t>(value) * static_cast<int64_t>(multiplier);
        value = scaled > INT32_MAX
                    ? INT32_MAX
                    : (scaled < INT32_MIN ? INT32_MIN
                                          : static_cast<int32_t>(scaled));
    }
    return value;
}

int find_plot_range(const BatchPlotState &plot, int64_t timestamp_ms) {
    if (!plot.ranges || plot.range_count == 0) return -1;
    if (plot.last_range_index >= 0 &&
        static_cast<size_t>(plot.last_range_index) < plot.range_count) {
        const EdfReportPlotRange &range =
            plot.ranges[plot.last_range_index];
        if (timestamp_ms >= range.start_ms && timestamp_ms < range.end_ms) {
            return plot.last_range_index;
        }
    }
    for (size_t i = 0; i < plot.range_count; ++i) {
        const EdfReportPlotRange &range = plot.ranges[i];
        if (timestamp_ms >= range.start_ms && timestamp_ms < range.end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool emit_plot_point(BatchPlotState &plot,
                     const EdfSignalScale &scale,
                     int64_t timestamp_ms,
                     int16_t digital) {
    if (!plot.callback) return false;
    EdfReportSeriesPlotPoint point;
    point.timestamp_ms = timestamp_ms;
    point.value_milli =
        scaled_digital_to_milli(scale, digital, plot.value_multiplier);
    if (!plot.callback(plot.context, plot.item_index, point)) return false;
    plot.points_emitted++;
    return true;
}

bool emit_plot_gap(BatchPlotState &plot) {
    if (!plot.callback) return false;
    EdfReportSeriesPlotPoint point;
    point.gap = true;
    if (!plot.callback(plot.context, plot.item_index, point)) return false;
    plot.have_last_sample = false;
    plot.last_sample_ms = 0;
    plot.last_range_index = -1;
    plot.current_bucket = -1;
    plot.current_bucket_start_ms = 0;
    plot.current_bucket_end_ms = 0;
    return true;
}

bool flush_plot_bucket(BatchPlotState &plot,
                       const EdfSignalScale &scale) {
    BatchPlotBucket &bucket = plot.bucket;
    if (!bucket.have) return true;
    struct Point {
        int64_t t = 0;
        int16_t digital = 0;
    };
    Point points[4] = {
        {bucket.start_t, bucket.start_digital},
        {bucket.min_t, bucket.min_digital},
        {bucket.max_t, bucket.max_digital},
        {bucket.end_t, bucket.end_digital},
    };
    for (size_t i = 1; i < 4; ++i) {
        Point v = points[i];
        size_t j = i;
        while (j > 0 && points[j - 1].t > v.t) {
            points[j] = points[j - 1];
            --j;
        }
        points[j] = v;
    }
    bool emitted[4] = {};
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;
        if (!emit_plot_point(plot,
                             scale,
                             points[i].t,
                             points[i].digital)) {
            return false;
        }
        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }
    bucket.clear();
    return true;
}

bool record_plot_digital_sample(BatchPlotState &plot,
                                const EdfSignalScale &scale,
                                int64_t timestamp_ms,
                                int16_t digital,
                                int range_index) {
    if (range_index < 0) return true;
    if (plot.have_last_sample &&
        (range_index != plot.last_range_index ||
         timestamp_ms >
             plot.last_sample_ms +
                 static_cast<int64_t>(plot.gap_threshold_ms))) {
        if (!flush_plot_bucket(plot, scale)) return false;
        if (!emit_plot_gap(plot)) return false;
    }

    const uint32_t bucket_ms = plot.bucket_ms ? plot.bucket_ms : 1u;
    int64_t sample_bucket = plot.current_bucket;
    if (plot.current_bucket < 0 ||
        timestamp_ms < plot.current_bucket_start_ms ||
        timestamp_ms >= plot.current_bucket_end_ms) {
        sample_bucket =
            (timestamp_ms - plot.plot_start_ms) /
            static_cast<int64_t>(bucket_ms);
        if (sample_bucket < 0) sample_bucket = 0;
    }
    if (plot.current_bucket != sample_bucket) {
        if (!flush_plot_bucket(plot, scale)) return false;
        plot.current_bucket = sample_bucket;
        plot.current_bucket_start_ms =
            plot.plot_start_ms +
            sample_bucket * static_cast<int64_t>(bucket_ms);
        plot.current_bucket_end_ms =
            plot.current_bucket_start_ms + static_cast<int64_t>(bucket_ms);
    }

    BatchPlotBucket &bucket = plot.bucket;
    if (!bucket.have) {
        bucket.have = true;
        bucket.start_t = timestamp_ms;
        bucket.end_t = timestamp_ms;
        bucket.min_t = timestamp_ms;
        bucket.max_t = timestamp_ms;
        bucket.start_digital = digital;
        bucket.end_digital = digital;
        bucket.min_digital = digital;
        bucket.max_digital = digital;
    } else {
        bucket.end_t = timestamp_ms;
        bucket.end_digital = digital;
        if (digital < bucket.min_digital) {
            bucket.min_t = timestamp_ms;
            bucket.min_digital = digital;
        }
        if (digital > bucket.max_digital) {
            bucket.max_t = timestamp_ms;
            bucket.max_digital = digital;
        }
    }

    plot.have_last_sample = true;
    plot.last_sample_ms = timestamp_ms;
    plot.last_range_index = range_index;
    return true;
}

uint32_t trim_edge_zero_padding(BuildUniformSeriesContext &ctx,
                                bool trim_leading,
                                bool trim_trailing) {
    if (!ctx.values || !ctx.missing_bitmap || ctx.sample_count == 0) {
        return 0;
    }
    uint32_t trimmed = 0;
    if (trim_leading) {
        for (uint32_t i = 0; i < ctx.sample_count; ++i) {
            if (missing_bit_set(ctx.missing_bitmap,
                                ctx.missing_bitmap_bytes,
                                i)) {
                continue;
            }
            if (ctx.values[i] != 0) break;
            set_missing_bit(ctx.missing_bitmap, ctx.missing_bitmap_bytes, i);
            trimmed++;
        }
    }
    if (trim_trailing) {
        for (uint32_t i = ctx.sample_count; i > 0; --i) {
            const uint32_t index = i - 1;
            if (missing_bit_set(ctx.missing_bitmap,
                                ctx.missing_bitmap_bytes,
                                index)) {
                continue;
            }
            if (ctx.values[index] != 0) break;
            set_missing_bit(ctx.missing_bitmap,
                            ctx.missing_bitmap_bytes,
                            index);
            trimmed++;
        }
    }
    return trimmed;
}

bool record_uniform_series_sample(void *context,
                                  const ReportSeriesSample &sample) {
    BuildUniformSeriesContext *ctx =
        static_cast<BuildUniformSeriesContext *>(context);
    if (!ctx || !ctx->values || !ctx->missing_bitmap ||
        ctx->interval_ms == 0 || sample.timestamp_ms < ctx->start_ms) {
        return false;
    }
    const int64_t delta = sample.timestamp_ms - ctx->start_ms;
    if (delta < 0 || delta % static_cast<int64_t>(ctx->interval_ms) != 0) {
        ctx->bad_sample = true;
        return false;
    }
    const int64_t index64 = delta / static_cast<int64_t>(ctx->interval_ms);
    if (index64 < 0 || index64 > UINT32_MAX ||
        static_cast<uint32_t>(index64) >= ctx->sample_count) {
        ctx->bad_sample = true;
        return false;
    }
    const uint32_t index = static_cast<uint32_t>(index64);
    ctx->values[index] = sample.value_milli;
    clear_missing_bit(ctx->missing_bitmap,
                      ctx->missing_bitmap_bytes,
                      index);
    return true;
}

bool emit_stream_series_sample(StreamSeriesContext &ctx,
                               const ReportSeriesSample &sample) {
    if (!ctx.callback) return false;
    if (!ctx.callback(ctx.context, sample)) return false;
    ctx.samples_emitted++;
    return true;
}

bool flush_stream_zero_run(StreamSeriesContext &ctx) {
    if (!ctx.pending_zero) return true;
    if (ctx.interval_ms == 0) return false;
    for (uint32_t i = 0; i < ctx.pending_zero_count; ++i) {
        ReportSeriesSample zero;
        zero.timestamp_ms =
            ctx.pending_zero_start_ms +
            static_cast<int64_t>(i) * static_cast<int64_t>(ctx.interval_ms);
        zero.value_milli = 0;
        if (!emit_stream_series_sample(ctx, zero)) return false;
    }
    ctx.pending_zero = false;
    ctx.pending_zero_start_ms = 0;
    ctx.pending_zero_next_ms = 0;
    ctx.pending_zero_count = 0;
    return true;
}

void clear_stream_zero_run(StreamSeriesContext &ctx) {
    if (!ctx.pending_zero) return;
    ctx.samples_trimmed += ctx.pending_zero_count;
    ctx.pending_zero = false;
    ctx.pending_zero_start_ms = 0;
    ctx.pending_zero_next_ms = 0;
    ctx.pending_zero_count = 0;
}

bool record_stream_series_sample(void *context,
                                 const ReportSeriesSample &sample) {
    StreamSeriesContext *ctx = static_cast<StreamSeriesContext *>(context);
    if (!ctx || !ctx->callback) return false;
    const bool zero = sample.value_milli == 0;

    if (ctx->trim_leading && ctx->leading_open) {
        if (zero) {
            ctx->samples_trimmed++;
            return true;
        }
        ctx->leading_open = false;
    }

    if (ctx->trim_trailing && zero) {
        if (!ctx->pending_zero ||
            sample.timestamp_ms != ctx->pending_zero_next_ms) {
            if (!flush_stream_zero_run(*ctx)) return false;
            ctx->pending_zero = true;
            ctx->pending_zero_start_ms = sample.timestamp_ms;
            ctx->pending_zero_count = 0;
        }
        ctx->pending_zero_count++;
        ctx->pending_zero_next_ms =
            sample.timestamp_ms +
            static_cast<int64_t>(ctx->interval_ms);
        return true;
    }

    if (!flush_stream_zero_run(*ctx)) return false;
    return emit_stream_series_sample(*ctx, sample);
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

bool emit_batch_series_sample(void *context,
                              const ReportSeriesSample &sample) {
    BatchSeriesEmitter *emitter =
        static_cast<BatchSeriesEmitter *>(context);
    if (!emitter || !emitter->callback) return false;
    return emitter->callback(emitter->context, emitter->item_index, sample);
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

EdfReportDataReadStatus open_data_file(
    const EdfReportSessionFileDescriptor &session_file,
    File &file) {
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

    EdfReportDataReadStatus open_status = open_data_file(session_file, file);
    if (open_status != EdfReportDataReadStatus::Ok) return open_status;

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
        0,
        file_desc);
    if (desc_status != EdfReportFileStatus::Ok) {
        Memory::free(header);
        header = nullptr;
        Storage::Guard guard;
        file.close();
        return EdfReportDataReadStatus::HeaderParseFailed;
    }
    file_desc.header_start_ms = session_file.header_start_ms;
    file_desc.header_end_ms = session_file.header_end_ms;
    file_desc.inventory.complete_records_from_size =
        session_file.complete_records;
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

EdfReportDataReadStatus collect_series_entry_decoded(
    const EdfReportSessionFileDescriptor &session_file,
    const EdfReportDataPlanEntry &entry,
    const EdfReportSeriesDecoder &decoder,
    File &file,
    UniformSeriesData &series,
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

    BuildUniformSeriesContext ctx;
    ctx.start_ms = entry.start_ms;
    ctx.interval_ms = interval_ms;
    ctx.sample_count = sample_count;
    ctx.values = values;
    ctx.missing_bitmap = missing_bitmap;
    ctx.missing_bitmap_bytes = missing_bitmap_bytes;
    EdfReportDataReadStatus status =
        seek_record(file, session_file, entry.first_record);
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        if (status != EdfReportDataReadStatus::Ok) break;
        status = read_exact(file, record, decoder.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        EdfReportSeriesDecodeStats record_stats;
        const EdfReportSeriesStatus decode_status =
            edf_report_decode_series_record(decoder,
                                            record,
                                            decoder.record_size,
                                            entry.first_record + i,
                                            entry.start_ms,
                                            entry.end_ms,
                                            record_uniform_series_sample,
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
            const uint32_t trimmed = trim_edge_zero_padding(
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
    UniformSeriesData &series,
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

    StreamSeriesContext ctx;
    ctx.callback = callback;
    ctx.context = context;
    ctx.interval_ms = interval_ms;
    if (derived_metric_edge_zero_padding(entry.signal)) {
        ctx.trim_leading = entry.trim_leading_padding;
        ctx.trim_trailing = entry.trim_trailing_padding;
        ctx.leading_open = ctx.trim_leading;
    }

    EdfReportDataReadStatus status =
        seek_record(file, session_file, entry.first_record);
    for (uint32_t i = 0; i < entry.record_count; ++i) {
        if (status != EdfReportDataReadStatus::Ok) break;
        status = read_exact(file, record, decoder.record_size);
        if (status != EdfReportDataReadStatus::Ok) break;
        stats.records_read++;
        EdfReportSeriesDecodeStats record_stats;
        const EdfReportSeriesStatus decode_status =
            edf_report_decode_series_record(decoder,
                                            record,
                                            decoder.record_size,
                                            entry.first_record + i,
                                            entry.start_ms,
                                            entry.end_ms,
                                            record_stream_series_sample,
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
        clear_stream_zero_run(ctx);
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
        status = seek_record(file, session_file, first_record);
    }
    for (uint32_t record_index = first_record;
         status == EdfReportDataReadStatus::Ok && record_index < end_record;
         ++record_index) {
        status = read_exact(file, record, session_file.record_size);
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
                edf_report_decode_series_record(item.decoder,
                                                record,
                                                session_file.record_size,
                                                record_index,
                                                entry.start_ms,
                                                entry.end_ms,
                                                record_stream_series_sample,
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
            clear_stream_zero_run(items[i].stream);
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
        status = seek_record(file, session_file, first_record);
    }
    for (uint32_t record_index = first_record;
         status == EdfReportDataReadStatus::Ok && record_index < end_record;
         ++record_index) {
        status = read_exact(file, record, session_file.record_size);
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
                    find_plot_range(item.plot, sample_ms);
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
                if (!record_plot_digital_sample(item.plot,
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
            if (!flush_plot_bucket(items[i].plot,
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
    UniformSeriesData series;
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
        read_header(session_file, file_desc, header, header_size, file);
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
        entry_file(session, entry);
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
        entry_file(session, entry);
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

    if (file) {
        Storage::Guard guard;
        file.close();
    }
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
        entry_file(session, entries[0]);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;
    for (size_t i = 1; i < entry_count; ++i) {
        const EdfReportSessionFileDescriptor *other =
            entry_file(session, entries[i]);
        if (other != session_file) {
            return EdfReportDataReadStatus::InvalidArgument;
        }
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        read_header(*session_file, file_desc, header, header_size, file);
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
    if (file) {
        Storage::Guard guard;
        file.close();
    }
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
        entry_file(session, entries[0]);
    if (!session_file) return EdfReportDataReadStatus::InvalidArgument;
    for (size_t i = 1; i < entry_count; ++i) {
        const EdfReportSessionFileDescriptor *other =
            entry_file(session, entries[i]);
        if (other != session_file) {
            return EdfReportDataReadStatus::InvalidArgument;
        }
    }

    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    File file;
    EdfReportDataReadStatus status =
        read_header(*session_file, file_desc, header, header_size, file);
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
    if (file) {
        Storage::Guard guard;
        file.close();
    }
    if (status != EdfReportDataReadStatus::Ok && metas) {
        for (size_t i = 0; i < entry_count; ++i) metas[i] = {};
    }
    return status;
}

}  // namespace aircannect
