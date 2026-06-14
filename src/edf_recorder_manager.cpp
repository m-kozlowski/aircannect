#include "edf_recorder_manager.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "as11_rpc.h"
#include "debug_log.h"
#include "edf_event_annotation.h"
#include "edf_numeric_file_layout.h"
#include "edf_storage_catalog.h"
#include "edf_storage_worker.h"
#include "edf_str_settings.h"
#include "edf_time.h"

namespace aircannect {
namespace {

static constexpr uint32_t AC_EDF_STR_SETTINGS_TIMEOUT_MS = 12000;
static constexpr uint32_t AC_EDF_STR_SUMMARY_TIMEOUT_MS = 12000;
static constexpr size_t AC_EDF_GAP_RECORD_BUDGET = 3;

const char *const EDF_CAPTURE_EVENT_IDS =
    "TherapyEvents-RespiratoryEvents";

const char *acquire_status_name(StreamAcquireStatus status) {
    switch (status) {
        case StreamAcquireStatus::Acquired: return "acquired";
        case StreamAcquireStatus::AlreadyActive: return "already_active";
        case StreamAcquireStatus::Incompatible: return "incompatible";
        case StreamAcquireStatus::Full: return "full";
        case StreamAcquireStatus::Busy: return "busy";
        case StreamAcquireStatus::Rejected:
        default:
            return "rejected";
    }
}

const char *file_kind_name(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp: return "BRP";
        case EdfFileKind::Pld: return "PLD";
        case EdfFileKind::Sa2: return "SA2";
        default: return "EDF";
    }
}

const char *annotation_kind_name(EdfAnnotationKind kind) {
    switch (kind) {
        case EdfAnnotationKind::Eve: return "EVE";
        case EdfAnnotationKind::Csl: return "CSL";
        default: return "EDF";
    }
}

}  // namespace

void EdfRecorderManager::begin(RpcArbiter &arbiter,
                               SessionManager &session) {
    if (initialized_) return;
    arbiter_ = &arbiter;
    session_ = &session;
    status_.rpc_observer_registered =
        arbiter_->set_source_event_observer(RpcSource::EdfRecorder,
                                            rpc_event_observer, this);
    status_.event_observer_registered =
        arbiter_->add_event_frame_observer(event_frame_observer, this);
    if (!status_.rpc_observer_registered) {
        set_error("rpc_observer_register_failed");
    }
    if (!status_.event_observer_registered) {
        set_error("event_observer_register_failed");
    }
    assembler_.set_record_observer(record_observer, this);
    (void)assembler_.begin();
    initialized_ = true;
}

void EdfRecorderManager::poll(uint32_t now_ms) {
    if (!initialized_ || !arbiter_ || !session_) return;
    note_str_get_timeouts(now_ms);
    dispatch_session_edges(now_ms);

    if (status_.enabled && !status_.event_attached) {
        attach_events();
    }

    update_event_coverage();

    if (!status_.enabled || !status_.active) {
        release_stream();
        return;
    }

    if (!numeric_files_open_ &&
        static_cast<int32_t>(now_ms - next_numeric_open_ms_) < 0) {
        release_stream();
        sync_annotation_open_status();
        return;
    }

    sync_annotation_open_status();
    attach_stream(now_ms);
    update_stream_queue_drops();
    if (!numeric_files_open_ && !open_numeric_files_from_stream(now_ms)) {
        return;
    }
    if (!numeric_open_synced_) {
        buffer_numeric_open_stream(now_ms);
    }
    if (!sync_numeric_open_status(now_ms)) return;
    drain_stream(now_ms);
}

void EdfRecorderManager::set_enabled(bool enabled) {
    if (status_.enabled == enabled) return;
    status_.enabled = enabled;
    if (enabled) {
        attach_events();
    } else {
        release_events();
    }
    if (!enabled) {
        if (session_) end_session(session_->status(), millis(), "disabled");
        release_stream();
        return;
    }
    if (session_ && session_->status().state == SessionState::Active) {
        start_session(session_->status(), millis(), "enabled_active");
    }
}

EdfStorageWorkerStatus EdfRecorderManager::storage_status() const {
    return EdfStorageWorker::status();
}

void EdfRecorderManager::handle_event_frame(const As11EventFrame &frame,
                                            uint32_t now_ms) {
    if (!status_.enabled || !status_.active) return;

    status_.event_frames++;
    status_.event_records += frame.event_count;
    status_.last_event_ms = now_ms;
    copy_text(status_.last_event_data_id,
              sizeof(status_.last_event_data_id),
              frame.data_id.c_str());

    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &record = frame.events[i];
        if (edf_event_frame_is_respiratory(frame)) {
            status_.respiratory_events++;
            (void)enqueue_event_annotation(EdfAnnotationKind::Eve, record);
            if (edf_event_record_is_csr(record)) {
                status_.csr_events++;
                (void)enqueue_event_annotation(EdfAnnotationKind::Csl, record);
            }
        }
        copy_text(status_.last_event_name,
                  sizeof(status_.last_event_name),
                  record.name.c_str());
    }
}

void EdfRecorderManager::event_frame_observer(
    void *context,
    const As11EventFrame &frame,
    uint32_t now_ms) {
    static_cast<EdfRecorderManager *>(context)->handle_event_frame(frame,
                                                                   now_ms);
}

void EdfRecorderManager::rpc_event_observer(void *context,
                                            const RpcEvent &event) {
    static_cast<EdfRecorderManager *>(context)->handle_rpc_event(event);
}

void EdfRecorderManager::record_observer(
    void *context,
    const EdfCompletedRecordView &record) {
    static_cast<EdfRecorderManager *>(context)->handle_completed_record(record);
}

void EdfRecorderManager::dispatch_session_edges(uint32_t now_ms) {
    const SessionStatus &session = session_->status();

    if (session.start_count != seen_session_starts_) {
        seen_session_starts_ = session.start_count;
        if (status_.enabled) start_session(session, now_ms, "session_start");
    }

    if (session.end_count != seen_session_ends_) {
        seen_session_ends_ = session.end_count;
        end_session(session, now_ms, "session_end");
    }

    if (status_.enabled && session.state == SessionState::Active &&
        !status_.active &&
        static_cast<int32_t>(now_ms - next_session_start_ms_) >= 0) {
        start_session(session, now_ms, "active_session");
    }
    if (session.state != SessionState::Active && status_.active) {
        end_session(session, now_ms, "session_idle");
    }
}

