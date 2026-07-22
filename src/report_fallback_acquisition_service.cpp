#include "report_fallback_acquisition_service.h"

#include <algorithm>
#include <limits>
#include <stdio.h>
#include <string.h>

#include "report_planner.h"
#include "report_sources.h"

namespace aircannect {
namespace {

constexpr size_t MAX_EVENT_BYTES = 64 * 1024;

NightCatalogTimeRange intersect_ranges(const NightCatalogTimeRange &lhs,
                                       const NightCatalogTimeRange &rhs) {
    return {
        std::max(lhs.start_ms, rhs.start_ms),
        std::min(lhs.end_ms, rhs.end_ms),
    };
}

bool ranges_overlap(const NightCatalogTimeRange &lhs,
                    const NightCatalogTimeRange &rhs) {
    return lhs.start_ms < rhs.end_ms && rhs.start_ms < lhs.end_ms;
}

uint32_t sample_index_at_or_after(int64_t chunk_start_ms,
                                  uint32_t interval_ms,
                                  int64_t timestamp_ms,
                                  uint32_t sample_count) {
    if (timestamp_ms <= chunk_start_ms) return 0;

    const uint64_t delta = static_cast<uint64_t>(
        timestamp_ms - chunk_start_ms);
    const uint64_t index =
        (delta + static_cast<uint64_t>(interval_ms) - 1u) / interval_ms;
    return index >= sample_count
        ? sample_count
        : static_cast<uint32_t>(index);
}

ReportFallbackSectionInput section_input(
    const NightCatalogFallbackSection &section) {
    ReportFallbackSectionInput input;
    input.kind = section.kind;
    input.source = section.source;
    input.signal = section.signal;
    input.event_mask = section.event_mask;
    input.payload_schema = section.payload_schema;
    input.record_count = section.record_count;
    input.sample_interval_ms = section.sample_interval_ms;
    input.coverage = section.coverage;
    input.payload_size = section.data_size;
    return input;
}

const ReportSignalDef *signal_from_chunk(const ReportParsedChunk &chunk) {
    if (!chunk.name || !chunk.name[0]) return nullptr;

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        if (signals[i].fallback_source == chunk.source &&
            strcmp(signals[i].store_name, chunk.name) == 0) {
            return &signals[i];
        }
    }
    return nullptr;
}

StorageReadLane preserve_lane(StorageReadLane requested) {
    return requested == StorageReadLane::Foreground
        ? StorageReadLane::Foreground
        : StorageReadLane::Report;
}

}  // namespace

ReportFallbackAcquisitionService::~ReportFallbackAcquisitionService() {
    abandon_operations();
}

void ReportFallbackAcquisitionService::begin(
    StorageReadPort &read_port,
    StorageAtomicWritePort &write_port,
    ReportSpoolPort &spool_port) {
    reset();
    read_port_ = &read_port;
    write_port_ = &write_port;
    spool_port_ = &spool_port;
}

OperationAdmission ReportFallbackAcquisitionService::start(
    std::shared_ptr<const ReportReadPlan> plan,
    uint32_t generation,
    StorageReadLane read_lane,
    StorageAtomicWriteLane write_lane) {
    if (status_.active()) return OperationAdmission::Busy;

    reset();
    if (!read_port_ || !write_port_ || !spool_port_ || !plan ||
        generation == 0) {
        fail("fallback_acquisition_invalid");
        return OperationAdmission::Rejected;
    }

    plan_ = std::move(plan);
    generation_ = generation;
    read_lane_ = read_lane;
    write_lane_ = write_lane;
    status_.sleep_day = plan_->key().sleep_day;
    if (!prepare()) return OperationAdmission::Rejected;

    status_.state = ReportFallbackAcquisitionState::Preserving;
    return OperationAdmission::Accepted;
}

bool ReportFallbackAcquisitionService::poll() {
    switch (status_.state) {
        case ReportFallbackAcquisitionState::Preserving:
            return poll_preservation();
        case ReportFallbackAcquisitionState::Fetching:
            return poll_fetch();
        case ReportFallbackAcquisitionState::Publishing:
            return poll_publication();
        case ReportFallbackAcquisitionState::Cancelling:
            return poll_cancellation();
        case ReportFallbackAcquisitionState::Idle:
        case ReportFallbackAcquisitionState::Ready:
        case ReportFallbackAcquisitionState::Failed:
        case ReportFallbackAcquisitionState::Cancelled:
            return false;
    }
    return false;
}

void ReportFallbackAcquisitionService::cancel() {
    if (!status_.active()) return;
    begin_terminal(ReportFallbackAcquisitionState::Cancelled, nullptr);
}

