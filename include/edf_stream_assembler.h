#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_clock.h"
#include "edf_series.h"
#include "edf_signal_router.h"
#include "stream_frame.h"

namespace aircannect {

struct EdfSeriesAssemblyStatus {
    uint32_t current_record = 0;
    uint32_t records_completed = 0;
    uint32_t missing_slots = 0;
    uint32_t samples_duplicate = 0;
    uint32_t samples_late = 0;

    uint16_t slots_filled = 0;
};

struct EdfStreamAssemblerStatus {
    bool buffers_ready = false;
    bool active = false;

    uint32_t frames = 0;
    uint32_t timestamp_errors = 0;

    uint32_t timestamp_resyncs = 0;

    int64_t session_start_epoch_ms = 0;

    EdfSeriesAssemblyStatus series[AC_EDF_NUMERIC_SERIES_COUNT];

    char unmapped_signal[AC_STREAM_FRAME_SIGNAL_NAME_MAX] = {};
    char last_error[80] = {};

    EdfSeriesAssemblyStatus &for_series(EdfSeriesId id) {
        return series[edf_series_index(id)];
    }
    const EdfSeriesAssemblyStatus &for_series(EdfSeriesId id) const {
        return series[edf_series_index(id)];
    }
};

using EdfRecordObserver = bool (*)(void *context,
                                   const EdfCompletedRecordView &record);

enum class EdfFramePrepareStatus : uint8_t {
    Ready,
    Deferred,
    Rejected,
};

enum class EdfSa2IngestStatus : uint8_t {
    Stored,
    Deferred,
    Rejected,
};

struct EdfSa2Sample {
    int64_t epoch_ms = 0;
    float pulse_bpm = 0.0f;
    float spo2 = 0.0f;
    bool valid = false;
};

class EdfStreamAssembler {
public:
    bool begin();
    void reset();
    void release();

    void set_record_observer(EdfRecordObserver observer, void *context);

    bool start_session(const char *device_start_time,
                       const As11ClockTransform &clock_transform);
    void set_current_records(
        const uint32_t (&records)[AC_EDF_NUMERIC_SERIES_COUNT]);
    bool end_session();

    EdfFramePrepareStatus prepare_frame(const StreamFrameData &frame,
                                        size_t max_records_to_publish);
    void ingest_frame(const StreamFrameData &frame);
    EdfSa2IngestStatus ingest_sa2_sample(
        const EdfSa2Sample &sample,
        size_t max_records_to_publish);

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

    struct SeriesStorage {
        float *values = nullptr;
        uint8_t *present = nullptr;
        uint8_t *valid = nullptr;
    };

    struct FrameTiming {
        int64_t reported_start_ms = 0;
        int64_t effective_start_ms = 0;
        uint32_t coverage_ms = 0;
        uint32_t tolerance_ms = 0;
        bool eligible = false;
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
    void maybe_rebase_initial_epoch(const StreamFrameData &frame,
                                    int64_t frame_start_ms);

    bool publish_record(const SeriesBuffer &series);
    bool publish_current_record(SeriesBuffer &series);
    bool advance_to_record(SeriesBuffer &series,
                           uint32_t new_record,
                           size_t *publish_budget = nullptr);
    bool advance_sparse_to_record(SeriesBuffer &series,
                                  uint32_t new_record,
                                  size_t publish_budget);
    bool flush_partial_records();

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
    bool initial_epoch_can_rebase() const;
    void set_error(const char *error);

    SeriesStorage series_storage_[AC_EDF_NUMERIC_SERIES_COUNT];

    EdfRecordObserver record_observer_ = nullptr;
    void *record_observer_context_ = nullptr;

    bool timeline_active_ = false;
    uint32_t timeline_stream_id_ = 0;
    int64_t timeline_next_frame_start_ms_ = 0;
    int64_t declared_start_epoch_ms_ = 0;
    bool initial_epoch_rebase_allowed_ = false;
    As11ClockTransform clock_transform_;

    EdfStreamAssemblerStatus status_;
};

}  // namespace aircannect