void EdfRecorderManager::start_session(const SessionStatus &session,
                                       uint32_t now_ms,
                                       const char *reason) {
    if (status_.active && status_.session_id == session.session_id) return;

    release_stream();
    status_.active = true;
    status_.session_id = session.session_id;
    status_.sessions_started++;
    status_.segment_rollovers = 0;
    status_.frames = 0;
    status_.frame_drops = 0;
    status_.event_frames = 0;
    status_.event_records = 0;
    status_.event_subscription_generation = 0;
    status_.event_coverage_gap_count = 0;
    status_.event_coverage_session_gaps = 0;
    status_.event_coverage_uncertain = false;
    status_.respiratory_events = 0;
    status_.csr_events = 0;
    status_.brp_records = 0;
    status_.pld_records = 0;
    status_.sa2_records = 0;
    status_.eve_records = 0;
    status_.csl_records = 0;
    status_.str_records = 0;
    status_.str_setting_requests = 0;
    status_.str_setting_responses = 0;
    status_.str_setting_timeouts = 0;
    status_.str_setting_values = 0;
    status_.str_setting_missing = 0;
    status_.str_setting_unmapped = 0;
    status_.str_summary_requests = 0;
    status_.str_summary_responses = 0;
    status_.str_summary_timeouts = 0;
    status_.str_summary_values = 0;
    status_.str_summary_missing = 0;
    status_.str_summary_unmapped = 0;
    status_.record_enqueue_failures = 0;
    status_.annotation_enqueue_failures = 0;
    status_.str_enqueue_failures = 0;
    status_.file_open_failures = 0;
    status_.numeric_record_drops = 0;
    status_.numeric_start_deferred_frames = 0;
    status_.numeric_start_forced = 0;
    status_.numeric_open_buffered_frames = 0;
    status_.numeric_open_buffer_drops = 0;
    status_.last_frame_ms = 0;
    status_.last_event_ms = 0;
    status_.brp_path[0] = 0;
    status_.pld_path[0] = 0;
    status_.sa2_path[0] = 0;
    status_.eve_path[0] = 0;
    status_.csl_path[0] = 0;
    status_.str_path[0] = 0;
    status_.last_event_data_id[0] = 0;
    status_.last_event_name[0] = 0;
    status_.last_error[0] = 0;
    annotation_start_epoch_ms_ = 0;
    numeric_start_first_frame_epoch_ms_ = 0;
    numeric_start_ready_since_epoch_ms_ = 0;
    numeric_files_open_ = false;
    annotation_open_synced_ = false;
    numeric_open_synced_ = false;
    numeric_segment_day_valid_ = false;
    numeric_segment_day_ = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
    reset_numeric_schemas();
    next_attach_ms_ = now_ms;
    next_numeric_open_ms_ = now_ms;
    str_settings_pending_ = false;
    str_settings_request_id_ = 0;
    str_settings_request_ms_ = 0;
    snapshot_event_coverage();

    Log::logf(CAT_STREAM, LOG_INFO,
              "[EDF] recorder session start id=%lu reason=%s\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--");
}

