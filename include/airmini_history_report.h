#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <vector>

#include "airmini_history_data.h"
#include "large_allocator.h"
#include "large_byte_buffer.h"
#include "report_fallback_artifact.h"
#include "report_records.h"

namespace aircannect {

enum class AirMiniHistoryReportError : uint8_t {
    None,
    InvalidInput,
    InvalidSample,
    InvalidSampleAlignment,
    InvalidEvent,
    TooManySections,
    PayloadBuildFailed,
    ArtifactBuildFailed,
    EmptyArtifact,
};

const char *airmini_history_report_error_name(
    AirMiniHistoryReportError error);

class AirMiniHistoryReportBuilder {
public:
    static constexpr size_t DefaultPollBudget = 128;

    AirMiniHistoryReportBuilder() = default;

    // The projection remains owned by the caller until completion.
    bool start(const AirMiniHistoryProjectionInput &input,
               const AirMiniHistoryStrProjection &projection,
               uint64_t identity);
    // Returns true while the operation remains usable; inspect ready/failed.
    bool poll(size_t budget = DefaultPollBudget);

    bool ready() const { return phase_ == Phase::Complete; }
    bool failed() const { return phase_ == Phase::Failed; }
    bool active() const {
        return phase_ != Phase::Idle && !ready() && !failed();
    }
    AirMiniHistoryReportError error() const { return error_; }
    // EmptyArtifact is a successful empty result when no partial rows exist.
    const std::shared_ptr<const LargeByteBuffer> &result() const {
        return result_;
    }

private:
    using SessionList = std::vector<NightCatalogTimeRange,
                                    LargeAllocator<NightCatalogTimeRange>>;

    enum class Phase : uint8_t {
        Idle,
        InspiratoryPressure,
        Leak,
        Events,
        Finish,
        Complete,
        Failed,
    };

    const AirMiniHistorySamples &series_samples() const;
    ReportSignalId series_signal() const;
    double series_scale() const;

    bool append_series_slot(int32_t value_milli);
    bool finish_series_run();
    bool finish_series();
    bool sample_in_session(int64_t timestamp_ms);
    void reset_series_run();
    void reset_series();
    void poll_series(size_t &budget);

    bool event_may_reach_session_end(
        const AirMiniHistoryEvent &event,
        const NightCatalogTimeRange &session) const;
    bool event_boundary(const AirMiniHistoryEvent &event,
                        int64_t &timestamp_ms) const;
    bool make_event_record(const AirMiniHistoryEvent &event,
                           ReportEventRecord &record) const;
    bool append_clipped_event(const ReportEventRecord &record,
                              const NightCatalogTimeRange &session);
    bool finish_event_session(const NightCatalogTimeRange &session);
    void reset_event_session();
    void poll_events(size_t &budget);

    bool convert_sample(const AirMiniHistorySample &sample,
                        int32_t &value_milli) const;
    void fail(AirMiniHistoryReportError error);
    void reset();

    const AirMiniHistoryStrProjection *projection_ = nullptr;

    Phase phase_ = Phase::Idle;
    AirMiniHistoryReportError error_ = AirMiniHistoryReportError::None;
    std::shared_ptr<const LargeByteBuffer> result_;

    SessionList sessions_;
    ReportFallbackArtifactBuilder artifact_builder_;

    size_t sample_cursor_ = 0;
    size_t sample_session_index_ = 0;
    size_t series_session_index_ = 0;
    bool series_started_ = false;
    uint32_t series_sample_count_ = 0;
    int64_t series_start_ms_ = 0;
    int64_t series_last_sample_ms_ = 0;
    int64_t series_coverage_end_ms_ = 0;
    ReportSpoolBuffer series_values_;
    ReportSpoolBuffer series_payload_;

    size_t event_session_index_ = 0;
    size_t event_cursor_ = 0;
    bool csr_open_ = false;
    int64_t csr_start_ms_ = 0;
    uint8_t event_mask_ = 0;
    int64_t event_coverage_start_ms_ = 0;
    int64_t event_coverage_end_ms_ = 0;
    ReportSpoolBuffer event_payload_;

    size_t section_count_ = 0;
};

}  // namespace aircannect
