#include "edf_recorder_manager.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "as11_rpc.h"
#include "debug_log.h"
#include "edf_event_annotation.h"
#include "edf_identification.h"
#include "edf_numeric_file_layout.h"
#include "edf_storage_catalog.h"
#include "storage_service.h"
#include "edf_stream_signal_table.h"
#include "edf_str_settings.h"
#include "edf_time.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t AC_EDF_STR_SETTINGS_TIMEOUT_MS = 12000;
static constexpr uint32_t AC_EDF_STR_SUMMARY_TIMEOUT_MS = 12000;
static constexpr uint32_t AC_EDF_IDENTIFICATION_TIMEOUT_MS = 12000;
static constexpr size_t AC_EDF_GAP_RECORD_BUDGET = 3;

const char *const EDF_RECORDING_GATE_DATA_ID = "_ZLE";
const char *const EDF_MASK_ON_EVENT = "MaskOn";
const char *const EDF_MASK_OFF_EVENT = "MaskOff";
const char *const EDF_CAPTURE_EVENT_IDS =
    "_ZLE,TherapyEvents-RespiratoryEvents";

struct StatusCarryover {
    bool enabled = false;
    bool rpc_observer_registered = false;
    bool event_observer_registered = false;
    bool event_attached = false;
    EventConsumerHandle event_handle = EVENT_CONSUMER_INVALID;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t attach_attempts = 0;
    uint32_t attach_failures = 0;
    uint32_t recording_gate_rises = 0;
    uint32_t recording_gate_falls = 0;
    uint32_t recording_gate_recoveries = 0;
    uint32_t recording_gate_bad_events = 0;
    uint32_t mask_events = 0;
    uint32_t mask_bad_events = 0;
};

StatusCarryover preserve_status_carryover(
    const EdfRecorderStatus &status) {
    StatusCarryover out;
    out.enabled = status.enabled;
    out.rpc_observer_registered = status.rpc_observer_registered;
    out.event_observer_registered = status.event_observer_registered;
    out.event_attached = status.event_attached;
    out.event_handle = status.event_handle;
    out.sessions_started = status.sessions_started;
    out.sessions_ended = status.sessions_ended;
    out.attach_attempts = status.attach_attempts;
    out.attach_failures = status.attach_failures;
    out.recording_gate_rises = status.recording_gate_rises;
    out.recording_gate_falls = status.recording_gate_falls;
    out.recording_gate_recoveries = status.recording_gate_recoveries;
    out.recording_gate_bad_events = status.recording_gate_bad_events;
    out.mask_events = status.mask_events;
    out.mask_bad_events = status.mask_bad_events;
    return out;
}

void restore_status_carryover(EdfRecorderStatus &status,
                              const StatusCarryover &carryover) {
    status.enabled = carryover.enabled;
    status.rpc_observer_registered = carryover.rpc_observer_registered;
    status.event_observer_registered = carryover.event_observer_registered;
    status.event_attached = carryover.event_attached;
    status.event_handle = carryover.event_handle;
    status.sessions_started = carryover.sessions_started;
    status.sessions_ended = carryover.sessions_ended;
    status.attach_attempts = carryover.attach_attempts;
    status.attach_failures = carryover.attach_failures;
    status.recording_gate_rises = carryover.recording_gate_rises;
    status.recording_gate_falls = carryover.recording_gate_falls;
    status.recording_gate_recoveries = carryover.recording_gate_recoveries;
    status.recording_gate_bad_events = carryover.recording_gate_bad_events;
    status.mask_events = carryover.mask_events;
    status.mask_bad_events = carryover.mask_bad_events;
}

const char *open_result_error(const EdfStorageOpenResult &result,
                              const char *fallback) {
    if (result.error[0]) return result.error;
    if (result.superseded) return "open_superseded";
    return fallback;
}

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

bool edf_event_record_is_mask_event(const As11EventFrame &frame,
                                    const As11EventRecord &record) {
    if (!as11_event_data_id_is_activity(frame.data_id) ||
        record.report_time.empty()) {
        return false;
    }
    return record.name == EDF_MASK_ON_EVENT ||
           record.name == EDF_MASK_OFF_EVENT;
}

}  // namespace