void EdfRecorderManager::end_session(const SessionStatus &session,
                                     uint32_t now_ms,
                                     const char *reason) {
    (void)now_ms;
    if (!status_.active) return;
    update_event_coverage();

    if (!finish_str_session(session, now_ms)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR session end skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    release_stream();
    assembler_.end_session();
    close_session_files();
    status_.active = false;
    annotation_start_epoch_ms_ = 0;
    numeric_start_first_frame_epoch_ms_ = 0;
    numeric_start_ready_since_epoch_ms_ = 0;
    numeric_open_frame_buffer_.clear();
    status_.sessions_ended++;
    Log::logf(CAT_STREAM, LOG_INFO,
              "[EDF] recorder session end id=%lu reason=%s frames=%lu "
              "drops=%lu events=%lu\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--",
              static_cast<unsigned long>(status_.frames),
              static_cast<unsigned long>(status_.frame_drops),
              static_cast<unsigned long>(status_.event_records));
}

bool EdfRecorderManager::open_session_annotation_files(
    const char *annotation_start_time) {
    if (files_open_) return true;

    int64_t annotation_start_ms = 0;
    if (!edf_parse_utc_ms(annotation_start_time, annotation_start_ms)) {
        status_.file_open_failures++;
        set_error("bad_annotation_epoch");
        return false;
    }

    EdfLocalDateTime start;
    if (!parse_session_local_time(annotation_start_time, start)) {
        status_.file_open_failures++;
        set_error("bad_annotation_time");
        return false;
    }

    return open_session_annotation_files_at(start, annotation_start_ms);
}

bool EdfRecorderManager::open_session_annotation_files_at(
    const EdfLocalDateTime &start,
    int64_t annotation_start_ms) {
    if (files_open_) return true;

    char date[9] = {};
    char time[9] = {};
    if (!edf_header_date(start, date, sizeof(date)) ||
        !edf_header_time(start, time, sizeof(time))) {
        status_.file_open_failures++;
        set_error("bad_edf_time");
        return false;
    }

    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    if (!build_recording_id(start, recording_id, sizeof(recording_id))) {
        status_.file_open_failures++;
        set_error("identity_unavailable");
        return false;
    }
    EdfHeaderInfo info;
    info.patient_id = "AirCANnect";
    info.recording_id = recording_id;
    info.start_date = date;
    info.start_time = time;
    info.record_count = 0;

    const bool eve_open = enqueue_annotation_file_open(EdfAnnotationKind::Eve,
                                                       start, info,
                                                       status_.eve_path,
                                                       sizeof(status_.eve_path));
    if (!eve_open) {
        status_.file_open_failures++;
        return false;
    }
    const bool csl_open = enqueue_annotation_file_open(EdfAnnotationKind::Csl,
                                                       start, info,
                                                       status_.csl_path,
                                                       sizeof(status_.csl_path));
    if (!csl_open) {
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
        status_.file_open_failures++;
        return false;
    }

    files_open_ = true;
    annotation_start_epoch_ms_ =
        edf_floor_epoch_ms_to_second(annotation_start_ms);
    status_.files_open = true;
    return true;
}

bool EdfRecorderManager::build_recording_id(const EdfLocalDateTime &start,
                                            char *dst,
                                            size_t dst_size) const {
    if (!arbiter_) return false;
    const As11DeviceState &state = arbiter_->as11_state();
    if (state.serial_number().empty() || !state.platform_id_valid() ||
        !state.variant_id_valid()) {
        return false;
    }
    return edf_recording_id(start,
                            state.serial_number().c_str(),
                            state.platform_id(),
                            state.variant_id(),
                            dst,
                            dst_size);
}

bool EdfRecorderManager::enqueue_numeric_file_open(
    const EdfFileSchema &schema,
    const EdfLocalDateTime &start,
    const EdfHeaderInfo &info,
    char *path,
    size_t path_size) {
    if (!edf_datalog_path(schema.kind, start, path, path_size)) {
        set_error("path_failed");
        return false;
    }

    if (!EdfStorageWorker::enqueue_open_numeric(path, schema, info)) {
        char error[80] = {};
        snprintf(error, sizeof(error), "open_queue_%s",
                 file_kind_name(schema.kind));
        set_error(error);
        return false;
    }
    return true;
}

void EdfRecorderManager::reset_numeric_schemas() {
    auto reset = [](NumericSchemaState &state) {
        state.open = false;
        edf_reset_numeric_file_layout(state.layout);
    };
    reset(brp_schema_);
    reset(pld_schema_);
    reset(sa2_schema_);
}

bool EdfRecorderManager::build_numeric_schemas() {
    reset_numeric_schemas();
    const char *accepted = arbiter_
                               ? arbiter_->stream_accepted_data_ids_csv().c_str()
                               : "";
    if (!edf_build_numeric_file_layout(EdfFileKind::Brp,
                                       accepted,
                                       brp_schema_.layout) ||
        !edf_build_numeric_file_layout(EdfFileKind::Pld,
                                       accepted,
                                       pld_schema_.layout) ||
        !edf_build_numeric_file_layout(EdfFileKind::Sa2,
                                       accepted,
                                       sa2_schema_.layout)) {
        set_error("numeric_schema_failed");
        return false;
    }
    if (!brp_schema_.layout.enabled && !pld_schema_.layout.enabled &&
        !sa2_schema_.layout.enabled) {
        set_error("no_accepted_numeric_streams");
        return false;
    }
    return true;
}

bool EdfRecorderManager::numeric_stream_ready() const {
    if (!arbiter_ || status_.stream_handle == STREAM_CONSUMER_INVALID) {
        return false;
    }
    if (!arbiter_->stream_consumer_active(status_.stream_handle)) {
        return false;
    }
    if (!arbiter_->stream_actual_active()) {
        return false;
    }
    return arbiter_->stream_accepted_data_ids_cover(DEFAULT_EDF_STREAM_IDS);
}

bool EdfRecorderManager::numeric_start_frame_has_data(
    const StreamFrameData &frame) const {
    const StreamSignalSpan *pressure =
        frame.find_signal(StreamSignalId::MaskPressure);
    const StreamSignalSpan *flow =
        frame.find_signal(StreamSignalId::PatientFlow);
    if (!pressure || !flow || pressure->sample_count == 0 ||
        flow->sample_count == 0) {
        return false;
    }

    const uint16_t samples =
        pressure->sample_count < flow->sample_count
            ? pressure->sample_count
            : flow->sample_count;
    if (samples == 0) return false;

    for (uint16_t sample = 0; sample < samples; ++sample) {
        const size_t pressure_index = pressure->value_offset + sample;
        const size_t flow_index = flow->value_offset + sample;
        if (pressure_index >= frame.value_count ||
            flow_index >= frame.value_count ||
            !frame.value_valid(pressure_index) ||
            !frame.value_valid(flow_index)) {
            return false;
        }
        const float pressure_value = frame.values[pressure_index];
        if (pressure_value <
            AC_EDF_NUMERIC_START_MIN_NONZERO_PRESSURE_CM_H2O) {
            return false;
        }
    }

    return true;
}

bool EdfRecorderManager::numeric_start_frame_ready(
    const StreamFrameData &frame) {
    int64_t frame_start_ms = 0;
    if (!edf_parse_utc_ms(frame.start_time, frame_start_ms)) {
        numeric_start_ready_since_epoch_ms_ = 0;
        return false;
    }
    if (!numeric_start_frame_has_data(frame)) {
        numeric_start_ready_since_epoch_ms_ = 0;
        return false;
    }
    if (numeric_start_ready_since_epoch_ms_ <= 0) {
        numeric_start_ready_since_epoch_ms_ = frame_start_ms;
    }
    return frame_start_ms >= numeric_start_ready_since_epoch_ms_ &&
           frame_start_ms - numeric_start_ready_since_epoch_ms_ >=
               static_cast<int64_t>(AC_EDF_NUMERIC_START_STABLE_MS);
}

bool EdfRecorderManager::numeric_start_wait_expired(
    const StreamFrameData &frame) {
    int64_t frame_start_ms = 0;
    if (!edf_parse_utc_ms(frame.start_time, frame_start_ms)) return false;
    if (numeric_start_first_frame_epoch_ms_ <= 0) {
        numeric_start_first_frame_epoch_ms_ = frame_start_ms;
    }
    return frame_start_ms >= numeric_start_first_frame_epoch_ms_ &&
           frame_start_ms - numeric_start_first_frame_epoch_ms_ >=
               static_cast<int64_t>(AC_EDF_NUMERIC_START_MAX_WAIT_MS);
}

bool EdfRecorderManager::open_numeric_files_from_stream(uint32_t now_ms) {
    if (numeric_files_open_) return true;
    if (static_cast<int32_t>(now_ms - next_numeric_open_ms_) < 0) {
        return false;
    }
    if (!numeric_stream_ready()) {
        return false;
    }

    if (!pending_stream_frame_) {
        if (!arbiter_->next_stream_frame(status_.stream_handle,
                                         pending_stream_frame_)) {
            return false;
        }
    }
    if (!pending_stream_frame_) return false;

    if (!pending_stream_frame_->start_time[0]) {
        pending_stream_frame_.reset();
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        set_error("missing_numeric_start_time");
        return false;
    }

    if (!open_session_annotation_files(pending_stream_frame_->start_time)) {
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return false;
    }

    if (!str_.mask_open() &&
        !begin_str_session(pending_stream_frame_->start_time)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR session start skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    if (!numeric_start_frame_ready(*pending_stream_frame_)) {
        if (!numeric_start_wait_expired(*pending_stream_frame_)) {
            status_.numeric_start_deferred_frames++;
            pending_stream_frame_.reset();
            return false;
        }
        status_.numeric_start_forced++;
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] numeric start forced after startup wait id=%lu\n",
                  static_cast<unsigned long>(status_.session_id));
    }

    return ensure_numeric_files_open(now_ms, pending_stream_frame_->start_time);
}

