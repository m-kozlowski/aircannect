#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_series.h"
#include "edf_signal_router.h"
#include "stream_frame.h"

namespace aircannect {

struct EdfSeriesAssemblyStatus {
    bool allocated = false;
    uint32_t current_record = 0;
    uint32_t records_completed = 0;
    uint32_t records_skipped = 0;
    uint32_t samples_accepted = 0;
    uint32_t samples_invalid = 0;
    uint32_t samples_missing = 0;
    uint32_t samples_duplicate = 0;
    uint32_t samples_late = 0;
    uint32_t records_dropped_partial = 0;
    uint16_t slots_filled = 0;
};

struct EdfStreamAssemblerStatus {
    bool buffers_ready = false;
    bool active = false;
    uint32_t frames = 0;
    uint32_t timestamp_errors = 0;
    uint32_t unknown_signals = 0;
    uint32_t samples_accepted = 0;
    uint32_t samples_invalid = 0;
    uint32_t samples_missing = 0;
    uint32_t samples_duplicate = 0;
    uint32_t samples_late = 0;
    uint32_t records_dropped_partial = 0;
    uint32_t records_completed = 0;
    uint32_t timestamp_jitter_corrections = 0;
    uint32_t timestamp_resyncs = 0;
    int32_t last_timestamp_jitter_ms = 0;
    int64_t session_start_epoch_ms = 0;
    int64_t last_sample_epoch_ms = 0;
    EdfSeriesAssemblyStatus brp;
    EdfSeriesAssemblyStatus pld;
    EdfSeriesAssemblyStatus sa2;
    char last_error[80] = {};
};

struct EdfCompletedRecordView {
    EdfSeriesId series = EdfSeriesId::Brp;
    uint32_t record_index = 0;
    size_t signal_count = 0;
    size_t samples_per_record = 0;
    const float *values = nullptr;
    const uint8_t *present = nullptr;
    const uint8_t *valid = nullptr;
};

using EdfRecordObserver = void (*)(void *context,
                                   const EdfCompletedRecordView &record);

enum class EdfFramePrepareStatus : uint8_t {
    Ready,
    Deferred,
    Rejected,
};

class EdfStreamAssembler {
public:
    bool begin();
    void reset();
    void release();

    void set_record_observer(EdfRecordObserver observer, void *context);
    bool start_session(const char *device_start_time);
    void set_current_records(uint32_t brp_record,
                             uint32_t pld_record,
                             uint32_t sa2_record);
    EdfFramePrepareStatus prepare_frame(const StreamFrameData &frame,
                                        size_t max_records_to_publish);
    void end_session();
    void ingest_frame(const StreamFrameData &frame);

    const EdfStreamAssemblerStatus &status() const { return status_; }

private:
    struct SeriesBuffer {
        EdfSeriesId id = EdfSeriesId::Brp;
        size_t signal_count = 0;
        size_t samples_per_record = 0;
        uint32_t sample_ms = 0;
        float *values = nullptr;
        uint8_t *present = nullptr;
        uint8_t *valid = nullptr;
        EdfSeriesAssemblyStatus *status = nullptr;
    };

    struct FrameTiming {
        int64_t reported_start_ms = 0;
        int64_t effective_start_ms = 0;
        uint32_t coverage_ms = 0;
        uint32_t tolerance_ms = 0;
        int32_t jitter_ms = 0;
        bool eligible = false;
        bool corrected = false;
        bool resync = false;
    };

    bool allocate_buffers();
    void free_buffers();
    void reset_session_counters();
    void reset_timeline();
    void reset_record(SeriesBuffer &series);
    bool record_has_samples(const SeriesBuffer &series) const;
    bool last_present_sample(const SeriesBuffer &series,
                             uint8_t signal_index,
                             uint16_t &sample_index) const;
    bool record_tail_complete(const SeriesBuffer &series) const;
    uint32_t count_missing_record_samples(const SeriesBuffer &series) const;
    void count_late_frame_samples(const StreamFrameData &frame,
                                  int64_t frame_start_ms,
                                  SeriesBuffer &series);
    void ingest_series_frame(const StreamFrameData &frame,
                             int64_t frame_start_ms,
                             SeriesBuffer &series);
    bool resolve_frame_timing(const StreamFrameData &frame,
                              int64_t reported_start_ms,
                              FrameTiming &timing) const;
    void commit_frame_timing(const StreamFrameData &frame,
                             const FrameTiming &timing);
    void publish_record(const SeriesBuffer &series);
    void publish_current_record(SeriesBuffer &series, bool skipped);
    bool advance_to_record(SeriesBuffer &series,
                           uint32_t new_record,
                           size_t *publish_budget = nullptr);
    void flush_partial_records();
    void store_sample(SeriesBuffer &series,
                      uint8_t signal_index,
                      uint32_t record_index,
                      uint16_t sample_index,
                      bool valid,
                      float value,
                      bool count_duplicate);
    SeriesBuffer series(EdfSeriesId id);
    bool parse_frame_start_ms(const StreamFrameData &frame, int64_t &start_ms);
    bool ensure_session_epoch(int64_t frame_start_ms);
    void set_error(const char *error);

    float *brp_values_ = nullptr;
    float *pld_values_ = nullptr;
    float *sa2_values_ = nullptr;
    uint8_t *brp_present_ = nullptr;
    uint8_t *pld_present_ = nullptr;
    uint8_t *sa2_present_ = nullptr;
    uint8_t *brp_valid_ = nullptr;
    uint8_t *pld_valid_ = nullptr;
    uint8_t *sa2_valid_ = nullptr;
    EdfRecordObserver record_observer_ = nullptr;
    void *record_observer_context_ = nullptr;
    bool timeline_active_ = false;
    uint32_t timeline_stream_id_ = 0;
    int64_t timeline_next_frame_start_ms_ = 0;
    EdfStreamAssemblerStatus status_;
};

}  // namespace aircannect