void ReportFallbackAcquisitionService::reset() {
    if (status_.active()) {
        cancel();
        return;
    }

    builder_.reset();
    event_records_.clear();
    plan_.reset();
    replacement_.reset();
    std::fill(std::begin(targets_), std::end(targets_), SourceTarget{});
    memset(missing_signal_masks_, 0, sizeof(missing_signal_masks_));
    memset(rebuild_events_, 0, sizeof(rebuild_events_));
    std::fill(std::begin(added_series_),
              std::end(added_series_),
              SeriesCoverage{});
    target_count_ = 0;
    target_index_ = 0;
    added_series_count_ = 0;
    session_count_ = 0;
    preserve_file_index_ = 0;
    preserve_section_index_ = 0;
    preserve_file_ = nullptr;
    preserve_section_ = nullptr;
    preserve_payload_ = nullptr;
    read_ticket_ = {};
    spool_ticket_ = {};
    write_ticket_ = {};
    generation_ = 0;
    read_lane_ = StorageReadLane::Report;
    write_lane_ = StorageAtomicWriteLane::Maintenance;
    terminal_state_ = ReportFallbackAcquisitionState::Idle;
    status_ = {};
}

bool ReportFallbackAcquisitionService::prepare() {
    if (!plan_->key().valid() ||
        (plan_->acquirable_signal_mask() == 0 &&
         plan_->missing_event_mask() == 0)) {
        fail("fallback_acquisition_not_required");
        return false;
    }
    if (!build_targets()) return false;

    const NightCatalogRecord &night = plan_->night();
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(night, session_count_);
    if (!sessions || session_count_ == 0 ||
        session_count_ > ReportFallbackArtifactCodec::MaxSessions ||
        !builder_.begin(night.sleep_day,
                        night.day_start_ms,
                        night.day_end_ms,
                        sessions,
                        session_count_)) {
        fail("fallback_builder_start_failed");
        return false;
    }

    event_records_.set_max_size(MAX_EVENT_BYTES);
    status_.sources_total = static_cast<uint32_t>(target_count_);
    return true;
}

bool ReportFallbackAcquisitionService::build_targets() {
    const NightCatalogRecord &night = plan_->night();
    size_t catalog_session_count = 0;
    const NightCatalogTimeRange *catalog_sessions =
        plan_->catalog().sessions(night, catalog_session_count);
    if (!catalog_sessions || catalog_session_count == 0 ||
        catalog_session_count > ReportFallbackArtifactCodec::MaxSessions) {
        fail("fallback_sessions_invalid");
        return false;
    }

    auto add_target = [this](ReportSourceId source, int64_t from_ms) {
        if (source == ReportSourceId::Summary || from_ms <= 0) return false;

        for (size_t i = 0; i < target_count_; ++i) {
            if (targets_[i].source != source) continue;
            targets_[i].from_ms = std::min(targets_[i].from_ms, from_ms);
            return true;
        }
        if (target_count_ >= MaxSourceTargets) return false;

        targets_[target_count_++] = {source, from_ms};
        return true;
    };

    for (size_t i = 0; i < plan_->session_count(); ++i) {
        const ReportReadSession *session = plan_->session(i);
        if (!session || session->catalog_session_index >=
                            catalog_session_count) {
            fail("fallback_plan_session_invalid");
            return false;
        }

        const size_t catalog_index = session->catalog_session_index;
        missing_signal_masks_[catalog_index] |=
            session->missing_signal_mask;
        const uint32_t acquirable = session->missing_signal_mask &
            ~session->unavailable_signal_mask;

        size_t signal_count = 0;
        const ReportSignalDef *signals = report_signal_defs(signal_count);
        for (size_t signal_index = 0;
             signal_index < signal_count;
             ++signal_index) {
            const ReportSignalDef &signal = signals[signal_index];
            if ((acquirable & report_signal_bit(signal.id)) == 0) continue;
            if (!add_target(signal.fallback_source,
                            catalog_sessions[catalog_index].start_ms)) {
                fail("fallback_source_targets_full");
                return false;
            }
        }

        if (session->missing_event_mask != 0 &&
            !add_target(ReportSourceId::RespiratoryEvents,
                        catalog_sessions[catalog_index].start_ms)) {
            fail("fallback_event_target_failed");
            return false;
        }
    }

    if (target_count_ == 0) {
        fail("fallback_source_targets_empty");
        return false;
    }

    std::sort(targets_,
              targets_ + target_count_,
              [](const SourceTarget &lhs, const SourceTarget &rhs) {
                  return static_cast<uint8_t>(lhs.source) <
                      static_cast<uint8_t>(rhs.source);
              });

    const SourceTarget *events =
        source_target(ReportSourceId::RespiratoryEvents);
    if (events) {
        for (size_t i = 0; i < catalog_session_count; ++i) {
            rebuild_events_[i] =
                catalog_sessions[i].end_ms > events->from_ms;
        }
    }
    return true;
}

