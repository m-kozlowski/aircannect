#include "edf_recorder_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "as11_rpc.h"
#include "as11_settings.h"
#include "debug_log.h"
#include "edf_storage_catalog.h"
#include "edf_storage_worker.h"
#include "edf_str_field_map.h"
#include "report_store.h"

namespace aircannect {
namespace {

static constexpr uint32_t AC_EDF_STR_SETTINGS_TIMEOUT_MS = 12000;

struct StrSummaryApplyContext {
    EdfRecorderManager *manager = nullptr;
    uint32_t applied_records = 0;
};

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

bool summary_record_sleep_day(const ReportSummaryRecord &record,
                              uint16_t &day) {
    if (!record.start_ms || !record.has_tz_offset_min) return false;
    const int64_t local_ms =
        static_cast<int64_t>(record.start_ms) +
        static_cast<int64_t>(record.tz_offset_min) * 60LL * 1000LL;
    if (local_ms < 0) return false;

    const time_t seconds = static_cast<time_t>(local_ms / 1000LL);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;

    EdfLocalDateTime local;
    local.year = tmv.tm_year + 1900;
    local.month = tmv.tm_mon + 1;
    local.day = tmv.tm_mday;
    local.hour = tmv.tm_hour;
    local.minute = tmv.tm_min;
    local.second = tmv.tm_sec;
    return edf_sleep_day_epoch_days(local, day);
}

bool parse_float_text(const char *text, float &out) {
    if (!text || !text[0]) return false;
    char *end = nullptr;
    const float value = strtof(text, &end);
    if (!end || *end != 0 || !isfinite(value)) return false;
    out = value;
    return true;
}

bool parse_iso8601_seconds(const char *text, float &out) {
    if (!text || text[0] != 'P' || text[1] != 'T') return false;
    const char *value = text + 2;
    char *end = nullptr;
    const float seconds = strtof(value, &end);
    if (!end || *end != 'S' || end[1] != 0 || !isfinite(seconds)) {
        return false;
    }
    out = seconds;
    return true;
}

void rpc_name_for_str_tag(const char *tag, char *out, size_t out_size) {
    if (!out || !out_size) return;
    out[0] = 0;
    if (!tag || !tag[0]) return;
    if (tag[0] == '_') {
        snprintf(out, out_size, "%s", tag);
        return;
    }
    snprintf(out, out_size, "_%s", tag);
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
    arbiter_->set_edf_recorder_event_observer(rpc_event_observer, this);
    status_.event_observer_registered =
        arbiter_->add_event_frame_observer(event_frame_observer, this);
    assembler_.set_record_observer(record_observer, this);
    (void)assembler_.begin();
    initialized_ = true;
}

void EdfRecorderManager::poll(uint32_t now_ms) {
    if (!initialized_ || !arbiter_ || !session_) return;
    note_str_settings_timeout(now_ms);
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
            (void)enqueue_event_annotation(EdfAnnotationKind::Eve, record);
            (void)enqueue_event_annotation(EdfAnnotationKind::Csl, record);
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

bool EdfRecorderManager::summary_record_observer(
    void *context,
    const ReportSummaryRecord &record) {
    StrSummaryApplyContext *apply =
        static_cast<StrSummaryApplyContext *>(context);
    if (!apply || !apply->manager) return true;
    if (apply->manager->apply_str_summary_record(record)) {
        apply->applied_records++;
    }
    return true;
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
    status_.eve_records = 0;
    status_.csl_records = 0;
    status_.str_records = 0;
    status_.str_setting_requests = 0;
    status_.str_setting_responses = 0;
    status_.str_setting_timeouts = 0;
    status_.str_setting_values = 0;
    status_.str_setting_missing = 0;
    status_.str_setting_unmapped = 0;
    status_.record_enqueue_failures = 0;
    status_.annotation_enqueue_failures = 0;
    status_.str_enqueue_failures = 0;
    status_.file_open_failures = 0;
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
    session_start_epoch_ms_ = 0;
    next_attach_ms_ = now_ms;
    str_settings_pending_ = false;
    str_settings_request_id_ = 0;
    str_settings_request_ms_ = 0;

    int64_t session_start_ms = 0;
    if (!edf_parse_utc_ms(session.start_device_time, session_start_ms)) {
        status_.active = false;
        status_.sessions_started--;
        set_error("bad_session_epoch");
        next_session_start_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return;
    }
    session_start_epoch_ms_ = session_start_ms;

    if (!open_session_files(session)) {
        status_.active = false;
        status_.sessions_started--;
        session_start_epoch_ms_ = 0;
        next_session_start_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return;
    }

    if (!assembler_.start_session(session.start_device_time)) {
        set_error(assembler_.status().last_error);
        close_session_files();
        status_.active = false;
        status_.sessions_started--;
        session_start_epoch_ms_ = 0;
        next_session_start_ms_ = now_ms + AC_EDF_SESSION_RETRY_MS;
        return;
    }

    if (!begin_str_session(session)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR session start skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

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

    if (!finish_str_session(session)) {
        Log::logf(CAT_STREAM, LOG_WARN,
                  "[EDF] STR session end skipped id=%lu error=%s\n",
                  static_cast<unsigned long>(status_.session_id),
                  status_.last_error[0] ? status_.last_error : "--");
    }

    release_stream();
    assembler_.end_session();
    close_session_files();
    status_.active = false;
    session_start_epoch_ms_ = 0;
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

bool EdfRecorderManager::open_session_files(const SessionStatus &session) {
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

    const bool brp_open = enqueue_numeric_file_open(EdfFileKind::Brp, start,
                                                    info, status_.brp_path,
                                                    sizeof(status_.brp_path));
    if (!brp_open) {
        status_.file_open_failures++;
        return false;
    }
    const bool pld_open = enqueue_numeric_file_open(EdfFileKind::Pld, start,
                                                    info, status_.pld_path,
                                                    sizeof(status_.pld_path));
    if (!pld_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        status_.file_open_failures++;
        return false;
    }
    const bool sa2_open = enqueue_numeric_file_open(EdfFileKind::Sa2, start,
                                                    info, status_.sa2_path,
                                                    sizeof(status_.sa2_path));
    if (!sa2_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
        status_.file_open_failures++;
        return false;
    }
    const bool eve_open = enqueue_annotation_file_open(EdfAnnotationKind::Eve,
                                                       start, info,
                                                       status_.eve_path,
                                                       sizeof(status_.eve_path));
    if (!eve_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
        status_.file_open_failures++;
        return false;
    }
    const bool csl_open = enqueue_annotation_file_open(EdfAnnotationKind::Csl,
                                                       start, info,
                                                       status_.csl_path,
                                                       sizeof(status_.csl_path));
    if (!csl_open) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
        status_.file_open_failures++;
        return false;
    }

    if (!enqueue_recording_start_annotations()) {
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
        (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
        (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Csl);
        status_.file_open_failures++;
        return false;
    }

    files_open_ = true;
    status_.files_open = true;
    return true;
}

bool EdfRecorderManager::enqueue_numeric_file_open(
    EdfFileKind kind,
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

bool EdfRecorderManager::enqueue_recording_start_annotations() {
    EdfAnnotationRecord record;
    record.onset_seconds = 0;
    record.duration_seconds = 0;
    record.label = "Recording starts";
    const bool eve_ok =
        EdfStorageWorker::enqueue_annotation_record(EdfAnnotationKind::Eve,
                                                    record);
    const bool csl_ok =
        EdfStorageWorker::enqueue_annotation_record(EdfAnnotationKind::Csl,
                                                    record);
    if (!eve_ok || !csl_ok) {
        status_.annotation_enqueue_failures++;
        set_error("recording_start_queue_failed");
        return false;
    }
    status_.eve_records++;
    status_.csl_records++;
    return true;
}

void EdfRecorderManager::close_session_files() {
    if (!files_open_) {
        status_.files_open = false;
        return;
    }
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Brp);
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Pld);
    (void)EdfStorageWorker::enqueue_close_numeric(EdfFileKind::Sa2);
    (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Eve);
    (void)EdfStorageWorker::enqueue_close_annotation(EdfAnnotationKind::Csl);
    files_open_ = false;
    status_.files_open = false;
}

bool EdfRecorderManager::parse_str_time(const char *text,
                                        EdfLocalDateTime &out) {
    return text && text[0] &&
           edf_parse_as11_local_datetime(text, out);
}

void EdfRecorderManager::reset_str_day(
    uint16_t epoch_days,
    const EdfLocalDateTime &sleep_day_start) {
    for (size_t i = 0; i < AC_EDF_STR_DATA_SAMPLES_PER_RECORD; ++i) {
        str_samples_[i] = -1;
    }

    const size_t date_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DATE_SIGNAL);
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
    if (date_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        str_samples_[date_offset] = static_cast<int16_t>(epoch_days);
    }
    if (events_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        str_samples_[events_offset] = 0;
    }
    if (duration_offset < AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        str_samples_[duration_offset] = 0;
    }

    str_day_active_ = true;
    str_mask_open_ = false;
    str_day_epoch_days_ = epoch_days;
    str_day_start_ = sleep_day_start;
    str_current_on_minute_ = 0;
    str_mask_events_ = 0;
}

bool EdfRecorderManager::begin_str_session(const SessionStatus &session) {
    EdfLocalDateTime start;
    if (!parse_str_time(session.start_device_time, start)) {
        set_error("bad_str_start_time");
        return false;
    }

    uint16_t day = 0;
    uint16_t minute = 0;
    EdfLocalDateTime sleep_day_start;
    if (!edf_sleep_day_epoch_days(start, day) ||
        !edf_sleep_day_minute(start, minute) ||
        !edf_sleep_day_start(start, sleep_day_start)) {
        set_error("bad_str_sleep_day");
        return false;
    }

    if (!str_day_active_ || str_day_epoch_days_ != day) {
        reset_str_day(day, sleep_day_start);
    }

    if (str_mask_events_ >= AC_EDF_STR_MASK_EVENT_CAPACITY) {
        set_error("str_mask_events_full");
        return false;
    }

    str_current_on_minute_ = minute;
    str_mask_open_ = true;
    request_str_settings(millis());
    return true;
}

void EdfRecorderManager::request_str_settings(uint32_t now_ms) {
    if (!arbiter_ || str_settings_pending_) return;

    std::string names;
    names.reserve(512);
    for (size_t i = 0; i < AC_EDF_STR_FIELD_MAP_COUNT; ++i) {
        const EdfStrFieldMap &field = AC_EDF_STR_FIELD_MAP[i];
        if (field.source != EdfStrFieldSource::SettingGet ||
            !field.short_tag) {
            continue;
        }
        char rpc_name[8] = {};
        rpc_name_for_str_tag(field.short_tag, rpc_name, sizeof(rpc_name));
        if (!rpc_name[0]) continue;
        if (!names.empty()) names += ' ';
        names += rpc_name;
    }
    if (names.empty()) return;

    uint32_t id = 0;
    if (!arbiter_->send_request_with_id("Get",
                                        build_get_params(names),
                                        RpcSource::EdfRecorder,
                                        AC_EDF_STR_SETTINGS_TIMEOUT_MS,
                                        id)) {
        set_error("str_settings_queue_failed");
        return;
    }
    str_settings_pending_ = true;
    str_settings_request_id_ = id;
    str_settings_request_ms_ = now_ms;
    status_.str_setting_requests++;
}

void EdfRecorderManager::note_str_settings_timeout(uint32_t now_ms) {
    if (!str_settings_pending_) return;
    if (static_cast<int32_t>(now_ms - str_settings_request_ms_) <
        static_cast<int32_t>(AC_EDF_STR_SETTINGS_TIMEOUT_MS + 1000)) {
        return;
    }
    str_settings_pending_ = false;
    str_settings_request_id_ = 0;
    str_settings_request_ms_ = 0;
    status_.str_setting_timeouts++;
    set_error("str_settings_timeout");
}

void EdfRecorderManager::handle_rpc_event(const RpcEvent &event) {
    if (event.kind != RpcEventKind::RpcResponse ||
        event.source != RpcSource::EdfRecorder ||
        !event.payload) {
        return;
    }
    if (!str_settings_pending_ || event.id != str_settings_request_id_) {
        return;
    }

    str_settings_pending_ = false;
    str_settings_request_id_ = 0;
    str_settings_request_ms_ = 0;
    status_.str_setting_responses++;
    handle_str_settings_response(event.payload_text());
}

bool EdfRecorderManager::set_str_signal_physical(size_t signal_index,
                                                 float physical_value) {
    const EdfSignalSpec *spec = edf_str_signal_spec(signal_index);
    if (!spec || spec->samples_per_record != 1) return false;
    const size_t offset = edf_str_signal_sample_offset(signal_index);
    if (offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) return false;
    str_samples_[offset] = edf_encode_physical_sample(*spec, physical_value);
    return true;
}

bool EdfRecorderManager::set_str_signal_digital(size_t signal_index,
                                                int16_t digital_value) {
    const EdfSignalSpec *spec = edf_str_signal_spec(signal_index);
    if (!spec || spec->samples_per_record != 1) return false;
    const size_t offset = edf_str_signal_sample_offset(signal_index);
    if (offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) return false;
    str_samples_[offset] = digital_value;
    return true;
}

bool EdfRecorderManager::apply_str_summary_record(
    const ReportSummaryRecord &record) {
    if (!str_day_active_) return false;
    uint16_t day = 0;
    if (!summary_record_sleep_day(record, day) ||
        day != str_day_epoch_days_) {
        return false;
    }

    uint32_t values = 0;
    for (size_t i = AC_REPORT_SUMMARY_STR_FIRST_SIGNAL;
         i <= AC_REPORT_SUMMARY_STR_LAST_SIGNAL;
         ++i) {
        int16_t digital = 0;
        if (report_summary_str_sample(record, i, digital) &&
            set_str_signal_digital(i, digital)) {
            values++;
        }
    }
    if (values == 0) return false;

    Log::logf(CAT_STREAM, LOG_DEBUG,
              "[EDF] STR summary values=%lu day=%u\n",
              static_cast<unsigned long>(values),
              static_cast<unsigned>(day));
    return true;
}

bool EdfRecorderManager::apply_str_summary_from_store() {
    if (!str_day_active_) return false;
    StrSummaryApplyContext context;
    context.manager = this;
    if (!ReportStore::read_summary_records(summary_record_observer,
                                           &context)) {
        return false;
    }
    return context.applied_records > 0;
}

void EdfRecorderManager::handle_str_settings_response(
    const std::string &payload) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        set_error("str_settings_json_failed");
        return;
    }