void EdfRecorderManager::begin(RpcArbiter &arbiter,
                               const As11DeviceState &device_state,
                               SessionManager &session) {
    if (initialized_) return;
    arbiter_ = &arbiter;
    device_state_ = &device_state;
    session_ = &session;
    // EdfRecorderManager is a program-lifetime singleton; these observer hooks
    // intentionally stay registered until reboot.
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
    if (!initialized_ || !arbiter_ || !device_state_ || !session_) return;
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

    if (str_start_pending_) {
        (void)ensure_str_session_started(now_ms);
    }

    if (annotation_open_pending_) {
        (void)ensure_annotation_files_open(now_ms);
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

EdfRecorderStatus EdfRecorderManager::status() const {
    EdfRecorderStatus snapshot = status_;
    snapshot.annotation_files_open = files_open_;
    snapshot.numeric_files_open = numeric_files_open_;
    snapshot.recording_gate_is_open = recording_gate_open_;
    snapshot.recording_gate_is_closed = recording_gate_closed_;
    snapshot.recording_gate_recovery_is_pending =
        recording_gate_recovery_pending_;
    snapshot.annotation_open_is_pending = annotation_open_pending_;
    snapshot.event_coverage_session_gap_count =
        event_coverage_session_gaps();
    return snapshot;
}

StorageServiceStatus EdfRecorderManager::storage_status() const {
    return StorageService::status();
}

bool EdfRecorderManager::handle_recording_gate_frame(
    const As11EventFrame &frame,
    uint32_t now_ms) {
    if (frame.data_id != EDF_RECORDING_GATE_DATA_ID) return false;

    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &record = frame.events[i];
        int32_t gate_value = 0;
        if (!as11_event_record_value_change(record, gate_value) ||
            record.report_time.empty()) {
            status_.recording_gate_bad_events++;
            continue;
        }

        if (gate_value != 0) {
            status_.recording_gate_rises++;
            if (!status_.active && session_ &&
                session_->status().state == SessionState::Active) {
                start_session(session_->status(), now_ms, "zle_rise");
            }
            if (status_.active) {
                begin_recording_gate(record.report_time.c_str(), now_ms);
            }
        } else {
            status_.recording_gate_falls++;
            close_recording_gate(record.report_time.c_str(), now_ms);
        }
    }
    return true;
}

bool EdfRecorderManager::handle_mask_event_frame(const As11EventFrame &frame,
                                                 uint32_t now_ms) {
    if (!as11_event_data_id_is_activity(frame.data_id)) return false;

    bool handled = false;
    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &record = frame.events[i];
        if (!edf_event_record_is_mask_event(frame, record)) continue;
        handled = true;
        status_.mask_events++;
        if (record.name == EDF_MASK_ON_EVENT) {
            begin_mask_event(record.report_time.c_str(), now_ms);
        } else {
            finish_mask_event(record.report_time.c_str(), now_ms);
        }
    }
    return handled;
}

void EdfRecorderManager::begin_mask_event(const char *start_time,
                                          uint32_t now_ms) {
    if (!start_time || !start_time[0]) {
        status_.mask_bad_events++;
        return;
    }

    if (!status_.active) {
        copy_cstr(pending_mask_event_start_time_,
                  sizeof(pending_mask_event_start_time_),
                  start_time);
        return;
    }

    EdfLocalDateTime start;
    if (!parse_session_local_time(start_time, start)) {
        status_.mask_bad_events++;
        set_error("bad_mask_on_time");
        return;
    }

    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.begin_mask_event(start, str_status)) {
        status_.mask_bad_events++;
        switch (str_status) {
            case EdfStrSessionStatus::MaskEventsFull:
                set_error("str_mask_events_full");
                break;
            case EdfStrSessionStatus::OffsetError:
                set_error("str_offset_failed");
                break;
            case EdfStrSessionStatus::BadSleepDay:
            default:
                set_error("bad_mask_on_sleep_day");
                break;
        }
        return;
    }

    copy_cstr(status_.last_mask_event_time,
              sizeof(status_.last_mask_event_time),
              start_time);
}

void EdfRecorderManager::finish_mask_event(const char *end_time,
                                           uint32_t now_ms) {
    (void)now_ms;

    if (!end_time || !end_time[0]) {
        status_.mask_bad_events++;
        return;
    }
    if (!str_.active()) return;

    EdfLocalDateTime end;
    if (!parse_session_local_time(end_time, end)) {
        status_.mask_bad_events++;
        set_error("bad_mask_off_time");
        return;
    }

    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.finish_mask_event(end, str_status)) {
        status_.mask_bad_events++;
        switch (str_status) {
            case EdfStrSessionStatus::MaskEventsFull:
                set_error("str_mask_events_full");
                break;
            case EdfStrSessionStatus::OffsetError:
                set_error("str_offset_failed");
                break;
            case EdfStrSessionStatus::BadSleepDay:
            default:
                set_error("bad_mask_off_sleep_day");
                break;
        }
        return;
    }

    copy_cstr(status_.last_mask_event_time,
              sizeof(status_.last_mask_event_time),
              end_time);

    if (!str_.therapy_open()) {
        if (str_summary_rpc_.active) {
            str_record_pending_write_ = true;
        } else {
            str_record_pending_write_ = false;
            (void)write_str_day_record();
        }
    }
}