bool ReportFallbackAcquisitionService::poll_preservation() {
    if (read_ticket_.valid()) return finish_preserved_read();

    if (preserve_section_) {
        StorageReadCommand command;
        command.path = plan_->catalog().path(*preserve_file_);
        command.offset = preserve_section_->data_offset;
        command.length = preserve_section_->data_size;
        command.lane = preserve_lane(read_lane_);
        command.generation = generation_;
        const OperationSubmission submitted =
            read_port_->request_read(command);
        if (submitted.admission == OperationAdmission::Busy) return false;
        if (!submitted.accepted()) {
            fail("fallback_preserved_read_rejected");
            return true;
        }

        read_ticket_ = submitted.ticket;
        return true;
    }

    return select_preserved_section();
}

bool ReportFallbackAcquisitionService::select_preserved_section() {
    const NightCatalogRecord &night = plan_->night();
    size_t file_count = 0;
    const NightCatalogFallbackFile *files =
        plan_->catalog().fallback_files(night, file_count);
    if (file_count > 0 && !files) {
        fail("fallback_catalog_files_invalid");
        return true;
    }

    while (preserve_file_index_ < file_count) {
        const NightCatalogFallbackFile &file =
            files[preserve_file_index_];
        size_t section_count = 0;
        const NightCatalogFallbackSection *sections =
            plan_->catalog().fallback_sections(file, section_count);
        if (section_count > 0 && !sections) {
            fail("fallback_catalog_sections_invalid");
            return true;
        }

        if (preserve_section_index_ >= section_count) {
            preserve_file_index_++;
            preserve_section_index_ = 0;
            continue;
        }

        const NightCatalogFallbackSection &section =
            sections[preserve_section_index_++];
        if (!should_preserve(section)) continue;

        ReportFallbackSectionInput input = section_input(section);
        if (section.data_size == 0) {
            if (!builder_.append_section(input)) {
                fail("fallback_preserved_section_rejected");
                return true;
            }
            status_.sections_preserved++;
            return true;
        }

        preserve_file_ = &file;
        preserve_section_ = &section;
        if (!builder_.reserve_section(input, preserve_payload_)) {
            fail("fallback_preserved_section_reserve_failed");
        }
        return true;
    }

    status_.state = ReportFallbackAcquisitionState::Fetching;
    return true;
}

bool ReportFallbackAcquisitionService::finish_preserved_read() {
    StorageReadCompletion completion;
    if (!read_port_->take_completion(read_ticket_, completion)) return false;

    read_ticket_ = {};
    const bool succeeded =
        completion.outcome.disposition == OperationDisposition::Succeeded &&
        completion.prepared.valid() && preserve_section_ &&
        completion.prepared.length == preserve_section_->data_size;
    const size_t copied = succeeded
        ? read_port_->read_prepared(completion.prepared,
                                    0,
                                    preserve_payload_,
                                    preserve_section_->data_size)
        : 0;
    if (completion.prepared.valid()) {
        read_port_->release_prepared(completion.prepared);
    }

    if (!succeeded || copied != preserve_section_->data_size ||
        !builder_.commit_reserved_section(
            true, preserve_section_->data_crc32)) {
        fail(completion.error[0]
                 ? completion.error
                 : "fallback_preserved_read_failed");
        return true;
    }

    status_.sections_preserved++;
    preserve_file_ = nullptr;
    preserve_section_ = nullptr;
    preserve_payload_ = nullptr;
    return true;
}

bool ReportFallbackAcquisitionService::should_preserve(
    const NightCatalogFallbackSection &section) const {
    if (section.kind == ReportFallbackSectionKind::Series) return true;

    size_t count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), count);
    if (!sessions) return false;

    for (size_t i = 0; i < count; ++i) {
        if (!ranges_overlap(section.coverage, sessions[i])) continue;

        if (section.kind == ReportFallbackSectionKind::Events &&
            event_session_targeted(i)) {
            return false;
        }
        if (section.kind == ReportFallbackSectionKind::Unavailable &&
            signal_targeted(i, section.signal, section.source)) {
            return false;
        }
    }
    return true;
}

bool ReportFallbackAcquisitionService::poll_fetch() {
    if (target_index_ >= target_count_) {
        status_.state = ReportFallbackAcquisitionState::Publishing;
        return true;
    }
    if (!spool_ticket_.valid() && !submit_current_fetch()) return false;
    if (status_.state != ReportFallbackAcquisitionState::Fetching) {
        return true;
    }
    if (consume_fetch_round()) return true;
    return finish_current_fetch();
}