    JsonObjectConst result = doc["result"].as<JsonObjectConst>();
    if (result.isNull()) {
        set_error(json_member_present(payload, "error")
                      ? "str_settings_rpc_error"
                      : "str_settings_missing_result");
        return;
    }

    uint32_t values = 0;
    uint32_t missing = 0;
    uint32_t unmapped = 0;
    for (size_t i = 0; i < AC_EDF_STR_FIELD_MAP_COUNT; ++i) {
        const EdfStrFieldMap &field = AC_EDF_STR_FIELD_MAP[i];
        if (field.source != EdfStrFieldSource::SettingGet ||
            !field.short_tag) {
            continue;
        }

        char rpc_name[8] = {};
        rpc_name_for_str_tag(field.short_tag, rpc_name, sizeof(rpc_name));
        JsonVariantConst value = result[rpc_name];
        if (value.isNull()) {
            missing++;
            continue;
        }

        bool mapped = false;
        float physical = 0.0f;
        if (value.is<bool>()) {
            physical = value.as<bool>() ? 1.0f : 0.0f;
            mapped = true;
        } else if (value.is<float>() || value.is<int>() ||
                   value.is<long>()) {
            physical = value.as<float>();
            mapped = true;
        } else if (value.is<const char *>()) {
            const char *text = value.as<const char *>();
            int16_t option_index = 0;
            if (parse_iso8601_seconds(text, physical) ||
                parse_float_text(text, physical)) {
                mapped = true;
            } else if (as11_setting_option_index_for_rpc_name(
                           rpc_name, text, option_index)) {
                physical = static_cast<float>(option_index);
                mapped = true;
            }
        }

        if (mapped && set_str_signal_physical(i, physical)) {
            values++;
        } else {
            unmapped++;
        }
    }