bool EdfRecorderManager::ensure_annotation_files_open(uint32_t now_ms) {
    if (!status_.active || !status_.recording_start_time[0]) return false;
    if (!annotation_open_pending_ && files_open_) return true;
    if (static_cast<int32_t>(now_ms - next_annotation_open_ms_) < 0) {
        return false;
    }
    if (!as11_timezone_ready()) {
        next_annotation_open_ms_ = now_ms + AC_EDF_ATTACH_RETRY_MS;
        set_error("timezone_unavailable");
        return false;
    }

    bool ok = true;
    if (!open_session_annotation_files(status_.recording_start_time)) {
        ok = false;
    }
    if (!ok) {
        next_annotation_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return false;
    }

    annotation_open_pending_ = false;
    status_.last_error[0] = 0;
    return true;
}

void EdfRecorderManager::begin_recording_gate(const char *start_time,
                                              uint32_t now_ms) {
    if (!start_time || !start_time[0]) {
        status_.recording_gate_bad_events++;
        return;
    }

    recording_gate_open_ = true;
    recording_gate_closed_ = false;
    copy_cstr(status_.recording_start_time,
              sizeof(status_.recording_start_time),
              start_time);
    status_.recording_end_time[0] = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
    next_numeric_open_ms_ = now_ms;
    annotation_open_pending_ = true;
    next_annotation_open_ms_ = now_ms;
    (void)ensure_annotation_files_open(now_ms);
}

void EdfRecorderManager::close_recording_gate(const char *end_time,
                                              uint32_t now_ms) {
    if (!end_time || !end_time[0]) {
        status_.recording_gate_bad_events++;
        return;
    }

    recording_gate_open_ = false;
    recording_gate_closed_ = true;
    copy_cstr(status_.recording_end_time,
              sizeof(status_.recording_end_time),
              end_time);

    if (status_.active && session_) {
        end_session(session_->status(), now_ms, "zle_fall", end_time);
    }
}