bool ReportFallbackAcquisitionService::submit_current_fetch() {
    const SourceTarget &target = targets_[target_index_];
    ReportSpoolFetchCommand command;
    command.source = target.source;
    command.from_ms = target.from_ms;
    command.generation = generation_;
    const OperationSubmission submitted =
        spool_port_->request_fetch(command);
    if (submitted.admission == OperationAdmission::Busy) return false;
    if (!submitted.accepted()) {
        fail("fallback_spool_request_rejected");
        return true;
    }

    spool_ticket_ = submitted.ticket;
    status_.source = target.source;
    return true;
}

bool ReportFallbackAcquisitionService::consume_fetch_round() {
    ReportSpoolFetchRound round;
    if (!spool_port_->take_round(spool_ticket_, round)) return false;

    const bool parsed = parse_round(round.result);
    round.clear();
    return parsed || status_.state != ReportFallbackAcquisitionState::Fetching;
}

bool ReportFallbackAcquisitionService::finish_current_fetch() {
    ReportSpoolFetchCompletion completion;
    if (!spool_port_->take_completion(spool_ticket_, completion)) {
        return false;
    }

    spool_ticket_ = {};
    if (completion.outcome.disposition !=
            OperationDisposition::Succeeded ||
        completion.result.truncated) {
        fail(completion.error[0]
                 ? completion.error
                 : "fallback_spool_fetch_failed");
        completion.clear();
        return true;
    }

    const ReportSourceDef *source =
        report_source_def(targets_[target_index_].source);
    if (!source) {
        fail("fallback_source_invalid");
        completion.clear();
        return true;
    }

    const bool finalized =
        targets_[target_index_].source ==
                ReportSourceId::RespiratoryEvents
            ? append_event_sections()
            : report_source_is_sampled(*source) &&
                  append_unavailable_sections();
    completion.clear();
    if (!finalized) return true;

    status_.sources_completed++;
    target_index_++;
    status_.source = ReportSourceId::Summary;
    if (target_index_ >= target_count_) {
        status_.state = ReportFallbackAcquisitionState::Publishing;
    }
    return true;
}

bool ReportFallbackAcquisitionService::parse_round(
    ReportSpoolResult &result) {
    char error[AC_STORAGE_ERROR_MAX] = {};
    const ReportSourceId source = targets_[target_index_].source;
    const bool parsed = source == ReportSourceId::RespiratoryEvents
        ? report_parse_event_spool(result,
                                   source,
                                   accept_parsed_chunk,
                                   this,
                                   error,
                                   sizeof(error))
        : report_parse_series_spool(result,
                                    source,
                                    accept_parsed_chunk,
                                    this,
                                    error,
                                    sizeof(error));
    if (parsed) return true;
    if (strcmp(error, "spool_empty") == 0) return true;
    if (status_.state == ReportFallbackAcquisitionState::Fetching) {
        fail(error[0] ? error : "fallback_spool_parse_failed");
    }
    return false;
}

bool ReportFallbackAcquisitionService::accept_parsed_chunk(
    void *context,
    const ReportParsedChunk &chunk) {
    ReportFallbackAcquisitionService *service =
        static_cast<ReportFallbackAcquisitionService *>(context);
    if (!service) return false;

    return chunk.kind == ReportStoreChunkKind::Events
        ? service->accept_event_chunk(chunk)
        : service->accept_series_chunk(chunk);
}