    status_.str_setting_values += values;
    status_.str_setting_missing += missing;
    status_.str_setting_unmapped += unmapped;
    Log::logf(CAT_STREAM, LOG_DEBUG,
              "[EDF] STR settings values=%lu missing=%lu unmapped=%lu\n",
              static_cast<unsigned long>(values),
              static_cast<unsigned long>(missing),
              static_cast<unsigned long>(unmapped));

    if (str_day_active_ && !str_mask_open_) {
        (void)write_str_day_record();
    }
}

bool EdfRecorderManager::finish_str_session(const SessionStatus &session) {
    if (!str_day_active_ || !str_mask_open_) return true;

    EdfLocalDateTime end;
    if (!parse_str_time(session.end_device_time, end) &&
        !parse_str_time(session.last_stream_start_time, end) &&
        !parse_str_time(session.start_device_time, end)) {
        set_error("bad_str_end_time");
        return false;
    }

    uint16_t end_day = 0;
    uint16_t end_minute = 0;
    if (!edf_sleep_day_epoch_days(end, end_day) ||
        !edf_sleep_day_minute(end, end_minute)) {
        set_error("bad_str_end_sleep_day");
        return false;
    }

    uint16_t off_minute = end_minute;
    if (end_day != str_day_epoch_days_) {
        off_minute = 1440;
    } else if (off_minute < str_current_on_minute_) {
        off_minute = str_current_on_minute_;
    }

    if (str_mask_events_ >= AC_EDF_STR_MASK_EVENT_CAPACITY) {
        set_error("str_mask_events_full");
        return false;
    }

    const size_t event_index = str_mask_events_;
    const size_t on_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_ON_SIGNAL) +
        event_index;
    const size_t off_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_OFF_SIGNAL) +
        event_index;
    const size_t events_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_MASK_EVENTS_SIGNAL);
    const size_t duration_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL);
    if (on_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        off_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        events_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD ||
        duration_offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
        set_error("str_offset_failed");
        return false;
    }

    str_samples_[on_offset] = static_cast<int16_t>(str_current_on_minute_);
    str_samples_[off_offset] = static_cast<int16_t>(off_minute);
    str_mask_events_++;
    str_samples_[events_offset] = static_cast<int16_t>(str_mask_events_);

    int duration = str_samples_[duration_offset];
    if (duration < 0) duration = 0;
    duration += off_minute > str_current_on_minute_
                    ? off_minute - str_current_on_minute_
                    : 0;
    if (duration > 1440) duration = 1440;
    str_samples_[duration_offset] = static_cast<int16_t>(duration);
    str_mask_open_ = false;

    return write_str_day_record();
}