bool EdfRecorderManager::ensure_numeric_files_open(
    uint32_t now_ms,
    const char *numeric_start_time) {
    if (numeric_files_open_) return true;
    if (static_cast<int32_t>(now_ms - next_numeric_open_ms_) < 0) {
        return true;
    }
    if (!numeric_stream_ready()) {
        return true;
    }

    EdfLocalDateTime numeric_start;
    if (!parse_session_local_time(numeric_start_time, numeric_start)) {
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        set_error("bad_numeric_start_time");
        return false;
    }
    uint16_t numeric_day = 0;
    if (!edf_sleep_day_epoch_days(numeric_start, numeric_day)) {
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        set_error("bad_numeric_sleep_day");
        return false;
    }

    char date[9] = {};
    char time[9] = {};
    if (!edf_header_date(numeric_start, date, sizeof(date)) ||
        !edf_header_time(numeric_start, time, sizeof(time))) {
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        set_error("bad_numeric_edf_time");
        return false;
    }

    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    if (!build_recording_id(numeric_start, recording_id,
                            sizeof(recording_id))) {
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        set_error("numeric_identity_unavailable");
        return false;
    }

    if (!build_numeric_schemas()) {
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return false;
    }

    if (!assembler_.start_session(numeric_start_time)) {
        set_error(assembler_.status().last_error);
        status_.file_open_failures++;
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return false;
    }
    numeric_open_synced_ = false;

    EdfHeaderInfo info;
    info.patient_id = "AirCANnect";
    info.recording_id = recording_id;
    info.start_date = date;
    info.start_time = time;
    info.record_count = 0;

    if (brp_schema_.layout.enabled) {
        if (!enqueue_numeric_file_open(brp_schema_.layout.schema,
                                       numeric_start, info,
                                       status_.brp_path,
                                       sizeof(status_.brp_path))) {
            status_.file_open_failures++;
            next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
            return false;
        }
        brp_schema_.open = true;
    }
    if (pld_schema_.layout.enabled) {
        if (!enqueue_numeric_file_open(pld_schema_.layout.schema,
                                       numeric_start, info,
                                       status_.pld_path,
                                       sizeof(status_.pld_path))) {
            if (brp_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(
                    EdfFileKind::Brp);
                brp_schema_.open = false;
            }
            status_.file_open_failures++;
            next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
            return false;
        }
        pld_schema_.open = true;
    }
    if (sa2_schema_.layout.enabled) {
        if (!enqueue_numeric_file_open(sa2_schema_.layout.schema,
                                       numeric_start, info,
                                       status_.sa2_path,
                                       sizeof(status_.sa2_path))) {
            if (brp_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(
                    EdfFileKind::Brp);
                brp_schema_.open = false;
            }
            if (pld_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(
                    EdfFileKind::Pld);
                pld_schema_.open = false;
            }
            status_.file_open_failures++;
            next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
            return false;
        }
        sa2_schema_.open = true;
    }

    numeric_files_open_ = brp_schema_.open || pld_schema_.open ||
                          sa2_schema_.open;
    numeric_segment_day_ = numeric_day;
    numeric_segment_day_valid_ = numeric_files_open_;
    status_.files_open = files_open_ || numeric_files_open_;
    if (numeric_files_open_) {
        Log::logf(CAT_STREAM, LOG_INFO,
                  "[EDF] numeric files open start=%s accepted=%s brp=%u "
                  "pld=%u sa2=%u\n",
                  numeric_start_time ? numeric_start_time : "--",
                  arbiter_->stream_accepted_data_ids_csv().c_str(),
                  static_cast<unsigned>(
                      brp_schema_.layout.schema.source_signal_count),
                  static_cast<unsigned>(
                      pld_schema_.layout.schema.source_signal_count),
                  static_cast<unsigned>(
                      sa2_schema_.layout.schema.source_signal_count));
    }
    return true;
}

bool EdfRecorderManager::enqueue_annotation_file_open(
    EdfAnnotationKind kind,
    const EdfLocalDateTime &start,
    const EdfHeaderInfo &info,
    char *path,
    size_t path_size) {
    if (!edf_datalog_annotation_path(kind, start, path, path_size)) {
        set_error("annotation_path_failed");
        return false;
    }

    if (!EdfStorageWorker::enqueue_open_annotation(path, kind, info)) {
        char error[80] = {};
        snprintf(error, sizeof(error), "open_queue_%s",
                 annotation_kind_name(kind));
        set_error(error);
        return false;
    }
    return true;
}

void EdfRecorderManager::close_session_files() {
    if (!files_open_ && !numeric_files_open_) {
        status_.files_open = false;
        return;
    }
    if (brp_schema_.open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
    }
    if (pld_schema_.open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
    }
    if (sa2_schema_.open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
    }
    if (files_open_) {
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Csl);
    }
    numeric_files_open_ = false;
    files_open_ = false;
    annotation_open_synced_ = false;
    numeric_open_synced_ = false;
    numeric_segment_day_valid_ = false;
    numeric_segment_day_ = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
    numeric_start_first_frame_epoch_ms_ = 0;
    numeric_start_ready_since_epoch_ms_ = 0;
    status_.files_open = false;
    reset_numeric_schemas();
}

bool EdfRecorderManager::storage_file_matches(
    const EdfStorageOpenFileStatus &file,
    const char *path) const {
    return path && path[0] && file.open && strcmp(file.path, path) == 0;
}

