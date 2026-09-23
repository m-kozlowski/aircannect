#include "airmini_history_report.h"

#include <algorithm>
#include <cmath>
#include <new>

#include "little_endian.h"

namespace aircannect {
namespace {

constexpr int64_t MINUTE_MS = 60000;

bool add_duration_ms(int64_t timestamp_ms,
                     uint32_t seconds,
                     int64_t &start_ms,
                     int32_t &duration_ms) {
    if (seconds > static_cast<uint32_t>(INT32_MAX) / 1000u) return false;

    const int64_t duration = static_cast<int64_t>(seconds) * 1000;
    if (timestamp_ms < INT64_MIN + duration) return false;

    start_ms = timestamp_ms - duration;
    duration_ms = static_cast<int32_t>(duration);
    return true;
}

bool add_backdate_ms(int64_t timestamp_ms,
                     uint32_t seconds,
                     int64_t &boundary_ms) {
    const int64_t backdate = static_cast<int64_t>(seconds) * 1000;
    if (timestamp_ms < INT64_MIN + backdate) return false;

    boundary_ms = timestamp_ms - backdate;
    return true;
}

}  // namespace

const char *airmini_history_report_error_name(
    AirMiniHistoryReportError error) {
    switch (error) {
        case AirMiniHistoryReportError::None: return "none";
        case AirMiniHistoryReportError::InvalidInput: return "invalid_input";
        case AirMiniHistoryReportError::InvalidSample:
            return "invalid_sample";
        case AirMiniHistoryReportError::InvalidSampleAlignment:
            return "invalid_sample_alignment";
        case AirMiniHistoryReportError::InvalidEvent:
            return "invalid_event";
        case AirMiniHistoryReportError::TooManySections:
            return "too_many_sections";
        case AirMiniHistoryReportError::PayloadBuildFailed:
            return "payload_build_failed";
        case AirMiniHistoryReportError::ArtifactBuildFailed:
            return "artifact_build_failed";
        case AirMiniHistoryReportError::EmptyArtifact:
            return "empty_artifact";
    }
    return "unknown";
}

void AirMiniHistoryReportBuilder::reset() {
    projection_ = nullptr;
    phase_ = Phase::Idle;
    error_ = AirMiniHistoryReportError::None;
    result_.reset();
    sessions_.clear();
    artifact_builder_.reset();

    sample_cursor_ = 0;
    sample_session_index_ = 0;
    series_session_index_ = 0;
    reset_series_run();

    event_session_index_ = 0;
    event_cursor_ = 0;
    csr_open_ = false;
    csr_start_ms_ = 0;
    event_mask_ = 0;
    event_coverage_start_ms_ = 0;
    event_coverage_end_ms_ = 0;
    event_payload_.clear();

    section_count_ = 0;
}

void AirMiniHistoryReportBuilder::fail(AirMiniHistoryReportError error) {
    if (phase_ == Phase::Failed) return;
    error_ = error;
    phase_ = Phase::Failed;
    result_.reset();
    artifact_builder_.reset();
}

bool AirMiniHistoryReportBuilder::start(
    const AirMiniHistoryProjectionInput &input,
    const AirMiniHistoryStrProjection &projection,
    uint64_t identity) {
    reset();

    if (!input.day.valid() || identity == 0 || input.day_start_ms <= 0 ||
        input.day_end_ms <= input.day_start_ms || projection.sessions.empty()) {
        fail(AirMiniHistoryReportError::InvalidInput);
        return false;
    }
    try {
        sessions_.reserve(projection.sessions.size());
        for (const AirMiniHistorySession &session : projection.sessions) {
            sessions_.push_back({session.start_ms, session.end_ms});
        }
    } catch (const std::bad_alloc &) {
        fail(AirMiniHistoryReportError::InvalidInput);
        return false;
    }

    if (!artifact_builder_.begin(input.day,
                                 identity,
                                 input.day_start_ms,
                                 input.day_end_ms,
                                 sessions_.data(),
                                 sessions_.size(),
                                 true,
                                 input.timezone_offset_minutes,
                                 true)) {
        fail(AirMiniHistoryReportError::ArtifactBuildFailed);
        return false;
    }

    projection_ = &projection;
    phase_ = Phase::InspiratoryPressure;
    reset_series();
    return true;
}

const AirMiniHistorySamples &AirMiniHistoryReportBuilder::series_samples()
    const {
    return phase_ == Phase::Leak
        ? projection_->leak
        : projection_->inspiratory_pressure;
}

ReportSignalId AirMiniHistoryReportBuilder::series_signal() const {
    return phase_ == Phase::Leak
        ? ReportSignalId::Leak
        : ReportSignalId::InspiratoryPressure;
}

double AirMiniHistoryReportBuilder::series_scale() const {
    return phase_ == Phase::Leak ? 60000.0 : 1000.0;
}

void AirMiniHistoryReportBuilder::reset_series_run() {
    series_started_ = false;
    series_sample_count_ = 0;
    series_start_ms_ = 0;
    series_last_sample_ms_ = 0;
    series_coverage_end_ms_ = 0;
    series_values_.clear();
    series_payload_.clear();
}

void AirMiniHistoryReportBuilder::reset_series() {
    sample_cursor_ = 0;
    sample_session_index_ = 0;
    series_session_index_ = 0;
    reset_series_run();
}

bool AirMiniHistoryReportBuilder::sample_in_session(int64_t timestamp_ms) {
    while (sample_session_index_ < sessions_.size() &&
           timestamp_ms >= sessions_[sample_session_index_].end_ms) {
        ++sample_session_index_;
    }
    return sample_session_index_ < sessions_.size() &&
           timestamp_ms >= sessions_[sample_session_index_].start_ms &&
           timestamp_ms < sessions_[sample_session_index_].end_ms;
}

bool AirMiniHistoryReportBuilder::convert_sample(
    const AirMiniHistorySample &sample,
    int32_t &value_milli) const {
    const double value = static_cast<double>(sample.value) * series_scale();
    if (!std::isfinite(value) ||
        value < static_cast<double>(INT32_MIN) - 0.5 ||
        value > static_cast<double>(INT32_MAX) + 0.5) {
        return false;
    }
    const int64_t rounded = std::llround(value);
    if (rounded < INT32_MIN || rounded > INT32_MAX) return false;
    value_milli = static_cast<int32_t>(rounded);
    return true;
}

bool AirMiniHistoryReportBuilder::append_series_slot(int32_t value_milli) {
    if (series_sample_count_ == UINT32_MAX) return false;

    size_t offset = 0;
    uint8_t *value = series_values_.append_uninitialized(4, offset);
    if (!value) return false;
    LittleEndian::put_le32(value, static_cast<uint32_t>(value_milli));

    ++series_sample_count_;
    return true;
}

bool AirMiniHistoryReportBuilder::finish_series_run() {
    if (!series_started_ || series_sample_count_ == 0) return true;
    if (series_sample_count_ > INT64_MAX / MINUTE_MS ||
        series_start_ms_ > INT64_MAX -
            static_cast<int64_t>(series_sample_count_) * MINUTE_MS) {
        return false;
    }

    if (!report_build_series_payload_v2_uniform_values_le(
            series_payload_,
            static_cast<uint32_t>(MINUTE_MS),
            series_values_.data(),
            series_sample_count_)) {
        return false;
    }

    const int64_t unbounded_end = series_start_ms_ +
        static_cast<int64_t>(series_sample_count_) * MINUTE_MS;
    const int64_t end_ms = std::min(unbounded_end, series_coverage_end_ms_);
    ReportFallbackSectionInput section;
    section.kind = ReportFallbackSectionKind::Series;
    section.source = ReportSourceId::TherapyOneMinute;
    section.signal = series_signal();
    section.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    section.record_count = series_sample_count_;
    section.sample_interval_ms = static_cast<uint32_t>(MINUTE_MS);
    section.coverage = {series_start_ms_, end_ms};
    section.payload = series_payload_.data();
    section.payload_size = series_payload_.size();
    if (section_count_ >= ReportFallbackArtifactCodec::MaxSections) {
        return false;
    }
    if (!section.coverage.valid() || !artifact_builder_.append_section(section)) {
        return false;
    }
    ++section_count_;
    reset_series_run();
    return true;
}

bool AirMiniHistoryReportBuilder::finish_series() {
    if (!finish_series_run()) return false;

    if (phase_ == Phase::InspiratoryPressure) {
        phase_ = Phase::Leak;
        reset_series();
    } else {
        phase_ = Phase::Events;
        event_session_index_ = 0;
        event_cursor_ = 0;
        reset_event_session();
    }
    return true;
}

void AirMiniHistoryReportBuilder::poll_series(size_t &budget) {
    const AirMiniHistorySamples &samples = series_samples();
    while (budget > 0) {
        if (sample_cursor_ >= samples.size()) {
            if (!finish_series()) {
                fail(section_count_ >= ReportFallbackArtifactCodec::MaxSections
                         ? AirMiniHistoryReportError::TooManySections
                         : AirMiniHistoryReportError::PayloadBuildFailed);
            }
            return;
        }

        const AirMiniHistorySample &sample = samples[sample_cursor_++];
        --budget;
        if (!sample_in_session(sample.timestamp_ms)) continue;

        int32_t value_milli = 0;
        if (!convert_sample(sample, value_milli)) {
            fail(AirMiniHistoryReportError::InvalidSample);
            return;
        }
        if (series_started_ && sample.timestamp_ms <= series_last_sample_ms_) {
            fail(AirMiniHistoryReportError::InvalidSampleAlignment);
            return;
        }
        if (series_started_ &&
            (series_session_index_ != sample_session_index_ ||
             sample.timestamp_ms - series_last_sample_ms_ != MINUTE_MS)) {
            if (!finish_series_run()) {
                fail(section_count_ >= ReportFallbackArtifactCodec::MaxSections
                         ? AirMiniHistoryReportError::TooManySections
                         : AirMiniHistoryReportError::PayloadBuildFailed);
                return;
            }
        }

        if (!series_started_) {
            series_started_ = true;
            series_session_index_ = sample_session_index_;
            series_start_ms_ = sample.timestamp_ms;
        }
        series_last_sample_ms_ = sample.timestamp_ms;
        series_coverage_end_ms_ = sessions_[sample_session_index_].end_ms;
        if (!append_series_slot(value_milli)) {
            fail(AirMiniHistoryReportError::PayloadBuildFailed);
            return;
        }
    }
}

bool AirMiniHistoryReportBuilder::event_boundary(
    const AirMiniHistoryEvent &event,
    int64_t &timestamp_ms) const {
    if (event.kind == AirMiniHistoryEventKind::CsrStart ||
        event.kind == AirMiniHistoryEventKind::CsrEnd) {
        return add_backdate_ms(event.timestamp_ms,
                               event.has_backdate ? event.backdate_seconds : 0,
                               timestamp_ms);
    }
    timestamp_ms = event.timestamp_ms;
    return true;
}

bool AirMiniHistoryReportBuilder::event_may_reach_session_end(
    const AirMiniHistoryEvent &event,
    const NightCatalogTimeRange &session) const {
    int64_t start_ms = 0;
    if (event.kind == AirMiniHistoryEventKind::CsrEnd && csr_open_) {
        start_ms = csr_start_ms_;
    } else if (event.kind == AirMiniHistoryEventKind::CsrStart ||
               event.kind == AirMiniHistoryEventKind::CsrEnd) {
        if (!event_boundary(event, start_ms)) return false;
    } else if (event.has_duration) {
        int32_t duration_ms = 0;
        if (!add_duration_ms(event.timestamp_ms,
                             event.duration_seconds,
                             start_ms,
                             duration_ms)) {
            return false;
        }
    } else {
        start_ms = event.timestamp_ms;
    }
    return start_ms < session.end_ms;
}

bool AirMiniHistoryReportBuilder::make_event_record(
    const AirMiniHistoryEvent &event,
    ReportEventRecord &record) const {
    record = {};
    record.flags = 0;
    switch (event.kind) {
        case AirMiniHistoryEventKind::HypopneaEnd:
            record.code = report_event_code_value(ReportEventCode::Hypopnea);
            break;
        case AirMiniHistoryEventKind::CentralApneaEnd:
            record.code = report_event_code_value(ReportEventCode::CentralApnea);
            break;
        case AirMiniHistoryEventKind::ObstructiveApneaEnd:
            record.code = report_event_code_value(
                ReportEventCode::ObstructiveApnea);
            break;
        case AirMiniHistoryEventKind::ApneaEnd:
            record.code = report_event_code_value(
                ReportEventCode::UnclassifiedApnea);
            break;
        case AirMiniHistoryEventKind::ReraEnd:
            record.code = report_event_code_value(ReportEventCode::Arousal);
            break;
        default:
            return false;
    }

    record.start_ms = event.timestamp_ms;
    if (event.has_duration &&
        !add_duration_ms(event.timestamp_ms,
                         event.duration_seconds,
                         record.start_ms,
                         record.duration_ms)) {
        return false;
    }
    return true;
}

bool AirMiniHistoryReportBuilder::append_clipped_event(
    const ReportEventRecord &record,
    const NightCatalogTimeRange &session) {
    if (record.duration_ms == 0) {
        if (record.start_ms < session.start_ms ||
            record.start_ms >= session.end_ms) {
            return true;
        }
    } else {
        if (record.start_ms > INT64_MAX - record.duration_ms) return false;
        const int64_t end_ms = record.start_ms + record.duration_ms;
        if (end_ms <= session.start_ms || record.start_ms >= session.end_ms) {
            return true;
        }
    }

    ReportEventRecord clipped = record;
    const int64_t original_end = record.start_ms + record.duration_ms;
    if (record.duration_ms > 0) {
        clipped.start_ms = std::max(record.start_ms, session.start_ms);
        const int64_t end_ms = std::min(original_end, session.end_ms);
        if (end_ms <= clipped.start_ms ||
            end_ms - clipped.start_ms > INT32_MAX) {
            return true;
        }
        clipped.duration_ms = static_cast<int32_t>(end_ms - clipped.start_ms);
    }

    if (!report_append_event_record(event_payload_, clipped)) return false;
    event_mask_ |= report_event_source_mask(clipped);

    const int64_t end_for_coverage = clipped.duration_ms > 0
        ? clipped.start_ms + clipped.duration_ms
        : clipped.start_ms + 1;
    if (event_payload_.size() == report_event_record_wire_size()) {
        event_coverage_start_ms_ = clipped.start_ms;
        event_coverage_end_ms_ = end_for_coverage;
    } else {
        event_coverage_start_ms_ = std::min(event_coverage_start_ms_,
                                            clipped.start_ms);
        event_coverage_end_ms_ = std::max(event_coverage_end_ms_,
                                          end_for_coverage);
    }
    return true;
}

void AirMiniHistoryReportBuilder::reset_event_session() {
    event_mask_ = 0;
    event_coverage_start_ms_ = 0;
    event_coverage_end_ms_ = 0;
    event_payload_.clear();
    csr_open_ = false;
    csr_start_ms_ = 0;
}

bool AirMiniHistoryReportBuilder::finish_event_session(
    const NightCatalogTimeRange &session) {
    const bool complete = projection_->respiratory_complete;
    if (!complete && event_payload_.size() == 0) return true;

    ReportFallbackSectionInput section;
    section.kind = complete
        ? ReportFallbackSectionKind::Events
        : ReportFallbackSectionKind::PartialEvents;
    section.source = ReportSourceId::RespiratoryEvents;
    section.signal = ReportSignalId::Invalid;
    section.event_mask = complete ? REPORT_EVENT_ALL : event_mask_;
    section.payload_schema = REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1;
    section.record_count = static_cast<uint32_t>(
        event_payload_.size() / report_event_record_wire_size());
    section.coverage = complete
        ? session
        : NightCatalogTimeRange{event_coverage_start_ms_,
                                event_coverage_end_ms_};
    section.payload = event_payload_.data();
    section.payload_size = event_payload_.size();
    if (section_count_ >= ReportFallbackArtifactCodec::MaxSections) {
        return false;
    }
    if (section.event_mask == 0 || !section.coverage.valid() ||
        !artifact_builder_.append_section(section)) {
        return false;
    }
    ++section_count_;
    return true;
}

void AirMiniHistoryReportBuilder::poll_events(size_t &budget) {
    const auto &events = projection_->events;
    while (budget > 0) {
        if (event_session_index_ >= sessions_.size()) {
            phase_ = Phase::Finish;
            return;
        }

        const NightCatalogTimeRange &session =
            sessions_[event_session_index_];
        if (event_cursor_ >= events.size() ||
            !event_may_reach_session_end(events[event_cursor_], session)) {
            if (!finish_event_session(session)) {
                fail(AirMiniHistoryReportError::TooManySections);
                return;
            }
            --budget;
            ++event_session_index_;
            reset_event_session();
            continue;
        }

        const AirMiniHistoryEvent &event = events[event_cursor_++];
        --budget;
        if (event.kind == AirMiniHistoryEventKind::CsrStart) {
            if (!event_boundary(event, csr_start_ms_)) {
                fail(AirMiniHistoryReportError::InvalidEvent);
                return;
            }
            csr_open_ = true;
            continue;
        }
        if (event.kind == AirMiniHistoryEventKind::CsrEnd) {
            if (!csr_open_) continue;

            int64_t end_ms = 0;
            if (!event_boundary(event, end_ms) || end_ms <= csr_start_ms_ ||
                end_ms - csr_start_ms_ > INT32_MAX) {
                csr_open_ = false;
                csr_start_ms_ = 0;
                continue;
            }

            ReportEventRecord record;
            record.start_ms = csr_start_ms_;
            record.duration_ms = static_cast<int32_t>(end_ms - csr_start_ms_);
            record.code = report_event_code_value(ReportEventCode::Csr);
            if (!append_clipped_event(record, session)) {
                fail(AirMiniHistoryReportError::PayloadBuildFailed);
                return;
            }
            csr_open_ = false;
            csr_start_ms_ = 0;
            continue;
        }

        ReportEventRecord record;
        if (!make_event_record(event, record)) {
            continue;
        }
        if (!append_clipped_event(record, session)) {
            fail(AirMiniHistoryReportError::PayloadBuildFailed);
            return;
        }
    }
}

bool AirMiniHistoryReportBuilder::poll(size_t budget) {
    if (phase_ == Phase::Idle || phase_ == Phase::Failed) return false;
    if (phase_ == Phase::Complete) return true;
    if (budget == 0) {
        fail(AirMiniHistoryReportError::InvalidInput);
        return false;
    }

    while (budget > 0 && active()) {
        switch (phase_) {
            case Phase::InspiratoryPressure:
            case Phase::Leak:
                poll_series(budget);
                break;
            case Phase::Events:
                poll_events(budget);
                break;
            case Phase::Finish:
                result_ = artifact_builder_.finish();
                if (!result_) {
                    if (section_count_ == 0) {
                        error_ = AirMiniHistoryReportError::EmptyArtifact;
                        phase_ = Phase::Complete;
                    } else {
                        fail(AirMiniHistoryReportError::ArtifactBuildFailed);
                    }
                    break;
                }
                phase_ = Phase::Complete;
                break;
            case Phase::Idle:
            case Phase::Complete:
            case Phase::Failed:
                break;
        }
    }
    return !failed();
}

}  // namespace aircannect