bool ReportFallbackAcquisitionService::accept_series_chunk(
    const ReportParsedChunk &chunk) {
    if (target_index_ >= target_count_ ||
        chunk.source != targets_[target_index_].source ||
        chunk.kind != ReportStoreChunkKind::Series ||
        chunk.payload_schema != REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2 ||
        !chunk.payload || chunk.payload_len == 0 ||
        chunk.end_ms <= chunk.start_ms) {
        fail("fallback_series_chunk_invalid");
        return false;
    }

    const ReportSignalDef *signal = signal_from_chunk(chunk);
    if (!signal) return true;

    ReportSeriesV2UniformView view;
    if (!report_series_payload_v2_uniform_view(chunk.payload,
                                               chunk.payload_len,
                                               chunk.record_count,
                                               view)) {
        fail("fallback_series_payload_invalid");
        return false;
    }

    size_t count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), count);
    if (!sessions || count != session_count_) {
        fail("fallback_series_sessions_invalid");
        return false;
    }

    for (size_t session_index = 0;
         session_index < session_count_;
         ++session_index) {
        if (!signal_targeted(session_index,
                             signal->id,
                             chunk.source)) {
            continue;
        }

        const NightCatalogTimeRange overlap =
            intersect_ranges(sessions[session_index],
                             {chunk.start_ms, chunk.end_ms});
        if (!overlap.valid()) continue;

        uint32_t cursor = sample_index_at_or_after(
            chunk.start_ms,
            view.interval_ms,
            overlap.start_ms,
            view.sample_count);
        const uint32_t end = sample_index_at_or_after(
            chunk.start_ms,
            view.interval_ms,
            overlap.end_ms,
            view.sample_count);
        while (cursor < end) {
            const int64_t timestamp_ms =
                chunk.start_ms +
                static_cast<int64_t>(cursor) * view.interval_ms;
            bool covered = false;
            int64_t covered_end_ms = timestamp_ms;
            int64_t next_start_ms = INT64_MAX;
            series_coverage_after(chunk.source,
                                  signal->id,
                                  timestamp_ms,
                                  covered,
                                  covered_end_ms,
                                  next_start_ms);
            if (covered) {
                const uint32_t next = sample_index_at_or_after(
                    chunk.start_ms,
                    view.interval_ms,
                    covered_end_ms,
                    view.sample_count);
                cursor = std::max(cursor + 1, next);
                continue;
            }

            uint32_t run_end = end;
            if (next_start_ms != INT64_MAX) {
                run_end = std::min(
                    run_end,
                    sample_index_at_or_after(chunk.start_ms,
                                             view.interval_ms,
                                             next_start_ms,
                                             view.sample_count));
            }
            if (run_end <= cursor) run_end = cursor + 1;

            if (!append_series_run(chunk,
                                   view,
                                   session_index,
                                   cursor,
                                   run_end - cursor)) {
                return false;
            }
            cursor = run_end;
        }
    }
    return true;
}

bool ReportFallbackAcquisitionService::append_series_run(
    const ReportParsedChunk &chunk,
    const ReportSeriesV2UniformView &view,
    size_t session_index,
    uint32_t first_sample,
    uint32_t sample_count) {
    const ReportSignalDef *signal = signal_from_chunk(chunk);
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), session_count);
    if (!signal || !sessions || session_index >= session_count ||
        sample_count == 0 ||
        static_cast<int64_t>(sample_count) >
            INT64_MAX / view.interval_ms) {
        fail("fallback_series_slice_invalid");
        return false;
    }

    const int64_t start_ms =
        chunk.start_ms +
        static_cast<int64_t>(first_sample) * view.interval_ms;
    const int64_t end_ms = std::min(
        sessions[session_index].end_ms,
        start_ms + static_cast<int64_t>(sample_count) * view.interval_ms);
    const size_t payload_size =
        report_series_payload_v2_uniform_slice_size(
            view, first_sample, sample_count);
    if (end_ms <= start_ms || payload_size == 0) {
        fail("fallback_series_slice_invalid");
        return false;
    }

    ReportFallbackSectionInput section;
    section.kind = ReportFallbackSectionKind::Series;
    section.source = chunk.source;
    section.signal = signal->id;
    section.payload_schema = REPORT_SERIES_CHUNK_PAYLOAD_SCHEMA_V2;
    section.record_count = sample_count;
    section.sample_interval_ms = view.interval_ms;
    section.coverage = {start_ms, end_ms};
    section.payload_size = payload_size;

    uint8_t *payload = nullptr;
    if (!builder_.reserve_section(section, payload) ||
        !report_write_series_payload_v2_uniform_slice(
            view,
            first_sample,
            sample_count,
            payload,
            payload_size) ||
        !builder_.commit_reserved_section() ||
        !append_series_coverage(chunk.source,
                                signal->id,
                                section.coverage)) {
        fail("fallback_series_append_failed");
        return false;
    }

    status_.sections_added++;
    return true;
}

bool ReportFallbackAcquisitionService::accept_event_chunk(
    const ReportParsedChunk &chunk) {
    if (target_index_ >= target_count_ ||
        targets_[target_index_].source !=
            ReportSourceId::RespiratoryEvents ||
        chunk.source != ReportSourceId::RespiratoryEvents ||
        chunk.kind != ReportStoreChunkKind::Events ||
        chunk.payload_schema != REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1 ||
        chunk.payload_len !=
            static_cast<size_t>(chunk.record_count) *
                report_event_record_wire_size()) {
        fail("fallback_event_chunk_invalid");
        return false;
    }

    for (size_t i = 0; i < chunk.record_count; ++i) {
        ReportEventRecord event;
        if (!report_read_event_record(chunk.payload,
                                      chunk.payload_len,
                                      i,
                                      event) ||
            report_event_source_mask(event) == 0) {
            fail("fallback_event_record_invalid");
            return false;
        }
        if (event_session_for(event) == SIZE_MAX) continue;
        if (!append_unique_event(event)) {
            fail("fallback_event_buffer_full");
            return false;
        }
    }
    return true;
}

