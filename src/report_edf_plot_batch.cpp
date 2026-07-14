#include "report_edf_plot_batch.h"

#include <algorithm>
#include <limits.h>
#include <string.h>

#include "edf_report_data_reader.h"
#include "edf_report_provider_token.h"
#include "memory_manager.h"
#include "report_data_provider.h"
#include "report_plot_payload.h"
#include "report_result_provider_bridge.h"

namespace aircannect {
namespace {

constexpr size_t NO_READER_GROUP = SIZE_MAX;

size_t stream_count_in_chunk(uint32_t mask,
                             size_t fallback_stream_index,
                             size_t stream_count) {
    if (mask == 0) return fallback_stream_index < stream_count ? 1 : 0;

    size_t count = 0;
    for (size_t i = 0; i < stream_count && i < 32; ++i) {
        if (mask & (1u << i)) ++count;
    }
    return count;
}

}  // namespace

struct ReportEdfPlotBatch::ReaderGroup {
    EdfReportProviderReaderGroupKey key;
};

ReportEdfPlotBatch::~ReportEdfPlotBatch() {
    release_workspace();
}

bool ReportEdfPlotBatch::ensure_candidate_capacity(size_t capacity) {
    if (capacity <= candidate_capacity_) return true;
    if (capacity == 0 ||
        capacity > SIZE_MAX / sizeof(ReportProviderChunk) ||
        capacity > SIZE_MAX / sizeof(ReportResultChunk) ||
        capacity > SIZE_MAX / sizeof(size_t) ||
        capacity > SIZE_MAX / sizeof(uint8_t) ||
        capacity > SIZE_MAX / sizeof(EdfReportSeriesPlotConfig)) {
        return false;
    }

    ReportProviderChunk *candidates =
        static_cast<ReportProviderChunk *>(Memory::calloc_large(
            capacity, sizeof(ReportProviderChunk), false));
    ReportResultChunk *logical_chunks =
        static_cast<ReportResultChunk *>(Memory::calloc_large(
            capacity, sizeof(ReportResultChunk), false));
    size_t *chunk_indices = static_cast<size_t *>(Memory::calloc_large(
        capacity, sizeof(size_t), false));
    uint8_t *stream_indices = static_cast<uint8_t *>(Memory::calloc_large(
        capacity, sizeof(uint8_t), false));
    EdfReportSeriesPlotConfig *plot_configs =
        static_cast<EdfReportSeriesPlotConfig *>(Memory::calloc_large(
            capacity, sizeof(EdfReportSeriesPlotConfig), false));
    if (!candidates || !logical_chunks || !chunk_indices ||
        !stream_indices || !plot_configs) {
        if (candidates) Memory::free(candidates);
        if (logical_chunks) Memory::free(logical_chunks);
        if (chunk_indices) Memory::free(chunk_indices);
        if (stream_indices) Memory::free(stream_indices);
        if (plot_configs) Memory::free(plot_configs);
        return false;
    }

    if (candidates_) Memory::free(candidates_);
    if (logical_chunks_) Memory::free(logical_chunks_);
    if (chunk_indices_) Memory::free(chunk_indices_);
    if (stream_indices_) Memory::free(stream_indices_);
    if (plot_configs_) Memory::free(plot_configs_);
    candidates_ = candidates;
    logical_chunks_ = logical_chunks;
    chunk_indices_ = chunk_indices;
    stream_indices_ = stream_indices;
    plot_configs_ = plot_configs;
    candidate_capacity_ = capacity;
    return true;
}

bool ReportEdfPlotBatch::ensure_chunk_capacity(size_t capacity) {
    if (capacity <= physical_capacity_) return true;
    if (capacity == 0 || capacity > SIZE_MAX / sizeof(bool)) return false;

    bool *physical_counted = static_cast<bool *>(
        Memory::calloc_large(capacity, sizeof(bool), false));
    if (!physical_counted) return false;

    if (physical_counted_) Memory::free(physical_counted_);
    physical_counted_ = physical_counted;
    physical_capacity_ = capacity;
    return true;
}

bool ReportEdfPlotBatch::ensure_range_capacity(size_t capacity) {
    if (capacity <= range_capacity_) return true;
    if (capacity == 0 ||
        capacity > SIZE_MAX / sizeof(EdfReportPlotRange)) {
        return false;
    }

    EdfReportPlotRange *ranges =
        static_cast<EdfReportPlotRange *>(Memory::calloc_large(
            capacity, sizeof(EdfReportPlotRange), false));
    if (!ranges) return false;

    if (ranges_) Memory::free(ranges_);
    ranges_ = ranges;
    range_capacity_ = capacity;
    return true;
}

bool ReportEdfPlotBatch::ensure_plan_capacity(size_t capacity) {
    if (capacity <= plan_capacity_) return true;
    if (capacity == 0 || capacity == SIZE_MAX ||
        capacity > SIZE_MAX / sizeof(ReaderGroup) ||
        capacity > SIZE_MAX / sizeof(size_t)) {
        return false;
    }

    ReaderGroup *reader_groups = static_cast<ReaderGroup *>(
        Memory::calloc_large(capacity, sizeof(ReaderGroup), false));
    size_t *chunk_group_ids = static_cast<size_t *>(Memory::calloc_large(
        capacity, sizeof(size_t), false));
    size_t *group_counts = static_cast<size_t *>(Memory::calloc_large(
        capacity, sizeof(size_t), false));
    size_t *group_offsets = static_cast<size_t *>(Memory::calloc_large(
        capacity + 1, sizeof(size_t), false));
    size_t *group_write_offsets = static_cast<size_t *>(Memory::calloc_large(
        capacity, sizeof(size_t), false));
    size_t *group_chunk_indices = static_cast<size_t *>(Memory::calloc_large(
        capacity, sizeof(size_t), false));
    if (!reader_groups || !chunk_group_ids || !group_counts ||
        !group_offsets || !group_write_offsets || !group_chunk_indices) {
        if (reader_groups) Memory::free(reader_groups);
        if (chunk_group_ids) Memory::free(chunk_group_ids);
        if (group_counts) Memory::free(group_counts);
        if (group_offsets) Memory::free(group_offsets);
        if (group_write_offsets) Memory::free(group_write_offsets);
        if (group_chunk_indices) Memory::free(group_chunk_indices);
        return false;
    }

    if (reader_groups_) Memory::free(reader_groups_);
    if (chunk_group_ids_) Memory::free(chunk_group_ids_);
    if (group_counts_) Memory::free(group_counts_);
    if (group_offsets_) Memory::free(group_offsets_);
    if (group_write_offsets_) Memory::free(group_write_offsets_);
    if (group_chunk_indices_) Memory::free(group_chunk_indices_);

    reader_groups_ = reader_groups;
    chunk_group_ids_ = chunk_group_ids;
    group_counts_ = group_counts;
    group_offsets_ = group_offsets;
    group_write_offsets_ = group_write_offsets;
    group_chunk_indices_ = group_chunk_indices;
    plan_capacity_ = capacity;
    return true;
}

bool ReportEdfPlotBatch::prepare_plan() {
    if (!input_.chunks || input_.chunk_count == 0 ||
        !ensure_plan_capacity(input_.chunk_count)) {
        return false;
    }

    for (size_t i = 0; i < input_.chunk_count; ++i) {
        chunk_group_ids_[i] = NO_READER_GROUP;
        group_counts_[i] = 0;
    }
    reader_group_count_ = 0;

    for (size_t chunk_index = 0;
         chunk_index < input_.chunk_count;
         ++chunk_index) {
        const ReportResultChunk &chunk = input_.chunks[chunk_index];
        if (chunk.kind != ReportStoreChunkKind::Series ||
            chunk.provider_ref.provider != ReportProviderId::Edf ||
            chunk.end_ms <= input_.window_start_ms ||
            chunk.start_ms >= input_.window_end_ms) {
            continue;
        }

        EdfReportProviderReaderGroupKey key;
        const bool sample_reader =
            edf_report_signal_uses_edge_zero_padding(chunk.signal);
        if (!edf_report_provider_reader_group_key(chunk.provider_ref,
                                                  sample_reader,
                                                  key)) {
            return false;
        }

        size_t group_index = 0;
        for (; group_index < reader_group_count_; ++group_index) {
            if (edf_report_provider_same_reader_group(
                    reader_groups_[group_index].key, key)) {
                break;
            }
        }
        if (group_index == reader_group_count_) {
            reader_groups_[group_index].key = key;
            ++reader_group_count_;
        }

        chunk_group_ids_[chunk_index] = group_index;
        ++group_counts_[group_index];
    }
    if (reader_group_count_ == 0) return false;

    group_offsets_[0] = 0;
    for (size_t group_index = 0;
         group_index < reader_group_count_;
         ++group_index) {
        if (group_counts_[group_index] >
            input_.chunk_count - group_offsets_[group_index]) {
            return false;
        }

        group_offsets_[group_index + 1] =
            group_offsets_[group_index] + group_counts_[group_index];
        group_write_offsets_[group_index] = group_offsets_[group_index];
    }

    for (size_t chunk_index = 0;
         chunk_index < input_.chunk_count;
         ++chunk_index) {
        const size_t group_index = chunk_group_ids_[chunk_index];
        if (group_index == NO_READER_GROUP) continue;
        if (group_index >= reader_group_count_ ||
            group_write_offsets_[group_index] >=
                group_offsets_[group_index + 1]) {
            return false;
        }

        group_chunk_indices_[group_write_offsets_[group_index]++] =
            chunk_index;
    }

    plan_ready_ = true;
    return true;
}

bool ReportEdfPlotBatch::add_chunk_candidates(size_t chunk_index) {
    if (chunk_index >= input_.chunk_count) return false;

    const ReportResultChunk &chunk = input_.chunks[chunk_index];
    for (size_t stream_index = 0;
         stream_index < input_.stream_count && stream_index < 32;
         ++stream_index) {
        if (!report_result_chunk_has_stream(chunk, stream_index)) continue;
        if (candidate_count_ >= candidate_capacity_ || stream_index > UINT8_MAX) {
            return false;
        }

        ReportProviderChunk &candidate = candidates_[candidate_count_];
        if (!report_provider_chunk_from_result_stream(
                chunk,
                stream_index,
                input_.streams,
                input_.stream_count,
                input_.edf_sessions,
                input_.edf_session_count,
                candidate)) {
            return false;
        }

        ReportResultChunk &logical = logical_chunks_[candidate_count_];
        logical = chunk;
        logical.provider_ref = candidate.ref;
        logical.source = candidate.source;
        logical.signal = candidate.signal;
        logical.name = candidate.name;
        logical.stream_index = static_cast<uint8_t>(stream_index);
        logical.stream_mask = 0;
        logical.start_ms = candidate.start_ms;
        logical.end_ms = candidate.end_ms;
        logical.record_count = candidate.record_count;
        logical.payload_len = candidate.payload_len;
        logical.payload_schema = candidate.payload_schema;

        const uint32_t interval_ms = infer_chunk_interval_ms(
            logical.record_count, logical.start_ms, logical.end_ms);
        EdfReportSeriesPlotConfig &config = plot_configs_[candidate_count_];
        config.ranges = ranges_;
        config.range_count = input_.range_count;
        config.plot_start_ms = input_.plot_start_ms;
        config.bucket_ms = static_cast<uint32_t>(std::min<int64_t>(
            UINT32_MAX,
            plot_bucket_ms_for_signal(logical.signal,
                                      logical.source,
                                      input_.base_bucket_ms,
                                      interval_ms,
                                      input_.range_plot)));
        config.gap_threshold_ms = static_cast<uint32_t>(std::min<int64_t>(
            UINT32_MAX, plot_gap_threshold_ms(interval_ms)));
        config.value_multiplier =
            plot_value_multiplier(logical.signal, logical.source);

        chunk_indices_[candidate_count_] = chunk_index;
        stream_indices_[candidate_count_] =
            static_cast<uint8_t>(stream_index);
        ++candidate_count_;
    }

    return true;
}

bool ReportEdfPlotBatch::collect_candidates(size_t seed_chunk_index) {
    if (!input_.chunks || !input_.streams || !input_.edf_sessions ||
        !input_.ranges || !input_.chunk_done ||
        seed_chunk_index >= input_.chunk_count ||
        input_.stream_count == 0 || input_.edf_session_count == 0 ||
        input_.range_count == 0) {
        return false;
    }

    const ReportResultChunk &seed = input_.chunks[seed_chunk_index];
    if (seed.kind != ReportStoreChunkKind::Series ||
        seed.provider_ref.provider != ReportProviderId::Edf ||
        input_.chunk_done[seed_chunk_index] || !plan_ready_) {
        return false;
    }

    const size_t group_index = chunk_group_ids_[seed_chunk_index];
    if (group_index == NO_READER_GROUP ||
        group_index >= reader_group_count_) {
        return false;
    }

    const size_t stream_count = std::min(input_.stream_count,
                                         static_cast<size_t>(32));
    size_t candidate_count = 0;
    const size_t group_begin = group_offsets_[group_index];
    const size_t group_end = group_offsets_[group_index + 1];
    for (size_t member = group_begin; member < group_end; ++member) {
        const size_t chunk_index = group_chunk_indices_[member];
        if (chunk_index >= input_.chunk_count) return false;
        if (input_.chunk_done[chunk_index]) continue;

        const ReportResultChunk &chunk = input_.chunks[chunk_index];
        const size_t logical_count = stream_count_in_chunk(
            chunk.stream_mask, chunk.stream_index, stream_count);
        if (logical_count > SIZE_MAX - candidate_count) return false;
        candidate_count += logical_count;
    }
    if (candidate_count == 0 ||
        !ensure_candidate_capacity(candidate_count) ||
        !ensure_chunk_capacity(input_.chunk_count) ||
        !ensure_range_capacity(input_.range_count)) {
        return false;
    }

    memset(physical_counted_, 0, input_.chunk_count * sizeof(bool));
    for (size_t i = 0; i < input_.range_count; ++i) {
        ranges_[i].start_ms = input_.ranges[i].start_ms;
        ranges_[i].end_ms = input_.ranges[i].end_ms;
    }

    candidate_count_ = 0;
    for (size_t member = group_begin; member < group_end; ++member) {
        const size_t chunk_index = group_chunk_indices_[member];
        if (chunk_index >= input_.chunk_count) return false;
        if (input_.chunk_done[chunk_index]) continue;
        if (!add_chunk_candidates(chunk_index)) return false;
    }

    return candidate_count_ == candidate_count;
}

bool ReportEdfPlotBatch::start_reader(bool samples) {
    using_samples_ = samples;
    if (samples) {
        return reader_.start_samples(candidates_,
                                     candidate_count_,
                                     input_.edf_sessions,
                                     input_.edf_session_count,
                                     emit_sample,
                                     this);
    }

    return reader_.start_plot(candidates_,
                              candidate_count_,
                              plot_configs_,
                              input_.edf_sessions,
                              input_.edf_session_count,
                              emit_plot_point,
                              this);
}

bool ReportEdfPlotBatch::start(size_t seed_chunk_index,
                               const ReportEdfPlotBatchInput &input,
                               const ReportEdfPlotBatchSink &sink) {
    reader_.reset();
    if (plan_ready_ &&
        (input_.chunks != input.chunks ||
         input_.chunk_count != input.chunk_count ||
         input_.window_start_ms != input.window_start_ms ||
         input_.window_end_ms != input.window_end_ms)) {
        plan_ready_ = false;
        reader_group_count_ = 0;
    }

    input_ = input;
    sink_ = sink;
    seed_chunk_index_ = seed_chunk_index;
    candidate_count_ = 0;
    points_emitted_ = 0;
    using_samples_ = false;
    sample_fallback_attempted_ = false;
    active_ = false;
    error_ = nullptr;
    if (!sink_.open_series || !sink_.append_point || !sink_.append_sample ||
        input_.window_end_ms <= input_.window_start_ms ||
        (!plan_ready_ && !prepare_plan()) ||
        !collect_candidates(seed_chunk_index)) {
        fail("edf_batch_invalid");
        return false;
    }

    const bool edge_padding = edf_report_signal_uses_edge_zero_padding(
        input_.chunks[seed_chunk_index].signal);
    if (!start_reader(edge_padding)) {
        if (edge_padding || !start_reader(true)) {
            fail("edf_batch_open_failed");
            return false;
        }
        sample_fallback_attempted_ = true;
    }

    active_ = true;
    return true;
}

bool ReportEdfPlotBatch::emit_plot_point(
    void *context,
    size_t candidate_index,
    const EdfReportSeriesPlotPoint &point) {
    ReportEdfPlotBatch *batch = static_cast<ReportEdfPlotBatch *>(context);
    if (!batch || candidate_index >= batch->candidate_count_) return false;

    const size_t stream_index = batch->stream_indices_[candidate_index];
    if (!batch->sink_.open_series(batch->sink_.context, stream_index)) {
        return false;
    }

    batch->points_emitted_++;
    return batch->sink_.append_point(batch->sink_.context,
                                     stream_index,
                                     point,
                                     batch->plot_configs_[candidate_index]);
}

bool ReportEdfPlotBatch::emit_sample(void *context,
                                     size_t candidate_index,
                                     const ReportSeriesSample &sample) {
    ReportEdfPlotBatch *batch = static_cast<ReportEdfPlotBatch *>(context);
    if (!batch || candidate_index >= batch->candidate_count_) return false;

    const size_t stream_index = batch->stream_indices_[candidate_index];
    if (!batch->sink_.open_series(batch->sink_.context, stream_index)) {
        return false;
    }

    return batch->sink_.append_sample(batch->sink_.context,
                                      stream_index,
                                      batch->logical_chunks_[candidate_index],
                                      sample);
}

bool ReportEdfPlotBatch::finish() {
    if (!input_.chunk_done || !physical_counted_) return false;

    for (size_t i = 0; i < candidate_count_; ++i) {
        const size_t chunk_index = chunk_indices_[i];
        if (chunk_index >= input_.chunk_count) return false;

        input_.chunk_done[chunk_index] = true;
        if (physical_counted_[chunk_index]) continue;

        physical_counted_[chunk_index] = true;
        if (input_.input_chunks) (*input_.input_chunks)++;
        if (input_.input_bytes) {
            *input_.input_bytes += input_.chunks[chunk_index].payload_len;
        }
    }

    active_ = false;
    return true;
}

ReportEdfPlotBatchResult ReportEdfPlotBatch::poll(uint32_t budget_ms) {
    if (!active_) return ReportEdfPlotBatchResult::Failed;

    const EdfReportBatchPollResult result = reader_.poll(budget_ms);
    if (result == EdfReportBatchPollResult::Pending) {
        return ReportEdfPlotBatchResult::Pending;
    }
    if (result == EdfReportBatchPollResult::Complete) {
        if (!finish()) {
            fail("edf_batch_finish_failed");
            return ReportEdfPlotBatchResult::Failed;
        }
        return ReportEdfPlotBatchResult::Complete;
    }

    if (!using_samples_ && points_emitted_ == 0 &&
        !sample_fallback_attempted_) {
        sample_fallback_attempted_ = true;
        if (start_reader(true)) return ReportEdfPlotBatchResult::Pending;
    }

    fail("edf_batch_decode_failed");
    return ReportEdfPlotBatchResult::Failed;
}

void ReportEdfPlotBatch::fail(const char *error) {
    reader_.reset();
    active_ = false;
    error_ = error;
}

void ReportEdfPlotBatch::reset() {
    reader_.reset();
    input_ = {};
    sink_ = {};
    candidate_count_ = 0;
    seed_chunk_index_ = 0;
    points_emitted_ = 0;
    using_samples_ = false;
    sample_fallback_attempted_ = false;
    active_ = false;
    error_ = nullptr;
    reader_group_count_ = 0;
    plan_ready_ = false;
}

void ReportEdfPlotBatch::release_workspace() {
    reset();
    if (group_chunk_indices_) Memory::free(group_chunk_indices_);
    if (group_write_offsets_) Memory::free(group_write_offsets_);
    if (group_offsets_) Memory::free(group_offsets_);
    if (group_counts_) Memory::free(group_counts_);
    if (chunk_group_ids_) Memory::free(chunk_group_ids_);
    if (reader_groups_) Memory::free(reader_groups_);
    if (ranges_) Memory::free(ranges_);
    if (physical_counted_) Memory::free(physical_counted_);
    if (plot_configs_) Memory::free(plot_configs_);
    if (stream_indices_) Memory::free(stream_indices_);
    if (chunk_indices_) Memory::free(chunk_indices_);
    if (logical_chunks_) Memory::free(logical_chunks_);
    if (candidates_) Memory::free(candidates_);

    group_chunk_indices_ = nullptr;
    group_write_offsets_ = nullptr;
    group_offsets_ = nullptr;
    group_counts_ = nullptr;
    chunk_group_ids_ = nullptr;
    reader_groups_ = nullptr;
    ranges_ = nullptr;
    physical_counted_ = nullptr;
    plot_configs_ = nullptr;
    stream_indices_ = nullptr;
    chunk_indices_ = nullptr;
    logical_chunks_ = nullptr;
    candidates_ = nullptr;
    range_capacity_ = 0;
    physical_capacity_ = 0;
    candidate_capacity_ = 0;
    plan_capacity_ = 0;
}

}  // namespace aircannect