bool EdfRecorderManager::write_str_day_record() {
    (void)apply_str_summary_from_store();

    char path[sizeof(status_.str_path)] = {};
    if (!edf_str_path(path, sizeof(path))) {
        set_error("str_path_failed");
        return false;
    }

    char date[9] = {};
    char time[9] = {};
    if (!edf_header_date(str_day_start_, date, sizeof(date)) ||
        !edf_header_time(str_day_start_, time, sizeof(time))) {
        set_error("bad_str_header_time");
        return false;
    }

    EdfHeaderInfo info;
    info.patient_id = "AirCANnect";
    info.recording_id = "AirCANnect AS11 STR";
    info.start_date = date;
    info.start_time = time;
    info.record_count = 0;

    EdfStrRecordView record;
    record.digital_samples = str_samples_;
    record.sample_count = AC_EDF_STR_DATA_SAMPLES_PER_RECORD;
    if (!EdfStorageWorker::enqueue_str_record(path, info, record)) {
        status_.str_enqueue_failures++;
        set_error("str_queue_failed");
        return false;
    }

    copy_text(status_.str_path, sizeof(status_.str_path), path);
    status_.str_records++;
    return true;
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

bool EdfRecorderManager::enqueue_event_annotation(
    EdfAnnotationKind kind,
    const As11EventRecord &record) {
    if (!files_open_) return false;

    const char *label = nullptr;
    if (!edf_annotation_label_for_event(kind, record.name.c_str(), label)) {
        return false;
    }

    int64_t event_ms = 0;
    if (session_start_epoch_ms_ <= 0 ||
        !edf_parse_utc_ms(record.report_time.c_str(), event_ms)) {
        status_.annotation_enqueue_failures++;
        set_error("event_time_failed");
        return false;
    }

    int64_t onset_ms = event_ms - session_start_epoch_ms_;
    if (onset_ms < 0) onset_ms = 0;

    EdfAnnotationRecord annotation;
    annotation.onset_seconds = static_cast<int32_t>(onset_ms / 1000);
    annotation.duration_seconds = 0;
    annotation.label = label;
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
