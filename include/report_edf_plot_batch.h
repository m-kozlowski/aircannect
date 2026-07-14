#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_provider_cursor.h"
#include "report_manager_internal_types.h"
#include "report_result_types.h"

namespace aircannect {

struct EdfReportPlotRange;
struct EdfReportSeriesPlotConfig;
struct EdfReportSessionDescriptor;
struct ReportProviderChunk;
struct ReportSeriesSample;

enum class ReportEdfPlotBatchResult : uint8_t {
    Pending,
    Complete,
    Failed,
};

struct ReportEdfPlotBatchInput {
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    const ReportResultChunk *chunks = nullptr;
    size_t chunk_count = 0;
    const ReportResultStream *streams = nullptr;
    size_t stream_count = 0;
    const EdfReportSessionDescriptor *edf_sessions = nullptr;
    size_t edf_session_count = 0;
    const ReportSessionRange *ranges = nullptr;
    size_t range_count = 0;

    bool *chunk_done = nullptr;
    uint32_t *input_chunks = nullptr;
    uint32_t *input_bytes = nullptr;

    int64_t window_start_ms = 0;
    int64_t window_end_ms = 0;
    int64_t plot_start_ms = 0;
    int64_t base_bucket_ms = 1;
    bool range_plot = false;
};

struct ReportEdfPlotBatchSink {
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    void *context = nullptr;
    bool (*open_series)(void *context, size_t stream_index) = nullptr;
    bool (*append_point)(void *context,
                         size_t stream_index,
                         const EdfReportSeriesPlotPoint &point,
                         const EdfReportSeriesPlotConfig &config) = nullptr;
    bool (*append_sample)(void *context,
                          size_t stream_index,
                          const ReportResultChunk &chunk,
                          const ReportSeriesSample &sample) = nullptr;
};

class ReportEdfPlotBatch {
public:
    using ReportResultChunk = report_manager_internal::ReportResultChunk;

    ~ReportEdfPlotBatch();

    bool start(size_t seed_chunk_index,
               const ReportEdfPlotBatchInput &input,
               const ReportEdfPlotBatchSink &sink);
    ReportEdfPlotBatchResult poll(uint32_t budget_ms);
    void reset();

    bool active() const { return active_; }
    const char *error() const { return error_; }

private:
    struct ReaderGroup;

    bool ensure_candidate_capacity(size_t capacity);
    bool ensure_chunk_capacity(size_t capacity);
    bool ensure_range_capacity(size_t capacity);
    bool ensure_plan_capacity(size_t capacity);
    bool prepare_plan();
    bool collect_candidates(size_t seed_chunk_index);
    bool add_chunk_candidates(size_t chunk_index);
    bool start_reader(bool samples);
    bool finish();
    void fail(const char *error);
    void release_workspace();

    static bool emit_plot_point(
        void *context,
        size_t candidate_index,
        const EdfReportSeriesPlotPoint &point);
    static bool emit_sample(void *context,
                            size_t candidate_index,
                            const ReportSeriesSample &sample);

    ReportEdfPlotBatchInput input_;
    ReportEdfPlotBatchSink sink_;
    EdfReportProviderBatchCursor reader_;

    ReportProviderChunk *candidates_ = nullptr;
    ReportResultChunk *logical_chunks_ = nullptr;
    size_t *chunk_indices_ = nullptr;
    uint8_t *stream_indices_ = nullptr;
    EdfReportSeriesPlotConfig *plot_configs_ = nullptr;
    size_t candidate_capacity_ = 0;
    size_t candidate_count_ = 0;

    bool *physical_counted_ = nullptr;
    size_t physical_capacity_ = 0;
    EdfReportPlotRange *ranges_ = nullptr;
    size_t range_capacity_ = 0;

    ReaderGroup *reader_groups_ = nullptr;
    size_t *chunk_group_ids_ = nullptr;
    size_t *group_counts_ = nullptr;
    size_t *group_offsets_ = nullptr;
    size_t *group_write_offsets_ = nullptr;
    size_t *group_chunk_indices_ = nullptr;
    size_t plan_capacity_ = 0;
    size_t reader_group_count_ = 0;
    bool plan_ready_ = false;

    size_t seed_chunk_index_ = 0;
    uint32_t points_emitted_ = 0;
    bool using_samples_ = false;
    bool sample_fallback_attempted_ = false;
    bool active_ = false;
    const char *error_ = nullptr;
};

}  // namespace aircannect
