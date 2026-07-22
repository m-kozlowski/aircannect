#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "report_fallback_artifact.h"
#include "report_parser.h"
#include "report_read_plan.h"
#include "report_spool_port.h"
#include "report_spool_types.h"
#include "storage_atomic_write_port.h"
#include "storage_read_port.h"

namespace aircannect {

enum class ReportFallbackAcquisitionState : uint8_t {
    Idle,
    Preserving,
    Fetching,
    Publishing,
    Cancelling,
    Ready,
    Failed,
    Cancelled,
};

struct ReportFallbackAcquisitionStatus {
    ReportFallbackAcquisitionState state =
        ReportFallbackAcquisitionState::Idle;
    SleepDayId sleep_day;
    ReportSourceId source = ReportSourceId::Summary;
    uint32_t sources_total = 0;
    uint32_t sources_completed = 0;
    uint32_t sections_preserved = 0;
    uint32_t sections_added = 0;
    uint32_t unavailable_added = 0;
    uint64_t replacement_identity = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};

    bool active() const {
        return state == ReportFallbackAcquisitionState::Preserving ||
               state == ReportFallbackAcquisitionState::Fetching ||
               state == ReportFallbackAcquisitionState::Publishing ||
               state == ReportFallbackAcquisitionState::Cancelling;
    }
    bool terminal() const {
        return state == ReportFallbackAcquisitionState::Ready ||
               state == ReportFallbackAcquisitionState::Failed ||
               state == ReportFallbackAcquisitionState::Cancelled;
    }
};

class ReportFallbackAcquisitionService {
public:
    ReportFallbackAcquisitionService() = default;
    ~ReportFallbackAcquisitionService();

    ReportFallbackAcquisitionService(
        const ReportFallbackAcquisitionService &) = delete;
    ReportFallbackAcquisitionService &operator=(
        const ReportFallbackAcquisitionService &) = delete;

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               ReportSpoolPort &spool_port);

    OperationAdmission start(
        std::shared_ptr<const ReportReadPlan> plan,
        uint32_t generation,
        StorageReadLane read_lane,
        StorageAtomicWriteLane write_lane);
    bool poll();
    void cancel();
    void reset();

    const ReportFallbackAcquisitionStatus &status() const {
        return status_;
    }
    std::shared_ptr<const LargeByteBuffer> replacement() const {
        return status_.state == ReportFallbackAcquisitionState::Ready
            ? replacement_
            : nullptr;
    }

private:
    static constexpr size_t MaxSourceTargets = 8;

    struct SourceTarget {
        ReportSourceId source = ReportSourceId::Summary;
        int64_t from_ms = 0;
    };

    struct SeriesCoverage {
        ReportSourceId source = ReportSourceId::Summary;
        ReportSignalId signal = ReportSignalId::Count;
        NightCatalogTimeRange range;
    };

    static bool accept_parsed_chunk(
        void *context,
        const ReportParsedChunk &chunk);

    bool prepare();
    bool build_targets();

    bool poll_preservation();
    bool select_preserved_section();
    bool finish_preserved_read();
    bool should_preserve(const NightCatalogFallbackSection &section) const;

    bool poll_fetch();
    bool submit_current_fetch();
    bool consume_fetch_round();
    bool finish_current_fetch();
    bool current_sampled_target_complete() const;
    bool complete_current_source();
    bool parse_round(ReportSpoolResult &result);
    bool accept_series_chunk(const ReportParsedChunk &chunk);
    bool accept_event_chunk(const ReportParsedChunk &chunk);
    bool append_series_run(const ReportParsedChunk &chunk,
                           const ReportSeriesV2UniformView &view,
                           size_t session_index,
                           uint32_t first_sample,
                           uint32_t sample_count);
    bool append_event_sections();
    bool append_unavailable_sections();

    bool poll_publication();
    bool submit_publication();
    bool finish_publication();
    bool poll_cancellation();

    const SourceTarget *source_target(ReportSourceId source) const;
    bool signal_targeted(size_t session_index,
                         ReportSignalId signal,
                         ReportSourceId source) const;
    bool event_session_targeted(size_t session_index) const;
    void series_coverage_after(ReportSourceId source,
                               ReportSignalId signal,
                               int64_t timestamp_ms,
                               bool &covered,
                               int64_t &covered_end_ms,
                               int64_t &next_start_ms) const;
    bool series_session_complete(size_t session_index,
                                 ReportSourceId source,
                                 ReportSignalId signal) const;
    bool append_series_coverage(ReportSourceId source,
                                ReportSignalId signal,
                                const NightCatalogTimeRange &range);
    size_t event_session_for(const ReportEventRecord &event) const;
    bool append_unique_event(const ReportEventRecord &event);

    void abandon_operations();
    void begin_terminal(ReportFallbackAcquisitionState state,
                        const char *error);
    void fail(const char *error);
    void finish(ReportFallbackAcquisitionState state);

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    ReportSpoolPort *spool_port_ = nullptr;
    std::shared_ptr<const ReportReadPlan> plan_;
    std::shared_ptr<const LargeByteBuffer> replacement_;
    ReportFallbackArtifactBuilder builder_;
    ReportSpoolBuffer event_records_;

    SourceTarget targets_[MaxSourceTargets] = {};
    size_t target_count_ = 0;
    size_t target_index_ = 0;
    uint32_t missing_signal_masks_[
        ReportFallbackArtifactCodec::MaxSessions] = {};
    bool rebuild_events_[ReportFallbackArtifactCodec::MaxSessions] = {};
    SeriesCoverage added_series_[
        ReportFallbackArtifactCodec::MaxSections] = {};
    size_t added_series_count_ = 0;

    size_t session_count_ = 0;
    size_t preserve_file_index_ = 0;
    size_t preserve_section_index_ = 0;
    const NightCatalogFallbackFile *preserve_file_ = nullptr;
    const NightCatalogFallbackSection *preserve_section_ = nullptr;
    uint8_t *preserve_payload_ = nullptr;
    StoragePreparedRead preserve_prepared_;

    OperationTicket read_ticket_;
    OperationTicket spool_ticket_;
    OperationTicket write_ticket_;
    bool source_stop_requested_ = false;
    uint32_t generation_ = 0;
    StorageReadLane read_lane_ = StorageReadLane::Report;
    StorageAtomicWriteLane write_lane_ =
        StorageAtomicWriteLane::Maintenance;
    ReportFallbackAcquisitionState terminal_state_ =
        ReportFallbackAcquisitionState::Idle;
    ReportFallbackAcquisitionStatus status_;
};

}  // namespace aircannect