void EdfRecorderManager::handle_event_frame(const As11EventFrame &frame,
                                            uint32_t now_ms) {
    if (!status_.enabled) return;

    (void)handle_mask_event_frame(frame, now_ms);
    const bool recording_gate_frame =
        handle_recording_gate_frame(frame, now_ms);
    if (!status_.active) return;

    status_.event_frames++;
    status_.event_records += frame.event_count;
    status_.last_event_ms = now_ms;
    copy_cstr(status_.last_event_data_id,
              sizeof(status_.last_event_data_id),
              frame.data_id.c_str());

    for (size_t i = 0; i < frame.event_count; ++i) {
        const As11EventRecord &record = frame.events[i];
        if (!recording_gate_frame &&
            !edf_event_record_is_mask_event(frame, record) &&
            edf_event_frame_is_respiratory(frame)) {
            status_.respiratory_events++;
            (void)enqueue_event_annotation(EdfAnnotationKind::Eve, record);
            if (edf_event_record_is_csr(record)) {
                status_.csr_events++;
                (void)enqueue_event_annotation(EdfAnnotationKind::Csl, record);
            }
        }
        copy_cstr(status_.last_event_name,
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
        !recording_gate_closed_ &&
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

    if (!flush_pending_str_record(reason ? reason : "session_start")) {
        Log::logf(CAT_EDF, LOG_WARN,
                  "pending STR record flush failed before session start "
                  "id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    release_stream();
    const StatusCarryover carryover = preserve_status_carryover(status_);

    status_ = {};
    restore_status_carryover(status_, carryover);
    status_.stream_handle = STREAM_CONSUMER_INVALID;
    status_.active = true;
    status_.session_id = session.session_id;
    status_.sessions_started++;
    annotation_start_epoch_ms_ = 0;
    next_annotation_open_ms_ = now_ms;
    recording_gate_open_ = false;
    recording_gate_closed_ = false;
    recording_gate_recovery_pending_ =
        session.recovered_active_start ||
        (reason && (strcmp(reason, "enabled_active") == 0 ||
                    strcmp(reason, "active_session") == 0));
    numeric_files_open_ = false;
    annotation_open_synced_ = false;
    numeric_open_synced_ = false;
    numeric_segment_day_ = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
    reset_numeric_schemas();
    next_attach_ms_ = now_ms;
    next_numeric_open_ms_ = now_ms;
    str_settings_rpc_.clear();
    str_summary_rpc_.clear();
    identification_rpc_.clear();
    str_start_pending_ = false;
    pending_str_start_time_[0] = 0;
    snapshot_event_coverage();

    begin_str_session(session.start_device_time, now_ms);
    if (pending_mask_event_start_time_[0]) {
        begin_mask_event(pending_mask_event_start_time_, now_ms);
        pending_mask_event_start_time_[0] = 0;
    }

    Log::logf(CAT_EDF, LOG_DEBUG,
              "recorder session start id=%lu reason=%s\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--");
}

void EdfRecorderManager::end_session(const SessionStatus &session,
                                     uint32_t now_ms,
                                     const char *reason,
                                     const char *recording_end_time) {
    if (!status_.active) return;
    update_event_coverage();

    if (!finish_str_session(session, now_ms, recording_end_time)) {
        Log::logf(CAT_EDF, LOG_WARN,
                  "STR session end skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    release_stream();
    assembler_.end_session();
    close_session_files();
    status_.active = false;
    annotation_start_epoch_ms_ = 0;
    next_annotation_open_ms_ = 0;
    recording_gate_open_ = false;
    recording_gate_recovery_pending_ = false;
    annotation_open_pending_ = false;
    str_start_pending_ = false;
    pending_str_start_time_[0] = 0;
    pending_mask_event_start_time_[0] = 0;
    numeric_open_frame_buffer_.clear();
    status_.sessions_ended++;
    const uint32_t enqueue_failures =
        status_.record_enqueue_failures +
        status_.annotation_enqueue_failures +
        status_.str_enqueue_failures;
    const uint32_t rpc_failures =
        status_.str_setting_timeouts +
        status_.str_summary_timeouts +
        status_.identification_timeouts +
        status_.identification_failures;
    const bool had_failures =
        status_.frame_drops != 0 ||
        status_.numeric_record_drops != 0 ||
        status_.numeric_open_buffer_drops != 0 ||
        enqueue_failures != 0 ||
        status_.file_open_failures != 0 ||
        status_.event_coverage_session_gap_count != 0 ||
        rpc_failures != 0;
    Log::logf(CAT_EDF, had_failures ? LOG_WARN : LOG_DEBUG,
              "recorder session end id=%lu reason=%s frames=%lu "
              "drops=%lu numeric_drops=%lu enqueue_failures=%lu "
              "open_failures=%lu event_gaps=%lu rpc_failures=%lu events=%lu\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--",
              static_cast<unsigned long>(status_.frames),
              static_cast<unsigned long>(status_.frame_drops),
              static_cast<unsigned long>(status_.numeric_record_drops),
              static_cast<unsigned long>(enqueue_failures),
              static_cast<unsigned long>(status_.file_open_failures),
              static_cast<unsigned long>(
                  status_.event_coverage_session_gap_count),
              static_cast<unsigned long>(rpc_failures),
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
                                                       sizeof(status_.eve_path),
                                                       eve_open_handle_);
    if (!eve_open) {
        status_.file_open_failures++;
        return false;
    }
    const bool csl_open = enqueue_annotation_file_open(EdfAnnotationKind::Csl,
                                                       start, info,
                                                       status_.csl_path,
                                                       sizeof(status_.csl_path),
                                                       csl_open_handle_);
    if (!csl_open) {
        (void)StorageService::enqueue_edf_close_annotation(
            EdfAnnotationKind::Eve);
        eve_open_handle_ = {};
        status_.file_open_failures++;
        return false;
    }

    files_open_ = true;
    annotation_start_epoch_ms_ =
        edf_floor_epoch_ms_to_second(annotation_start_ms);
    return true;
}

bool EdfRecorderManager::build_recording_id(const EdfLocalDateTime &start,
                                            char *dst,
                                            size_t dst_size) const {
    if (!arbiter_) return false;
    const As11DeviceState &state = *device_state_;
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
    size_t path_size,
    EdfStorageOpenHandle &handle) {
    handle = {};
    if (!edf_datalog_path(schema.kind, start, path, path_size)) {
        set_error("path_failed");
        return false;
    }

    if (!StorageService::enqueue_edf_open_numeric(path, schema, info,
                                                  &handle)) {
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
        state.open_handle = {};
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

bool EdfRecorderManager::open_numeric_files_from_stream(uint32_t now_ms) {
    if (numeric_files_open_) return true;
    if (static_cast<int32_t>(now_ms - next_numeric_open_ms_) < 0) {
        return false;
    }
    if (!numeric_stream_ready()) {
        return false;
    }

    if (!recording_gate_open_ || !status_.recording_start_time[0]) {
        if (!recording_gate_recovery_pending_) {
            StreamFrameRef discarded;
            while (arbiter_->next_stream_frame(status_.stream_handle,
                                               discarded)) {
            }
            pending_stream_frame_.reset();
            return false;
        }

        if (!pending_stream_frame_ &&
            !arbiter_->next_stream_frame(status_.stream_handle,
                                         pending_stream_frame_)) {
            return false;
        }
        if (!pending_stream_frame_) return false;
        if (!pending_stream_frame_->start_time[0]) {
            pending_stream_frame_.reset();
            status_.file_open_failures++;
            next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
            set_error("missing_recovery_start_time");
            return false;
        }

        StreamFrameRef recovery_frame = pending_stream_frame_;
        recording_gate_recovery_pending_ = false;
        status_.recording_gate_recoveries++;
        begin_recording_gate(recovery_frame->start_time, now_ms);
        pending_stream_frame_ = recovery_frame;
        Log::logf(CAT_EDF, LOG_INFO,
                  "recording gate recovered id=%lu start=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.recording_start_time[0]
                      ? status_.recording_start_time
                      : "--");
    }

    if (!ensure_annotation_files_open(now_ms)) {
        next_numeric_open_ms_ = now_ms + AC_EDF_ATTACH_RETRY_MS;
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

    return ensure_numeric_files_open(now_ms, status_.recording_start_time);
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
                                       sizeof(status_.brp_path),
                                       brp_schema_.open_handle)) {
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
                                       sizeof(status_.pld_path),
                                       pld_schema_.open_handle)) {
            if (brp_schema_.open) {
                (void)StorageService::enqueue_edf_close_numeric(
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
                                       sizeof(status_.sa2_path),
                                       sa2_schema_.open_handle)) {
            if (brp_schema_.open) {
                (void)StorageService::enqueue_edf_close_numeric(
                    EdfFileKind::Brp);
                brp_schema_.open = false;
            }
            if (pld_schema_.open) {
                (void)StorageService::enqueue_edf_close_numeric(
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
    if (numeric_files_open_) {
        Log::logf(CAT_EDF, LOG_DEBUG,
                  "numeric files open start=%s accepted=%s brp=%u "
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
    size_t path_size,
    EdfStorageOpenHandle &handle) {
    handle = {};
    if (!edf_datalog_annotation_path(kind, start, path, path_size)) {
        set_error("annotation_path_failed");
        return false;
    }

    if (!StorageService::enqueue_edf_open_annotation(path, kind, info,
                                                     &handle)) {
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
        return;
    }
    if (brp_schema_.open) {
        (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Brp);
    }
    if (pld_schema_.open) {
        (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Pld);
    }
    if (sa2_schema_.open) {
        (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Sa2);
    }
    if (files_open_) {
        (void)StorageService::enqueue_edf_close_annotation(
            EdfAnnotationKind::Eve);
        (void)StorageService::enqueue_edf_close_annotation(
            EdfAnnotationKind::Csl);
    }
    numeric_files_open_ = false;
    files_open_ = false;
    annotation_open_synced_ = false;
    numeric_open_synced_ = false;
    eve_open_handle_ = {};
    csl_open_handle_ = {};
    numeric_segment_day_ = 0;
    pending_stream_frame_.reset();
    numeric_open_frame_buffer_.clear();
    reset_numeric_schemas();
}

void EdfRecorderManager::sync_annotation_open_status() {
    if (!status_.active || !files_open_ || annotation_open_synced_) return;

    EdfStorageOpenResult eve;
    EdfStorageOpenResult csl;
    const bool eve_known =
        StorageService::edf_open_result(eve_open_handle_, eve);
    const bool csl_known =
        StorageService::edf_open_result(csl_open_handle_, csl);

    if (eve_known && csl_known &&
        eve.complete && csl.complete &&
        eve.success && csl.success &&
        eve.open && csl.open) {
        annotation_open_synced_ = true;
        if (status_.eve_records < eve.record_count) {
            status_.eve_records = eve.record_count;
        }
        if (status_.csl_records < csl.record_count) {
            status_.csl_records = csl.record_count;
        }
        return;
    }

    const bool failed =
        !eve_known || !csl_known ||
        (eve.complete && !eve.success) ||
        (csl.complete && !csl.success);
    const StorageServiceStatus storage = StorageService::status();
    if (failed || (storage.edf_queued == 0 && !storage.busy)) {
        status_.file_open_failures++;
        if (eve_known && eve.complete && !eve.success) {
            set_error(open_result_error(eve, "annotation_open_failed"));
        } else if (csl_known && csl.complete && !csl.success) {
            set_error(open_result_error(csl, "annotation_open_failed"));
        } else {
            set_error(storage.last_error[0] ? storage.last_error
                                            : "annotation_open_failed");
        }
        (void)StorageService::enqueue_edf_close_annotation(
            EdfAnnotationKind::Eve);
        (void)StorageService::enqueue_edf_close_annotation(
            EdfAnnotationKind::Csl);
        files_open_ = false;
        annotation_open_synced_ = true;
        eve_open_handle_ = {};
        csl_open_handle_ = {};
    }
}

bool EdfRecorderManager::sync_numeric_open_status(uint32_t now_ms) {
    if (!numeric_files_open_) return true;
    if (numeric_open_synced_) return true;

    EdfStorageOpenResult brp;
    EdfStorageOpenResult pld;
    EdfStorageOpenResult sa2;
    const bool brp_known =
        !brp_schema_.open ||
        StorageService::edf_open_result(brp_schema_.open_handle, brp);
    const bool pld_known =
        !pld_schema_.open ||
        StorageService::edf_open_result(pld_schema_.open_handle, pld);
    const bool sa2_known =
        !sa2_schema_.open ||
        StorageService::edf_open_result(sa2_schema_.open_handle, sa2);

    const bool brp_ready =
        !brp_schema_.open || (brp.complete && brp.success && brp.open);
    const bool pld_ready =
        !pld_schema_.open || (pld.complete && pld.success && pld.open);
    const bool sa2_ready =
        !sa2_schema_.open || (sa2.complete && sa2.success && sa2.open);
    const bool failed =
        !brp_known || !pld_known || !sa2_known ||
        (brp_schema_.open && brp.complete && !brp.success) ||
        (pld_schema_.open && pld.complete && !pld.success) ||
        (sa2_schema_.open && sa2.complete && !sa2.success);

    if (!brp_ready || !pld_ready || !sa2_ready) {
        const StorageServiceStatus storage = StorageService::status();
        if (!failed && (storage.edf_queued > 0 || storage.busy)) return false;

        status_.file_open_failures++;
        if (brp_schema_.open && brp.complete && !brp.success) {
            set_error(open_result_error(brp, "numeric_open_failed"));
        } else if (pld_schema_.open && pld.complete && !pld.success) {
            set_error(open_result_error(pld, "numeric_open_failed"));
        } else if (sa2_schema_.open && sa2.complete && !sa2.success) {
            set_error(open_result_error(sa2, "numeric_open_failed"));
        } else {
            set_error(storage.last_error[0] ? storage.last_error
                                            : "numeric_open_failed");
        }
        if (brp_schema_.open) {
            (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Brp);
        }
        if (pld_schema_.open) {
            (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Pld);
        }
        if (sa2_schema_.open) {
            (void)StorageService::enqueue_edf_close_numeric(EdfFileKind::Sa2);
        }
        numeric_files_open_ = false;
        numeric_open_synced_ = true;
        reset_numeric_schemas();
        next_numeric_open_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        release_stream();
        return false;
    }

    const uint32_t brp_records =
        brp_schema_.open ? brp.record_count : 0;
    const uint32_t pld_records =
        pld_schema_.open ? pld.record_count : 0;
    const uint32_t sa2_records =
        sa2_schema_.open ? sa2.record_count : 0;
    assembler_.set_current_records(brp_records, pld_records, sa2_records);
    status_.brp_records = brp_records;
    status_.pld_records = pld_records;
    status_.sa2_records = sa2_records;
    numeric_open_synced_ = true;
    return true;
}

bool EdfRecorderManager::as11_timezone_ready() const {
    if (!arbiter_) return true;
    return device_state_ && device_state_->timezone_offset_valid();
}

bool EdfRecorderManager::parse_session_local_time(
    const char *text,
    EdfLocalDateTime &out) const {
    if (!text || !text[0]) return false;

    int64_t epoch_ms = 0;
    if (!edf_parse_utc_ms(text, epoch_ms)) return false;
    if (arbiter_) {
        const As11DeviceState &state = *device_state_;
        if (state.timezone_offset_valid()) {
            return edf_epoch_ms_to_local_datetime(
                epoch_ms, state.timezone_offset_minutes(), out);
        }
        return false;
    }
    return edf_epoch_ms_to_configured_local_datetime(epoch_ms, out);
}

void EdfRecorderManager::begin_str_session(const char *session_start_time,
                                           uint32_t now_ms) {
    if (!session_start_time || !session_start_time[0]) {
        set_error("missing_str_start_time");
        return;
    }
    copy_cstr(pending_str_start_time_,
              sizeof(pending_str_start_time_),
              session_start_time);
    str_start_pending_ = true;
    (void)ensure_str_session_started(now_ms);
}

bool EdfRecorderManager::ensure_str_session_started(uint32_t now_ms) {
    if (!str_start_pending_) return true;

    EdfLocalDateTime start;
    if (!parse_session_local_time(pending_str_start_time_, start)) {
        set_error("bad_str_start_time");
        return false;
    }

    if (!begin_str_session_at(start, now_ms)) return false;
    str_start_pending_ = false;
    pending_str_start_time_[0] = 0;
    return true;
}

bool EdfRecorderManager::begin_str_session_at(const EdfLocalDateTime &start,
                                              uint32_t now_ms) {
    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.begin_therapy(start, str_status)) {
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
    (void)request_identification(now_ms);
    return true;
}

bool EdfRecorderManager::request_str_settings(uint32_t now_ms) {
    if (!arbiter_ || str_settings_rpc_.active) return false;

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
    str_settings_rpc_.mark(id, now_ms);
    status_.str_setting_requests++;
    return true;
}

bool EdfRecorderManager::request_str_summary(uint32_t now_ms) {
    if (!arbiter_ || str_summary_rpc_.active) return false;

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
    str_summary_rpc_.mark(id, now_ms);
    status_.str_summary_requests++;
    return true;
}

bool EdfRecorderManager::request_identification(uint32_t now_ms) {
    if (!arbiter_ || identification_rpc_.active) return false;

    uint32_t id = 0;
    if (!arbiter_->send_request_with_id(
            "Get",
            build_get_params("IdentificationProfiles"),
            RpcSource::EdfRecorder,
            AC_EDF_IDENTIFICATION_TIMEOUT_MS,
            id)) {
        status_.identification_failures++;
        set_error("identification_queue_failed");
        return false;
    }
    identification_rpc_.mark(id, now_ms);
    status_.identification_requests++;
    return true;
}

void EdfRecorderManager::note_str_get_timeouts(uint32_t now_ms) {
    if (str_settings_rpc_.timed_out(now_ms, AC_EDF_STR_SETTINGS_TIMEOUT_MS)) {
        str_settings_rpc_.clear();
        status_.str_setting_timeouts++;
        set_error("str_settings_timeout");
    }

    if (str_summary_rpc_.timed_out(now_ms, AC_EDF_STR_SUMMARY_TIMEOUT_MS)) {
        str_summary_rpc_.clear();
        status_.str_summary_timeouts++;
        set_error("str_summary_timeout");
        if (str_record_pending_write_) {
            str_record_pending_write_ = false;
            (void)write_str_day_record();
        }
    }

    if (identification_rpc_.timed_out(now_ms,
                                      AC_EDF_IDENTIFICATION_TIMEOUT_MS)) {
        identification_rpc_.clear();
        status_.identification_timeouts++;
        status_.identification_failures++;
        set_error("identification_timeout");
    }
}

bool EdfRecorderManager::flush_pending_str_record(const char *reason) {
    if (!str_record_pending_write_) return true;

    const bool summary_was_pending = str_summary_rpc_.active;
    str_summary_rpc_.clear();
    str_record_pending_write_ = false;

    if (!write_str_day_record()) {
        return false;
    }

    if (summary_was_pending) {
        Log::logf(CAT_EDF, LOG_WARN,
                  "STR record flushed without summary reason=%s\n",
                  reason ? reason : "--");
    }
    return true;
}

void EdfRecorderManager::handle_rpc_event(const RpcEvent &event) {
    if (event.kind != RpcEventKind::RpcResponse ||
        event.source != RpcSource::EdfRecorder ||
        !event.payload) {
        return;
    }
    if (str_settings_rpc_.matches(event.id)) {
        str_settings_rpc_.clear();
        status_.str_setting_responses++;
        handle_str_settings_response(event.payload_text());
        return;
    }
    if (str_summary_rpc_.matches(event.id)) {
        str_summary_rpc_.clear();
        status_.str_summary_responses++;
        handle_str_summary_response(event.payload_text());
        return;
    }
    if (identification_rpc_.matches(event.id)) {
        identification_rpc_.clear();
        status_.identification_responses++;
        handle_identification_response(event.payload_text());
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
    Log::logf(CAT_EDF, LOG_DEBUG,
              "STR settings values=%lu missing=%lu unmapped=%lu\n",
              static_cast<unsigned long>(result.values),
              static_cast<unsigned long>(result.missing),
              static_cast<unsigned long>(result.unmapped));

    if (str_.active() && !str_.therapy_open() && !str_record_pending_write_ &&
        !str_summary_rpc_.active) {
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
        Log::logf(CAT_EDF, LOG_DEBUG,
                  "STR summary values=%lu missing=%lu unmapped=%lu\n",
                  static_cast<unsigned long>(result.values),
                  static_cast<unsigned long>(result.missing),
                  static_cast<unsigned long>(result.unmapped));
    }

    if (str_record_pending_write_) {
        str_record_pending_write_ = false;
        (void)write_str_day_record();
    }
}

void EdfRecorderManager::handle_identification_response(
    const std::string &payload) {
    std::string json;
    if (!edf_build_identification_json(payload, json)) {
        status_.identification_failures++;
        set_error("identification_json_failed");
        return;
    }
    if (!StorageService::enqueue_edf_identification_files(json)) {
        status_.identification_failures++;
        set_error("identification_queue_failed");
        return;
    }

    status_.identification_write_requests++;
    Log::logf(CAT_EDF,
              LOG_DEBUG,
              "identification bytes=%u\n",
              static_cast<unsigned>(json.size()));
}

bool EdfRecorderManager::finish_str_session(const SessionStatus &session,
                                            uint32_t now_ms,
                                            const char *recording_end_time) {
    (void)recording_end_time;
    if (str_start_pending_) {
        if (!ensure_str_session_started(now_ms)) return false;
    }
    if (!str_.active() || !str_.therapy_open()) return true;

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
    if (!str_.active() || !str_.therapy_open()) return true;

    bool record_ready = false;
    EdfStrSessionStatus str_status = EdfStrSessionStatus::Ok;
    if (!str_.finish_therapy(end, record_ready, str_status)) {
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
        (str_summary_rpc_.active || request_str_summary(now_ms))) {
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
    if (!StorageService::enqueue_edf_str_record(path, info, record)) {
        status_.str_enqueue_failures++;
        set_error("str_queue_failed");
        return false;
    }

    copy_cstr(status_.str_path, sizeof(status_.str_path), path);
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

uint32_t EdfRecorderManager::event_coverage_session_gaps() const {
    if (status_.event_coverage_gap_count >=
        session_event_coverage_gap_count_) {
        return status_.event_coverage_gap_count -
               session_event_coverage_gap_count_;
    }
    return status_.event_coverage_gap_count;
}

void EdfRecorderManager::snapshot_event_coverage() {
    if (!arbiter_) {
        session_event_subscription_generation_ = 0;
        session_event_coverage_gap_count_ = 0;
        status_.event_subscription_generation = 0;
        status_.event_coverage_gap_count = 0;
        status_.event_coverage_uncertain = true;
        return;
    }

    const EventBrokerStatus event = arbiter_->event_broker().status();
    session_event_subscription_generation_ = event.subscription_generation;
    session_event_coverage_gap_count_ = event.coverage_gap_count;
    status_.event_subscription_generation = event.subscription_generation;
    status_.event_coverage_gap_count = event.coverage_gap_count;
    status_.event_coverage_uncertain =
        !status_.event_attached || !event.subscription_active;
}

void EdfRecorderManager::update_event_coverage() {
    if (!status_.active || !arbiter_) return;

    const EventBrokerStatus event = arbiter_->event_broker().status();
    status_.event_subscription_generation = event.subscription_generation;
    status_.event_coverage_gap_count = event.coverage_gap_count;

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
    if (!numeric_files_open_ || !numeric_open_synced_) {
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

    Log::logf(CAT_EDF, LOG_INFO,
              "segment rollover old_day=%u new_day=%u frame=%s\n",
              static_cast<unsigned>(numeric_segment_day_),
              static_cast<unsigned>(frame_day),
              frame.start_time);

    if (!finish_str_session_at(boundary, now_ms, false)) {
        Log::logf(CAT_EDF, LOG_WARN,
                  "STR rollover finish skipped id=%lu error=%s\n",
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
        Log::logf(CAT_EDF, LOG_WARN,
                  "STR rollover start skipped id=%lu error=%s\n",
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
    if (numeric_files_open_ && state && state->open) {
        if (!StorageService::enqueue_edf_numeric_record(state->layout.schema,
                                                        record)) {
            status_.record_enqueue_failures++;
            set_error("record_queue_failed");
        }
        return;
    } else {
        status_.numeric_record_drops++;
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
    if (!StorageService::enqueue_edf_annotation_record(kind, annotation)) {
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
    copy_cstr(status_.last_error, sizeof(status_.last_error), error);
}

}  // namespace aircannect