void EdfRecorderManager::sync_annotation_open_status() {
    if (!status_.active || !files_open_ || annotation_open_synced_) return;

    const EdfStorageWorkerStatus storage = EdfStorageWorker::status();
    const EdfStorageOpenFileStatus &eve =
        storage.files[AC_EDF_STORAGE_FILE_EVE];
    const EdfStorageOpenFileStatus &csl =
        storage.files[AC_EDF_STORAGE_FILE_CSL];

    if (storage_file_matches(eve, status_.eve_path) &&
        storage_file_matches(csl, status_.csl_path)) {
        annotation_open_synced_ = true;
        if (status_.eve_records < eve.record_count) {
            status_.eve_records = eve.record_count;
        }
        if (status_.csl_records < csl.record_count) {
            status_.csl_records = csl.record_count;
        }
        return;
    }

    if (storage.queued == 0 && !storage.busy) {
        status_.file_open_failures++;
        set_error(storage.last_error[0] ? storage.last_error
                                        : "annotation_open_failed");
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Csl);
        files_open_ = false;
        annotation_open_synced_ = true;
        status_.files_open = numeric_files_open_;
    }
}

bool EdfRecorderManager::sync_numeric_open_status(uint32_t now_ms) {
    if (!numeric_files_open_) return true;
    if (numeric_open_synced_) return true;

    const EdfStorageWorkerStatus storage = EdfStorageWorker::status();
    const EdfStorageOpenFileStatus &brp =
        storage.files[AC_EDF_STORAGE_FILE_BRP];
    const EdfStorageOpenFileStatus &pld =
        storage.files[AC_EDF_STORAGE_FILE_PLD];
    const EdfStorageOpenFileStatus &sa2 =
        storage.files[AC_EDF_STORAGE_FILE_SA2];

    const bool brp_ready =
        !brp_schema_.open || storage_file_matches(brp, status_.brp_path);
    const bool pld_ready =
        !pld_schema_.open || storage_file_matches(pld, status_.pld_path);
    const bool sa2_ready =
        !sa2_schema_.open || storage_file_matches(sa2, status_.sa2_path);
    if (!brp_ready || !pld_ready || !sa2_ready) {
        if (storage.queued == 0 && !storage.busy) {
            status_.file_open_failures++;
            set_error(storage.last_error[0] ? storage.last_error
                                            : "numeric_open_failed");
            if (brp_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
            }
            if (pld_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
            }
            if (sa2_schema_.open) {
                (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
            }
            numeric_files_open_ = false;
            numeric_open_synced_ = true;
            reset_numeric_schemas();
            status_.files_open = files_open_;
            next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
            release_stream();
        }
        return false;
    }

    const uint32_t brp_records = brp_schema_.open ? brp.record_count : 0;
    const uint32_t pld_records = pld_schema_.open ? pld.record_count : 0;
    const uint32_t sa2_records = sa2_schema_.open ? sa2.record_count : 0;
    assembler_.set_current_records(brp_records, pld_records, sa2_records);
    status_.brp_records = brp_records;
    status_.pld_records = pld_records;
    status_.sa2_records = sa2_records;
    numeric_open_synced_ = true;
    return true;
}

bool EdfRecorderManager::parse_session_local_time(
    const char *text,
    EdfLocalDateTime &out) const {
    if (!text || !text[0]) return false;

    int64_t epoch_ms = 0;
    if (!edf_parse_utc_ms(text, epoch_ms)) return false;
    if (arbiter_) {
        const As11DeviceState &state = arbiter_->as11_state();
        if (state.timezone_offset_valid()) {
            return edf_epoch_ms_to_local_datetime(
                epoch_ms, state.timezone_offset_minutes(), out);
        }
    }
    return edf_epoch_ms_to_configured_local_datetime(epoch_ms, out);
}

bool EdfRecorderManager::begin_str_session(const char *session_start_time) {
    EdfLocalDateTime start;
    if (!parse_session_local_time(session_start_time, start)) {
        set_error("bad_str_start_time");
        return false;
    }

    return begin_str_session_at(start, millis());
}

bool EdfRecorderManager::begin_str_session_at(const EdfLocalDateTime &start,
                                              uint32_t now_ms) {
    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.begin_session(start, str_status)) {
        switch (str_status) {
            case EdfStrSessionStatus::MaskEventsFull:
                set_error("str_mask_events_full");
                break;
            case EdfStrSessionStatus::OffsetError:
                set_error("str_offset_failed");
                break;
            case EdfStrSessionStatus::BadSleepDay:
            default:
                set_error("bad_str_sleep_day");
                break;
        }
        return false;
    }

    (void)request_str_settings(now_ms);
    return true;
}

bool EdfRecorderManager::request_str_settings(uint32_t now_ms) {
    if (!arbiter_ || str_settings_pending_) return false;

    const std::string names = edf_str_setting_get_names();
    if (names.empty()) return false;

    uint32_t id = 0;
    if (!arbiter_->send_request_with_id("Get",
                                        build_get_params(names),
                                        RpcSource::EdfRecorder,
                                        AC_EDF_STR_SETTINGS_TIMEOUT_MS,
                                        id)) {
        set_error("str_settings_queue_failed");
        return false;
    }
    str_settings_pending_ = true;
    str_settings_request_id_ = id;
    str_settings_request_ms_ = now_ms;
    status_.str_setting_requests++;
    return true;
}

bool EdfRecorderManager::request_str_summary(uint32_t now_ms) {
    if (!arbiter_ || str_summary_pending_) return false;

    const std::string names = edf_str_summary_get_names();
    if (names.empty()) return false;

    uint32_t id = 0;
    if (!arbiter_->send_request_with_id("Get",
                                        build_get_params(names),
                                        RpcSource::EdfRecorder,
                                        AC_EDF_STR_SUMMARY_TIMEOUT_MS,
                                        id)) {
        set_error("str_summary_queue_failed");
        return false;
    }
    str_summary_pending_ = true;
    str_summary_request_id_ = id;
    str_summary_request_ms_ = now_ms;
    status_.str_summary_requests++;
    return true;
}