bool ReportFallbackAcquisitionService::append_event_sections() {
    const size_t record_bytes = report_event_record_wire_size();
    const size_t record_count = event_records_.size() / record_bytes;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), session_count);
    if (!sessions || session_count != session_count_) {
        fail("fallback_event_sessions_invalid");
        return false;
    }

    for (size_t session_index = 0;
         session_index < session_count_;
         ++session_index) {
        if (!event_session_targeted(session_index)) continue;

        uint32_t selected = 0;
        for (size_t i = 0; i < record_count; ++i) {
            ReportEventRecord event;
            if (!report_read_event_record(event_records_.data(),
                                          event_records_.size(),
                                          i,
                                          event)) {
                fail("fallback_event_buffer_invalid");
                return false;
            }
            if (event_session_for(event) == session_index) selected++;
        }

        ReportFallbackSectionInput section;
        section.kind = ReportFallbackSectionKind::Events;
        section.source = ReportSourceId::RespiratoryEvents;
        section.signal = ReportSignalId::Count;
        section.event_mask = REPORT_EVENT_ALL;
        section.payload_schema = REPORT_EVENT_CHUNK_PAYLOAD_SCHEMA_V1;
        section.record_count = selected;
        section.coverage = sessions[session_index];
        section.payload_size =
            static_cast<size_t>(selected) * record_bytes;

        uint8_t *payload = nullptr;
        if (!builder_.reserve_section(section, payload)) {
            fail("fallback_event_section_reserve_failed");
            return false;
        }

        size_t write = 0;
        for (size_t i = 0; i < record_count; ++i) {
            ReportEventRecord event;
            if (!report_read_event_record(event_records_.data(),
                                          event_records_.size(),
                                          i,
                                          event)) {
                builder_.discard_reserved_section();
                fail("fallback_event_buffer_invalid");
                return false;
            }
            if (event_session_for(event) != session_index) continue;

            memcpy(payload + write * record_bytes,
                   event_records_.data() + i * record_bytes,
                   record_bytes);
            write++;
        }

        if (write != selected || !builder_.commit_reserved_section()) {
            fail("fallback_event_section_commit_failed");
            return false;
        }
        status_.sections_added++;
    }

    event_records_.clear();
    event_records_.set_max_size(MAX_EVENT_BYTES);
    return true;
}

bool ReportFallbackAcquisitionService::append_unavailable_sections() {
    if (target_index_ >= target_count_) return false;

    const ReportSourceId source = targets_[target_index_].source;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), session_count);
    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    if (!sessions || session_count != session_count_ || !signals) {
        fail("fallback_unavailable_inputs_invalid");
        return false;
    }

    for (size_t session_index = 0;
         session_index < session_count_;
         ++session_index) {
        for (size_t signal_index = 0;
             signal_index < signal_count;
             ++signal_index) {
            const ReportSignalDef &signal = signals[signal_index];
            if (!signal_targeted(session_index, signal.id, source) ||
                series_session_complete(session_index,
                                        source,
                                        signal.id)) {
                continue;
            }

            ReportFallbackSectionInput section;
            section.kind = ReportFallbackSectionKind::Unavailable;
            section.source = source;
            section.signal = signal.id;
            section.coverage = sessions[session_index];
            if (!builder_.append_section(section)) {
                fail("fallback_unavailable_append_failed");
                return false;
            }
            status_.sections_added++;
            status_.unavailable_added++;
        }
    }
    return true;
}

bool ReportFallbackAcquisitionService::poll_publication() {
    if (write_ticket_.valid()) return finish_publication();
    return submit_publication();
}

bool ReportFallbackAcquisitionService::submit_publication() {
    if (!replacement_) {
        replacement_ = builder_.finish();
        if (!replacement_) {
            fail("fallback_replacement_build_failed");
            return true;
        }

        ReportFallbackArtifactInfo info;
        if (!ReportFallbackArtifactCodec::inspect_header(
                replacement_->data(), replacement_->size(), info)) {
            fail("fallback_replacement_invalid");
            return true;
        }
        status_.replacement_identity = info.content_identity;
    }

    char path[AC_STORAGE_PATH_MAX] = {};
    if (!report_fallback_artifact_path(status_.sleep_day,
                                       path,
                                       sizeof(path))) {
        fail("fallback_replacement_path_invalid");
        return true;
    }

    StorageAtomicWriteCommand command;
    command.path = path;
    command.bytes = replacement_;
    command.lane = write_lane_;
    command.generation = generation_;
    const OperationSubmission submitted =
        write_port_->request_write(command);
    if (submitted.admission == OperationAdmission::Busy) return false;
    if (!submitted.accepted()) {
        fail("fallback_replacement_write_rejected");
        return true;
    }

    write_ticket_ = submitted.ticket;
    return true;
}

