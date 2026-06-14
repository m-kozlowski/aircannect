#include "edf_recorder_manager.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include "as11_rpc.h"
#include "debug_log.h"
#include "edf_storage_catalog.h"
#include "edf_storage_worker.h"

namespace aircannect {
namespace {

const char *const EDF_CAPTURE_STREAM_IDS =
    "_RFL,"
    "_MKP,"
    "_MKF,"
    "_MKI,"
    "_MKE,"
    "_LKF,"
    "_RR2,"
    "_TD2,"
    "_MV2,"
    "_TGT,"
    "_IE2,"
    "_SNI,"
    "_FFL,"
    "_INT,"
    "_HRT,"
    "_SAO";

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

bool event_is_respiratory(const As11EventFrame &frame) {
    return frame.data_id == "TherapyEvents-RespiratoryEvents";
}

bool event_is_csr(const As11EventRecord &record) {
    return record.name == "CsrStart" || record.name == "CsrEnd";
}

const char *file_kind_name(EdfFileKind kind) {
    switch (kind) {
        case EdfFileKind::Brp: return "BRP";
        case EdfFileKind::Pld: return "PLD";
        case EdfFileKind::Sa2: return "SA2";
        default: return "EDF";
    }
}

}  // namespace

void EdfRecorderManager::begin(RpcArbiter &arbiter,
                               SessionManager &session) {
    if (initialized_) return;
    arbiter_ = &arbiter;
    session_ = &session;
    status_.event_observer_registered =
        arbiter_->add_event_frame_observer(event_frame_observer, this);
    assembler_.set_record_observer(record_observer, this);
    (void)assembler_.begin();
    initialized_ = true;
}

void EdfRecorderManager::poll(uint32_t now_ms) {
    if (!initialized_ || !arbiter_ || !session_) return;
    dispatch_session_edges(now_ms);

    if (!status_.enabled || !status_.active) {
        release_stream();
        return;
    }

    attach_stream(now_ms);
    drain_stream(now_ms);
}

void EdfRecorderManager::set_enabled(bool enabled) {
    if (status_.enabled == enabled) return;
    status_.enabled = enabled;
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
        if (event_is_respiratory(frame)) {
            status_.respiratory_events++;
            if (event_is_csr(record)) status_.csr_events++;
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
    status_.frames = 0;
    status_.frame_drops = 0;
    status_.event_frames = 0;
    status_.event_records = 0;
    status_.respiratory_events = 0;
    status_.csr_events = 0;
    status_.brp_records = 0;
    status_.pld_records = 0;
    status_.sa2_records = 0;
    status_.record_enqueue_failures = 0;
    status_.file_open_failures = 0;
    status_.last_frame_ms = 0;
    status_.last_event_ms = 0;
    status_.brp_path[0] = 0;
    status_.pld_path[0] = 0;
    status_.sa2_path[0] = 0;
    status_.last_event_data_id[0] = 0;
    status_.last_event_name[0] = 0;
    status_.last_error[0] = 0;
    next_attach_ms_ = now_ms;

    if (!open_numeric_files(session)) {
        status_.active = false;
        status_.sessions_started--;
        next_session_start_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return;
    }

    if (!assembler_.start_session(session.start_device_time)) {
        set_error(assembler_.status().last_error);
        close_numeric_files();
        status_.active = false;
        status_.sessions_started--;
        next_session_start_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return;
    }

    Log::logf(CAT_STREAM, LOG_INFO,
              "[EDF] recorder session start id=%lu reason=%s\n",
              static_cast<unsigned long>(status_.session_id),
              reason ? reason : "--");
}

void EdfRecorderManager::end_session(const SessionStatus &session,
                                     uint32_t now_ms,
                                     const char *reason) {
    (void)session;
    (void)now_ms;
    if (!status_.active) return;

    release_stream();
    assembler_.end_session();
    close_numeric_files();
    status_.active = false;
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

bool EdfRecorderManager::open_numeric_files(const SessionStatus &session) {
    EdfLocalDateTime start;
    if (!edf_parse_as11_local_datetime(session.start_device_time, start)) {
        status_.file_open_failures++;
        set_error("bad_session_time");
        return false;
    }

    char date[9] = {};
    char time[9] = {};
    if (!edf_header_date(start, date, sizeof(date)) ||
        !edf_header_time(start, time, sizeof(time))) {
        status_.file_open_failures++;
        set_error("bad_edf_time");
        return false;
    }

    char recording_id[AC_EDF_STORAGE_RECORDING_ID_MAX] = {};
    snprintf(recording_id, sizeof(recording_id),
             "AirCANnect AS11 session %lu",
             static_cast<unsigned long>(session.session_id));

    EdfHeaderInfo info;
    info.patient_id = "AirCANnect";
    info.recording_id = recording_id;
    info.start_date = date;
    info.start_time = time;
    info.record_count = 0;

    const bool brp_open = enqueue_file_open(EdfFileKind::Brp, start, info,
                                            status_.brp_path,
                                            sizeof(status_.brp_path));
    if (!brp_open) {
        status_.file_open_failures++;
        return false;
    }
    const bool pld_open = enqueue_file_open(EdfFileKind::Pld, start, info,
                                            status_.pld_path,
                                            sizeof(status_.pld_path));
    if (!pld_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        status_.file_open_failures++;
        return false;
    }
    const bool sa2_open = enqueue_file_open(EdfFileKind::Sa2, start, info,
                                            status_.sa2_path,
                                            sizeof(status_.sa2_path));
    if (!sa2_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
        status_.file_open_failures++;
        return false;
    }

    files_open_ = true;
    status_.files_open = true;
    return true;
}

bool EdfRecorderManager::enqueue_file_open(EdfFileKind kind,
                                           const EdfLocalDateTime &start,
                                           const EdfHeaderInfo &info,
                                           char *path,
                                           size_t path_size) {
    if (!edf_datalog_path(kind, start, path, path_size)) {
        set_error("path_failed");
        return false;
    }

    const EdfFileSchema &schema = edf_numeric_schema(kind);
    if (!EdfStorageWorker::enqueue_open_numeric(path, schema, info)) {
        char error[80] = {};
        snprintf(error, sizeof(error), "open_queue_%s", file_kind_name(kind));
        set_error(error);
        return false;
    }
    return true;
}

void EdfRecorderManager::close_numeric_files() {
    if (!files_open_) {
        status_.files_open = false;
        return;
    }
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
    files_open_ = false;
    status_.files_open = false;
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

    const std::string params = build_stream_params(EDF_CAPTURE_STREAM_IDS,
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
}

void EdfRecorderManager::drain_stream(uint32_t now_ms) {
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

    for (size_t i = 0; i < AC_EDF_STREAM_FRAME_BUDGET; ++i) {
        StreamFrameRef frame;
        if (!arbiter_->next_stream_frame(status_.stream_handle, frame)) break;
        if (!frame) continue;
        assembler_.ingest_frame(*frame);
        status_.frames++;
        status_.last_frame_ms = now_ms;
    }
}

void EdfRecorderManager::handle_completed_record(
    const EdfCompletedRecordView &record) {
    const EdfFileSchema *schema = edf_numeric_schema_for_series(record.series);
    if (files_open_ && schema &&
        !EdfStorageWorker::enqueue_numeric_record(*schema, record)) {
        status_.record_enqueue_failures++;
        set_error("record_queue_failed");
    }

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

void EdfRecorderManager::set_error(const char *error) {
    copy_text(status_.last_error, sizeof(status_.last_error), error);
}

void EdfRecorderManager::copy_text(char *dst, size_t size, const char *src) {
    if (!dst || size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

}  // namespace aircannect