void EdfRecorderManager::note_str_get_timeouts(uint32_t now_ms) {
    if (str_settings_pending_ &&
        static_cast<int32_t>(now_ms - str_settings_request_ms_) >=
            static_cast<int32_t>(AC_EDF_STR_SETTINGS_TIMEOUT_MS + 1000)) {
        str_settings_pending_ = false;
        str_settings_request_id_ = 0;
        str_settings_request_ms_ = 0;
        status_.str_setting_timeouts++;
        set_error("str_settings_timeout");
    }

    if (str_summary_pending_ &&
        static_cast<int32_t>(now_ms - str_summary_request_ms_) >=
            static_cast<int32_t>(AC_EDF_STR_SUMMARY_TIMEOUT_MS + 1000)) {
        str_summary_pending_ = false;
        str_summary_request_id_ = 0;
        str_summary_request_ms_ = 0;
        status_.str_summary_timeouts++;
        set_error("str_summary_timeout");
        if (str_record_pending_write_) {
            str_record_pending_write_ = false;
            (void)write_str_day_record();
        }
    }
}

void EdfRecorderManager::handle_rpc_event(const RpcEvent &event) {
    if (event.kind != RpcEventKind::RpcResponse ||
        event.source != RpcSource::EdfRecorder ||
        !event.payload) {
        return;
    }
    if (str_settings_pending_ && event.id == str_settings_request_id_) {
        str_settings_pending_ = false;
        str_settings_request_id_ = 0;
        str_settings_request_ms_ = 0;
        status_.str_setting_responses++;
        handle_str_settings_response(event.payload_text());
        return;
    }
    if (str_summary_pending_ && event.id == str_summary_request_id_) {
        str_summary_pending_ = false;
        str_summary_request_id_ = 0;
        str_summary_request_ms_ = 0;
        status_.str_summary_responses++;
        handle_str_summary_response(event.payload_text());
        return;
    }
}

void EdfRecorderManager::handle_str_settings_response(
    const std::string &payload) {
    EdfStrSettingsApplyResult result;
    if (!edf_str_apply_settings_response(payload, str_, result)) {
        set_error(result.error);
        return;
    }

    status_.str_setting_values += result.values;
    status_.str_setting_missing += result.missing;
    status_.str_setting_unmapped += result.unmapped;
    Log::logf(CAT_STREAM, LOG_DEBUG,
              "[EDF] STR settings values=%lu missing=%lu unmapped=%lu\n",
              static_cast<unsigned long>(result.values),
              static_cast<unsigned long>(result.missing),
              static_cast<unsigned long>(result.unmapped));

    if (str_.active() && !str_.mask_open() && !str_record_pending_write_ &&
        !str_summary_pending_) {
        (void)write_str_day_record();
    }
}

void EdfRecorderManager::handle_str_summary_response(
    const std::string &payload) {
    EdfStrSettingsApplyResult result;
    if (!edf_str_apply_summary_get_response(payload, str_, result)) {
        set_error(result.error);
    } else {
        status_.str_summary_values += result.values;
        status_.str_summary_missing += result.missing;
        status_.str_summary_unmapped += result.unmapped;
        Log::logf(CAT_STREAM, LOG_DEBUG,
                  "[EDF] STR summary values=%lu missing=%lu unmapped=%lu\n",
                  static_cast<unsigned long>(result.values),
                  static_cast<unsigned long>(result.missing),
                  static_cast<unsigned long>(result.unmapped));
    }

    if (str_record_pending_write_) {
        str_record_pending_write_ = false;
        (void)write_str_day_record();
    }
}

bool EdfRecorderManager::finish_str_session(const SessionStatus &session,
                                            uint32_t now_ms) {
    if (!str_.active() || !str_.mask_open()) return true;

    EdfLocalDateTime end;
    if (!parse_session_local_time(session.end_device_time, end) &&
        !parse_session_local_time(session.last_stream_start_time, end) &&
        !parse_session_local_time(session.start_device_time, end)) {
        set_error("bad_str_end_time");
        return false;
    }

    uint16_t end_day = 0;
    uint16_t end_minute = 0;
    EdfLocalDateTime end_day_start;
    if (!edf_sleep_day_epoch_days(end, end_day) ||
        !edf_sleep_day_minute(end, end_minute) ||
        !edf_sleep_day_start(end, end_day_start)) {
        set_error("bad_str_end_sleep_day");
        return false;
    }

    if (end_day != str_.day_epoch_days()) {
        if (!finish_str_session_at(end_day_start, now_ms, false)) {
            return false;
        }
        if (end_minute == 0) return true;
        if (!begin_str_session_at(end_day_start, now_ms)) {
            return false;
        }
    }

    return finish_str_session_at(end, now_ms, true);
}

bool EdfRecorderManager::finish_str_session_at(const EdfLocalDateTime &end,
                                               uint32_t now_ms,
                                               bool request_summary) {
    if (!str_.active() || !str_.mask_open()) return true;

    bool record_ready = false;
    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.finish_session(end, record_ready, str_status)) {
        switch (str_status) {
            case EdfStrSessionStatus::MaskEventsFull:
                set_error("str_mask_events_full");
                break;
            case EdfStrSessionStatus::OffsetError:
                set_error("str_offset_failed");
                break;
            case EdfStrSessionStatus::BadSleepDay:
            default:
                set_error("bad_str_end_sleep_day");
                break;
        }
        return false;
    }
    if (!record_ready) return true;

    str_record_pending_write_ = true;
    if (request_summary &&
        (str_summary_pending_ || request_str_summary(now_ms))) {
        return true;
    }

    str_record_pending_write_ = false;
    return write_str_day_record();
}

bool EdfRecorderManager::write_str_day_record() {
    char path[sizeof(status_.str_path)] = {};
    if (!edf_str_path(path, sizeof(path))) {
        set_error("str_path_failed");
        return false;
    }

    char date[9] = {};
    char time[9] = {};
    if (!edf_header_date(str_.day_start(), date, sizeof(date)) ||
        !edf_header_time(str_.day_start(), time, sizeof(time))) {
        set_error("bad_str_header_time");
        return false;
    }

    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    if (!build_recording_id(str_.day_start(), recording_id,
                            sizeof(recording_id))) {
        set_error("str_identity_unavailable");
        return false;
    }

    EdfHeaderInfo info;
    info.patient_id = "AirCANnect";
    info.recording_id = recording_id;
    info.start_date = date;
    info.start_time = time;
    info.record_count = 0;

    EdfStrRecordView record;
    record.digital_samples = str_.samples();
    record.sample_count = str_.sample_count();
    if (!EdfStorageWorker::enqueue_str_record(path, info, record)) {
        status_.str_enqueue_failures++;
        set_error("str_queue_failed");
        return false;
    }

    copy_text(status_.str_path, sizeof(status_.str_path), path);
    status_.str_records++;
    return true;
}