bool ReportFallbackAcquisitionService::finish_publication() {
    StorageAtomicWriteCompletion completion;
    if (!write_port_->take_completion(write_ticket_, completion)) {
        return false;
    }

    write_ticket_ = {};
    if (completion.outcome.disposition !=
            OperationDisposition::Succeeded ||
        !replacement_ || completion.bytes_written != replacement_->size()) {
        fail(completion.error[0]
                 ? completion.error
                 : "fallback_replacement_write_failed");
        return true;
    }

    finish(ReportFallbackAcquisitionState::Ready);
    return true;
}

bool ReportFallbackAcquisitionService::poll_cancellation() {
    if (spool_ticket_.valid()) {
        ReportSpoolFetchCompletion completion;
        if (!spool_port_->take_completion(spool_ticket_, completion)) {
            (void)spool_port_->cancel(spool_ticket_);
            return false;
        }
        completion.clear();
        spool_ticket_ = {};
    }

    finish(terminal_state_);
    return true;
}

bool ReportFallbackAcquisitionService::source_targeted(
    ReportSourceId source) const {
    return source_target(source) != nullptr;
}

const ReportFallbackAcquisitionService::SourceTarget *
ReportFallbackAcquisitionService::source_target(
    ReportSourceId source) const {
    for (size_t i = 0; i < target_count_; ++i) {
        if (targets_[i].source == source) return &targets_[i];
    }
    return nullptr;
}

bool ReportFallbackAcquisitionService::signal_targeted(
    size_t session_index,
    ReportSignalId signal,
    ReportSourceId source) const {
    if (session_index >= session_count_) return false;

    const SourceTarget *target = source_target(source);
    const ReportSignalDef *definition = report_signal_def(signal);
    size_t count = 0;
    const NightCatalogTimeRange *sessions = plan_
        ? plan_->catalog().sessions(plan_->night(), count)
        : nullptr;
    const uint32_t bit = report_signal_bit(signal);
    return target && definition && sessions && session_index < count &&
           definition->fallback_source == source && bit != 0 &&
           sessions[session_index].end_ms > target->from_ms &&
           (missing_signal_masks_[session_index] & bit) != 0;
}

bool ReportFallbackAcquisitionService::event_session_targeted(
    size_t session_index) const {
    return session_index < session_count_ && rebuild_events_[session_index];
}

void ReportFallbackAcquisitionService::series_coverage_after(
    ReportSourceId source,
    ReportSignalId signal,
    int64_t timestamp_ms,
    bool &covered,
    int64_t &covered_end_ms,
    int64_t &next_start_ms) const {
    covered = false;
    covered_end_ms = timestamp_ms;
    next_start_ms = INT64_MAX;

    auto consider = [&](const NightCatalogTimeRange &range) {
        if (!range.valid()) return;
        if (range.start_ms <= timestamp_ms && range.end_ms > timestamp_ms) {
            covered = true;
            covered_end_ms = std::max(covered_end_ms, range.end_ms);
        } else if (range.start_ms > timestamp_ms) {
            next_start_ms = std::min(next_start_ms, range.start_ms);
        }
    };

    size_t file_count = 0;
    const NightCatalogFallbackFile *files = plan_->catalog().fallback_files(
        plan_->night(), file_count);
    for (size_t file_index = 0;
         files && file_index < file_count;
         ++file_index) {
        size_t section_count = 0;
        const NightCatalogFallbackSection *sections =
            plan_->catalog().fallback_sections(files[file_index],
                                               section_count);
        for (size_t i = 0; sections && i < section_count; ++i) {
            if (sections[i].kind == ReportFallbackSectionKind::Series &&
                sections[i].source == source &&
                sections[i].signal == signal) {
                consider(sections[i].coverage);
            }
        }
    }

    for (size_t i = 0; i < added_series_count_; ++i) {
        if (added_series_[i].source == source &&
            added_series_[i].signal == signal) {
            consider(added_series_[i].range);
        }
    }
}