void EdfRecorderManager::attach_events() {
    if (!arbiter_) {
        status_.event_attached = false;
        status_.event_handle = EVENT_CONSUMER_INVALID;
        return;
    }
    if (status_.event_handle != EVENT_CONSUMER_INVALID &&
        arbiter_->event_consumer_active(status_.event_handle)) {
        status_.event_attached =
            status_.event_handle != EVENT_CONSUMER_INVALID;
        return;
    }

    EventAcquireResult result =
        arbiter_->acquire_events(EDF_CAPTURE_EVENT_IDS);
    if (result.status == EventAcquireStatus::Acquired ||
        result.status == EventAcquireStatus::AlreadyActive) {
        status_.event_handle = result.handle;
        status_.event_attached = true;
        return;
    }

    status_.event_handle = EVENT_CONSUMER_INVALID;
    status_.event_attached = false;
    set_error("event_attach_failed");
}

void EdfRecorderManager::release_events() {
    if (!arbiter_ || status_.event_handle == EVENT_CONSUMER_INVALID) {
        status_.event_attached = false;
        status_.event_handle = EVENT_CONSUMER_INVALID;
        return;
    }
    arbiter_->release_events(status_.event_handle);
    status_.event_handle = EVENT_CONSUMER_INVALID;
    status_.event_attached = false;
}

void EdfRecorderManager::snapshot_event_coverage() {
    if (!arbiter_) {
        session_event_subscription_generation_ = 0;
        session_event_coverage_gap_count_ = 0;
        status_.event_subscription_generation = 0;
        status_.event_coverage_gap_count = 0;
        status_.event_coverage_session_gaps = 0;
        status_.event_coverage_uncertain = true;
        return;
    }

    const EventBrokerStatus event = arbiter_->event_broker().status();
    session_event_subscription_generation_ = event.subscription_generation;
    session_event_coverage_gap_count_ = event.coverage_gap_count;
    status_.event_subscription_generation = event.subscription_generation;
    status_.event_coverage_gap_count = event.coverage_gap_count;
    status_.event_coverage_session_gaps = 0;
    status_.event_coverage_uncertain =
        !status_.event_attached || !event.subscription_active;
}

void EdfRecorderManager::update_event_coverage() {
    if (!status_.active || !arbiter_) return;

    const EventBrokerStatus event = arbiter_->event_broker().status();
    status_.event_subscription_generation = event.subscription_generation;
    status_.event_coverage_gap_count = event.coverage_gap_count;
    status_.event_coverage_session_gaps =
        event.coverage_gap_count >= session_event_coverage_gap_count_
            ? event.coverage_gap_count - session_event_coverage_gap_count_
            : event.coverage_gap_count;

    if (!status_.event_attached || !event.subscription_active ||
        event.coverage_gap_count != session_event_coverage_gap_count_ ||
        event.subscription_generation !=
            session_event_subscription_generation_) {
        status_.event_coverage_uncertain = true;
    }
}

void EdfRecorderManager::attach_stream(uint32_t now_ms) {
    if (status_.stream_handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(status_.stream_handle)) {
        status_.stream_attached = true;
        return;
    }

    status_.stream_handle = STREAM_CONSUMER_INVALID;
    status_.stream_attached = false;
    if (static_cast<int32_t>(now_ms - next_attach_ms_) < 0) return;

    next_attach_ms_ = now_ms + AC_EDF_ATTACH_RETRY_MS;
    status_.attach_attempts++;

    const std::string params = build_stream_params(DEFAULT_EDF_STREAM_IDS,
                                                   40,
                                                   200);
    StreamAcquireResult result =
        arbiter_->acquire_stream(params, RpcSource::EdfRecorder);
    if (result.status == StreamAcquireStatus::Acquired ||
        result.status == StreamAcquireStatus::AlreadyActive) {
        status_.stream_handle = result.handle;
        status_.stream_attached = true;
        last_queue_drops_ = 0;
        status_.last_error[0] = 0;
        return;
    }

    status_.attach_failures++;
    set_error(acquire_status_name(result.status));
}

void EdfRecorderManager::release_stream() {
    if (!arbiter_) return;
    if (status_.stream_handle != STREAM_CONSUMER_INVALID &&
        arbiter_->stream_consumer_active(status_.stream_handle)) {
        arbiter_->release_stream(status_.stream_handle);
    }
    status_.stream_handle = STREAM_CONSUMER_INVALID;
    status_.stream_attached = false;
    last_queue_drops_ = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
}

void EdfRecorderManager::update_stream_queue_drops() {
    if (status_.stream_handle == STREAM_CONSUMER_INVALID ||
        !arbiter_->stream_consumer_active(status_.stream_handle)) {
        status_.stream_attached = false;
        return;
    }

    const uint32_t queue_drops =
        arbiter_->stream_consumer_queue_drops(status_.stream_handle);
    if (queue_drops < last_queue_drops_) {
        last_queue_drops_ = queue_drops;
    } else if (queue_drops != last_queue_drops_) {
        const uint32_t delta = queue_drops - last_queue_drops_;
        last_queue_drops_ = queue_drops;
        status_.frame_drops += delta;
    }
}

void EdfRecorderManager::buffer_numeric_open_stream(uint32_t now_ms) {
    if (status_.stream_handle == STREAM_CONSUMER_INVALID ||
        !arbiter_->stream_consumer_active(status_.stream_handle)) {
        status_.stream_attached = false;
        return;
    }

    update_stream_queue_drops();

    for (size_t i = 0; i < AC_EDF_NUMERIC_OPEN_FRAME_BUFFER_DEPTH; ++i) {
        StreamFrameRef frame;
        if (!arbiter_->next_stream_frame(status_.stream_handle, frame)) {
            break;
        }
        if (!frame) continue;
        if (!numeric_open_frame_buffer_.push(std::move(frame))) {
            status_.numeric_open_buffer_drops++;
            status_.frame_drops++;
            break;
        }
        status_.numeric_open_buffered_frames++;
        status_.last_frame_ms = now_ms;
    }
}

void EdfRecorderManager::drain_stream(uint32_t now_ms) {
    if (status_.stream_handle == STREAM_CONSUMER_INVALID ||
        !arbiter_->stream_consumer_active(status_.stream_handle)) {
        status_.stream_attached = false;
        return;
    }

    update_stream_queue_drops();

    for (size_t i = 0; i < AC_EDF_STREAM_FRAME_BUDGET; ++i) {
        StreamFrameRef frame = pending_stream_frame_;
        if (frame) {
            pending_stream_frame_.reset();
        } else if (numeric_open_frame_buffer_.pop(frame)) {
        } else if (!arbiter_->next_stream_frame(status_.stream_handle, frame)) {
            break;
        }
        if (!frame) continue;
        if (!roll_segment_if_needed(*frame, now_ms)) {
            pending_stream_frame_ = frame;
            break;
        }
        if (!numeric_open_synced_) {
            pending_stream_frame_ = frame;
            break;
        }
        const EdfFramePrepareStatus prepared =
            assembler_.prepare_frame(*frame, AC_EDF_GAP_RECORD_BUDGET);
        if (prepared == EdfFramePrepareStatus::Deferred) {
            pending_stream_frame_ = frame;
            break;
        }
        if (prepared == EdfFramePrepareStatus::Rejected) {
            status_.last_frame_ms = now_ms;
            continue;
        }
        assembler_.ingest_frame(*frame);
        status_.frames++;
        status_.last_frame_ms = now_ms;
    }
}

bool EdfRecorderManager::frame_sleep_day(const StreamFrameData &frame,
                                         EdfLocalDateTime &local,
                                         uint16_t &sleep_day) const {
    if (!frame.start_time[0] || !parse_session_local_time(frame.start_time,
                                                          local)) {
        return false;
    }
    return edf_sleep_day_epoch_days(local, sleep_day);
}

bool EdfRecorderManager::sleep_day_boundary_epoch_ms(
    const StreamFrameData &frame,
    const EdfLocalDateTime &local,
    int64_t &boundary_ms) const {
    int64_t frame_ms = 0;
    uint16_t minute = 0;
    if (!edf_parse_utc_ms(frame.start_time, frame_ms) ||
        !edf_sleep_day_minute(local, minute)) {
        return false;
    }

    int64_t millisecond = frame_ms % 1000;
    if (millisecond < 0) millisecond += 1000;
    const int64_t elapsed_ms =
        static_cast<int64_t>(minute) * 60 * 1000 +
        static_cast<int64_t>(local.second) * 1000 +
        millisecond;
    boundary_ms = frame_ms - elapsed_ms;
    return true;
}

bool EdfRecorderManager::roll_segment_if_needed(
    const StreamFrameData &frame,
    uint32_t now_ms) {
    if (!numeric_files_open_ || !numeric_open_synced_ ||
        !numeric_segment_day_valid_) {
        return true;
    }

    EdfLocalDateTime frame_local;
    uint16_t frame_day = 0;
    if (!frame_sleep_day(frame, frame_local, frame_day)) {
        return true;
    }
    if (frame_day == numeric_segment_day_) return true;
    if (frame_day < numeric_segment_day_) return true;

    EdfLocalDateTime boundary;
    if (!edf_sleep_day_start(frame_local, boundary)) {
        set_error("bad_segment_boundary");
        return false;
    }
    int64_t boundary_ms = 0;
    if (!sleep_day_boundary_epoch_ms(frame, frame_local, boundary_ms)) {
        set_error("bad_segment_boundary_epoch");
        return false;
    }

    Log::logf(CAT_STREAM, LOG_INFO,
              "[EDF] segment rollover old_day=%u new_day=%u frame=%s\n",
              static_cast<unsigned>(numeric_segment_day_),
              static_cast<unsigned>(frame_day),
              frame.start_time);

    if (!finish_str_session_at(boundary, now_ms, false)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR rollover finish skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    assembler_.end_session();
    close_session_files();
    status_.segment_rollovers++;

    if (!open_session_annotation_files_at(boundary, boundary_ms)) {
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return false;
    }

    if (!begin_str_session_at(boundary, now_ms)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR rollover start skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    return ensure_numeric_files_open(now_ms, frame.start_time);
}

void EdfRecorderManager::handle_completed_record(
    const EdfCompletedRecordView &record) {
    const NumericSchemaState *state = nullptr;
    switch (record.series) {
        case EdfSeriesId::Brp:
            state = &brp_schema_;
            break;
        case EdfSeriesId::Pld:
            state = &pld_schema_;
            break;
        case EdfSeriesId::Sa2:
            state = &sa2_schema_;
            break;
    }
    bool enqueued = false;
    if (numeric_files_open_ && state && state->open) {
        if (EdfStorageWorker::enqueue_numeric_record(state->layout.schema,
                                                     record)) {
            enqueued = true;
        } else {
            status_.record_enqueue_failures++;
            set_error("record_queue_failed");
        }
    } else {
        status_.numeric_record_drops++;
    }

    if (!enqueued) return;

    switch (record.series) {
        case EdfSeriesId::Brp:
            status_.brp_records++;
            break;
        case EdfSeriesId::Pld:
            status_.pld_records++;
            break;
        case EdfSeriesId::Sa2:
            status_.sa2_records++;
            break;
    }
}

bool EdfRecorderManager::enqueue_event_annotation(
    EdfAnnotationKind kind,
    const As11EventRecord &record) {
    if (!files_open_ || annotation_start_epoch_ms_ <= 0) return false;

    EdfAnnotationRecord annotation;
    EdfEventAnnotationResult result;
    if (!edf_build_event_annotation(kind, record, annotation_start_epoch_ms_,
                                    annotation, result)) {
        if (result.status != EdfEventAnnotationStatus::TimeError) {
            return false;
        }
        status_.annotation_enqueue_failures++;
        set_error(result.error);
        return false;
    }
    if (!EdfStorageWorker::enqueue_annotation_record(kind, annotation)) {
        status_.annotation_enqueue_failures++;
        set_error("event_queue_failed");
        return false;
    }

    if (kind == EdfAnnotationKind::Eve) {
        status_.eve_records++;
    } else if (kind == EdfAnnotationKind::Csl) {
        status_.csl_records++;
    }
    return true;
}

void EdfRecorderManager::set_error(const char *error) {
    copy_text(status_.last_error, sizeof(status_.last_error), error);
}

void EdfRecorderManager::copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

}  // namespace aircannect