bool ReportFallbackAcquisitionService::series_session_complete(
    size_t session_index,
    ReportSourceId source,
    ReportSignalId signal) const {
    size_t count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), count);
    if (!sessions || session_index >= count) return false;

    const NightCatalogTimeRange &window = sessions[session_index];
    const int64_t target_start = std::min(
        window.end_ms,
        window.start_ms + REPORT_SOURCE_EDGE_TOLERANCE_MS);
    const int64_t target_end = std::max(
        window.start_ms,
        window.end_ms - REPORT_SOURCE_EDGE_TOLERANCE_MS);
    if (target_end <= target_start) return true;

    int64_t cursor = target_start;
    while (cursor < target_end) {
        bool covered = false;
        int64_t covered_end_ms = cursor;
        int64_t next_start_ms = INT64_MAX;
        series_coverage_after(source,
                              signal,
                              cursor,
                              covered,
                              covered_end_ms,
                              next_start_ms);
        (void)next_start_ms;
        if (!covered || covered_end_ms <= cursor) return false;
        cursor = covered_end_ms;
    }
    return true;
}

bool ReportFallbackAcquisitionService::append_series_coverage(
    ReportSourceId source,
    ReportSignalId signal,
    const NightCatalogTimeRange &range) {
    if (!range.valid() ||
        added_series_count_ >=
            ReportFallbackArtifactCodec::MaxSections) {
        return false;
    }

    added_series_[added_series_count_++] = {source, signal, range};
    return true;
}

size_t ReportFallbackAcquisitionService::event_session_for(
    const ReportEventRecord &event) const {
    size_t count = 0;
    const NightCatalogTimeRange *sessions =
        plan_->catalog().sessions(plan_->night(), count);
    if (!sessions) return SIZE_MAX;

    for (size_t i = 0; i < count; ++i) {
        if (event_session_targeted(i) &&
            event.start_ms >= sessions[i].start_ms &&
            event.start_ms < sessions[i].end_ms) {
            return i;
        }
    }

    size_t selected = SIZE_MAX;
    int64_t distance = INT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        if (!event_session_targeted(i) ||
            !report_event_overlaps_window(event,
                                          sessions[i].start_ms,
                                          sessions[i].end_ms,
                                          REPORT_SOURCE_EDGE_TOLERANCE_MS)) {
            continue;
        }

        const int64_t candidate = event.start_ms < sessions[i].start_ms
            ? sessions[i].start_ms - event.start_ms
            : event.start_ms - sessions[i].end_ms;
        if (candidate < distance) {
            selected = i;
            distance = candidate;
        }
    }
    return selected;
}

bool ReportFallbackAcquisitionService::append_unique_event(
    const ReportEventRecord &event) {
    const size_t count =
        event_records_.size() / report_event_record_wire_size();
    for (size_t i = 0; i < count; ++i) {
        ReportEventRecord existing;
        if (!report_read_event_record(event_records_.data(),
                                      event_records_.size(),
                                      i,
                                      existing)) {
            return false;
        }
        if (existing.start_ms == event.start_ms &&
            existing.duration_ms == event.duration_ms &&
            existing.code == event.code &&
            existing.flags == event.flags) {
            return true;
        }
    }
    return report_append_event_record(event_records_, event);
}

void ReportFallbackAcquisitionService::abandon_operations() {
    if (read_port_ && read_ticket_.valid()) {
        (void)read_port_->abandon(read_ticket_);
        read_ticket_ = {};
    }
    if (write_port_ && write_ticket_.valid()) {
        (void)write_port_->abandon(write_ticket_);
        write_ticket_ = {};
    }
    if (spool_port_ && spool_ticket_.valid()) {
        (void)spool_port_->cancel(spool_ticket_);
    }
    builder_.discard_reserved_section();
    preserve_file_ = nullptr;
    preserve_section_ = nullptr;
    preserve_payload_ = nullptr;
}

void ReportFallbackAcquisitionService::begin_terminal(
    ReportFallbackAcquisitionState state,
    const char *error) {
    terminal_state_ = state;
    if (error) {
        snprintf(status_.error, sizeof(status_.error), "%s", error);
    }

    abandon_operations();
    if (spool_ticket_.valid()) {
        status_.state = ReportFallbackAcquisitionState::Cancelling;
    } else {
        finish(state);
    }
}

void ReportFallbackAcquisitionService::fail(const char *error) {
    begin_terminal(ReportFallbackAcquisitionState::Failed,
                   error ? error : "fallback_acquisition_failed");
}

void ReportFallbackAcquisitionService::finish(
    ReportFallbackAcquisitionState state) {
    builder_.reset();
    event_records_.clear();
    plan_.reset();
    replacement_.reset();
    preserve_file_ = nullptr;
    preserve_section_ = nullptr;
    preserve_payload_ = nullptr;
    read_ticket_ = {};
    spool_ticket_ = {};
    write_ticket_ = {};
    terminal_state_ = ReportFallbackAcquisitionState::Idle;
    status_.state = state;
}

}  // namespace aircannect
