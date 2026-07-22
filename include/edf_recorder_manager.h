#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "as11_clock.h"
#include "as11_device_state.h"
#include "as11_event_frame.h"
#include "edf_numeric_file_layout.h"
#include "edf_series.h"
#include "edf_session_metadata.h"
#include "edf_storage_catalog.h"
#include "edf_str_session.h"
#include "edf_stream_assembler.h"
#include "event_broker.h"
#include "fixed_queue.h"
#include "rpc_request_port.h"
#include "session_manager.h"
#include "storage_service.h"
#include "stream_broker.h"
#include "stream_frame.h"

namespace aircannect {

class TimeSyncService;

static constexpr size_t AC_EDF_STREAM_FRAME_BUDGET = 8;
static constexpr uint32_t AC_EDF_ATTACH_RETRY_MS = 1000;
static constexpr uint32_t AC_EDF_SESSION_RETRY_MS = 5000;
static constexpr size_t AC_EDF_NUMERIC_OPEN_FRAME_BUFFER_DEPTH = 32;

struct EdfRecorderStatus {
    // capture state
    bool enabled = false;
    bool active = false;
    bool stream_attached = false;
    bool annotation_files_open = false;
    bool numeric_files_open = false;
    bool event_observer_registered = false;
    bool event_attached = false;
    bool event_coverage_uncertain = false;
    bool recording_gate_is_open = false;
    bool recording_gate_is_closed = false;
    bool recording_gate_recovery_is_pending = false;
    bool annotation_open_is_pending = false;
    bool clock_correction_applied = false;

    // subscriptions
    StreamConsumerHandle stream_handle = STREAM_CONSUMER_INVALID;
    EventConsumerHandle event_handle = EVENT_CONSUMER_INVALID;

    // sessions
    uint32_t session_id = 0;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t segment_rollovers = 0;

    // attachment diagnostics
    uint32_t attach_attempts = 0;
    uint32_t attach_failures = 0;

    // stream frames
    uint32_t frames = 0;
    uint32_t frame_drops = 0;

    // event frames
    uint32_t event_frames = 0;
    uint32_t event_records = 0;
    uint32_t event_subscription_generation = 0;
    uint32_t event_coverage_gap_count = 0;
    uint32_t event_coverage_session_gap_count = 0;
    uint32_t respiratory_events = 0;
    uint32_t csr_events = 0;

    // EDF records
    uint32_t brp_records = 0;
    uint32_t pld_records = 0;
    uint32_t sa2_records = 0;
    uint32_t eve_records = 0;
    uint32_t csl_records = 0;
    uint32_t str_records = 0;

    // STR RPCs
    uint32_t str_setting_requests = 0;
    uint32_t str_setting_responses = 0;
    uint32_t str_setting_timeouts = 0;
    uint32_t str_setting_values = 0;
    uint32_t str_setting_missing = 0;
    uint32_t str_setting_unmapped = 0;

    uint32_t str_summary_requests = 0;
    uint32_t str_summary_responses = 0;
    uint32_t str_summary_timeouts = 0;
    uint32_t str_summary_values = 0;
    uint32_t str_summary_missing = 0;
    uint32_t str_summary_unmapped = 0;

    // identification RPCs
    uint32_t identification_requests = 0;
    uint32_t identification_responses = 0;
    uint32_t identification_timeouts = 0;
    uint32_t identification_write_requests = 0;
    uint32_t identification_failures = 0;

    // queue/storage failures
    uint32_t record_enqueue_failures = 0;
    uint32_t annotation_enqueue_failures = 0;
    uint32_t str_enqueue_failures = 0;
    uint32_t file_open_failures = 0;
    uint32_t numeric_record_drops = 0;
    uint32_t numeric_open_buffered_frames = 0;
    uint32_t numeric_open_buffer_drops = 0;

    // recording gate and mask events
    uint32_t recording_gate_rises = 0;
    uint32_t recording_gate_falls = 0;
    uint32_t recording_gate_recoveries = 0;
    uint32_t recording_gate_bad_events = 0;
    uint32_t mask_events = 0;
    uint32_t mask_bad_events = 0;

    int64_t clock_correction_ms = 0;
    uint32_t clock_correction_sample_age_ms = 0;

    // last activity
    uint32_t last_frame_ms = 0;
    uint32_t last_event_ms = 0;

    // files and messages
    char brp_path[80] = {};
    char pld_path[80] = {};
    char sa2_path[80] = {};
    char eve_path[80] = {};
    char csl_path[80] = {};
    char str_path[80] = {};
    char last_mask_event_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char recording_start_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char recording_end_time[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char last_event_data_id[64] = {};
    char last_event_name[48] = {};
    char last_error[80] = {};

    bool files_open() const {
        return annotation_files_open || numeric_files_open;
    }
    bool recording_gate_open() const { return recording_gate_is_open; }
    bool recording_gate_closed() const { return recording_gate_is_closed; }
    bool recording_gate_recovery_pending() const {
        return recording_gate_recovery_is_pending;
    }
    bool annotation_open_pending() const { return annotation_open_is_pending; }
    uint32_t event_coverage_session_gaps() const {
        return event_coverage_session_gap_count;
    }
};

class EdfRecorderManager {
public:
    // lifecycle
    explicit EdfRecorderManager(RpcRequestPort &rpc) : rpc_(rpc) {}

    void begin(EventBroker &events,
               StreamBroker &stream,
               const As11DeviceState &device_state,
               SessionManager &session,
               TimeSyncService &time_sync,
               StorageAtomicWritePort &metadata_storage);
    void poll(uint32_t now_ms);

    // control/status
    void set_enabled(bool enabled);
    EdfRecorderStatus status() const;
    StorageServiceStatus storage_status() const;
    const EdfStreamAssemblerStatus &assembler_status() const {
        return assembler_.status();
    }

    // device events
    void handle_event_frame(const As11EventFrame &frame, uint32_t now_ms);

private:
    struct ColdState;

    struct NumericSchemaState {
        bool open = false;
        EdfNumericFileLayout layout;
        EdfStorageOpenHandle open_handle;
    };

    struct PendingRpc {
        bool active() const { return ticket.valid(); }

        uint32_t next_generation() {
            generation++;
            if (generation == 0) generation++;
            return generation;
        }

        void mark(OperationTicket next_ticket) { ticket = next_ticket; }
        void clear() { ticket = {}; }

        OperationTicket ticket;
        uint32_t generation = 0;
    };

    static void event_frame_observer(void *context,
                                     const As11EventFrame &frame,
                                     uint32_t now_ms);
    static void record_observer(void *context,
                                const EdfCompletedRecordView &record);

    // session and recording gates
    void dispatch_session_edges(uint32_t now_ms);
    void start_session(const SessionStatus &session,
                       uint32_t now_ms,
                       const char *reason);
    void end_session(const SessionStatus &session,
                     uint32_t now_ms,
                     const char *reason,
                     const char *recording_end_time = nullptr);
    bool handle_recording_gate_frame(const As11EventFrame &frame,
                                     uint32_t now_ms);
    bool handle_mask_event_frame(const As11EventFrame &frame,
                                 uint32_t now_ms);
    void begin_mask_event(const char *start_time, uint32_t now_ms);
    void finish_mask_event(const char *end_time, uint32_t now_ms);
    bool ensure_annotation_files_open(uint32_t now_ms);
    void begin_recording_gate(const char *start_time, uint32_t now_ms);
    void close_recording_gate(const char *end_time, uint32_t now_ms);
    void close_recording_segment();

    // session clock provenance
    bool ensure_segment_metadata_published(const char *start_time,
                                           uint32_t now_ms);
    bool build_segment_metadata(const char *start_time,
                                EdfSessionMetadata &metadata) const;
    void finalize_segment_metadata(const char *segment_end_time,
                                   const char *therapy_end_time,
                                   uint32_t now_ms);
    void finalize_segment_metadata_at(int64_t raw_segment_end_ms,
                                      int64_t raw_therapy_end_ms,
                                      uint32_t now_ms);
    bool queue_pending_final_metadata();

    // EDF file opens
    bool open_session_annotation_files(const char *annotation_start_time);
    bool open_session_annotation_files_at(const EdfLocalDateTime &start,
                                          int64_t annotation_start_ms);
    bool open_numeric_files_from_stream(uint32_t now_ms);
    bool ensure_numeric_files_open(uint32_t now_ms,
                                   const char *numeric_start_time);
    bool numeric_stream_ready() const;
    bool build_numeric_schemas();
    void reset_numeric_schemas();
    bool enqueue_numeric_file_open(const EdfFileSchema &schema,
                                   const EdfLocalDateTime &start,
                                   const EdfHeaderInfo &info,
                                   char *path,
                                   size_t path_size,
                                   EdfStorageOpenHandle &handle);
    bool enqueue_annotation_file_open(EdfAnnotationKind kind,
                                      const EdfLocalDateTime &start,
                                      const EdfHeaderInfo &info,
                                      char *path,
                                      size_t path_size,
                                      EdfStorageOpenHandle &handle);
    bool build_recording_id(const EdfLocalDateTime &start,
                            char *dst,
                            size_t dst_size) const;

    // file state synchronization
    void close_session_files();
    void sync_annotation_open_status();
    bool sync_numeric_open_status(uint32_t now_ms);
    void buffer_numeric_open_stream(uint32_t now_ms);
    uint32_t event_coverage_session_gaps() const;

    // STR and identification records
    void begin_str_session(const char *session_start_time, uint32_t now_ms);
    bool ensure_str_session_started(uint32_t now_ms);
    bool begin_str_session_at(const EdfLocalDateTime &start,
                              uint32_t now_ms);
    bool finish_str_session(const SessionStatus &session,
                            uint32_t now_ms,
                            const char *recording_end_time = nullptr);
    bool finish_str_session_at(const EdfLocalDateTime &end,
                               bool request_summary);
    bool request_str_settings();
    bool request_str_summary();
    bool request_identification();
    void poll_rpc_completions();
    void poll_str_settings_completion();
    void poll_str_summary_completion();
    void poll_identification_completion();
    void cancel_pending_rpc(PendingRpc &pending);
    void cancel_session_rpc_requests();
    bool flush_pending_str_record(const char *reason);
    void handle_str_settings_response(const std::string &payload);
    void handle_str_summary_response(const std::string &payload);
    void handle_identification_response(const std::string &payload);
    bool write_str_day_record();

    // device time
    void freeze_session_clock(uint32_t now_ms);
    void freeze_session_timezone();
    void apply_pending_mask_event(uint32_t now_ms);
    bool parse_session_raw_time(const char *text, int64_t &epoch_ms) const;
    bool parse_session_utc_time(const char *text, int64_t &epoch_ms) const;
    bool as11_timezone_ready() const;
    bool parse_session_local_time(const char *text, EdfLocalDateTime &out) const;

    // subscriptions
    void attach_events();
    void release_events();
    void snapshot_event_coverage();
    void update_event_coverage();

    void attach_stream(uint32_t now_ms);
    void release_stream();
    void update_stream_queue_drops();
    void drain_stream(uint32_t now_ms);

    // record assembly/output
    bool roll_segment_if_needed(const StreamFrameData &frame,
                                uint32_t now_ms);
    bool frame_sleep_day(const StreamFrameData &frame,
                         EdfLocalDateTime &local,
                         uint16_t &sleep_day) const;
    bool sleep_day_boundary_epoch_ms(const StreamFrameData &frame,
                                     const EdfLocalDateTime &local,
                                     int64_t &boundary_ms) const;
    void handle_completed_record(const EdfCompletedRecordView &record);
    bool enqueue_event_annotation(EdfAnnotationKind kind,
                                  const As11EventRecord &record);

    void set_error(const char *error);

    // subsystem owners
    RpcRequestPort &rpc_;
    EventBroker *events_ = nullptr;
    StreamBroker *stream_ = nullptr;
    const As11DeviceState *device_state_ = nullptr;
    SessionManager *session_ = nullptr;
    TimeSyncService *time_sync_ = nullptr;

    // session cursors
    uint32_t seen_session_starts_ = 0;
    uint32_t seen_session_ends_ = 0;
    uint32_t last_queue_drops_ = 0;

    uint32_t next_attach_ms_ = 0;
    uint32_t next_session_start_ms_ = 0;
    uint32_t next_numeric_open_ms_ = 0;
    uint32_t next_annotation_open_ms_ = 0;

    // event coverage
    int64_t annotation_start_epoch_ms_ = 0;
    uint32_t session_event_subscription_generation_ = 0;
    uint32_t session_event_coverage_gap_count_ = 0;

    // open/recording state
    bool initialized_ = false;
    bool files_open_ = false;
    bool numeric_files_open_ = false;
    bool recording_gate_open_ = false;
    bool recording_gate_closed_ = false;
    bool recording_gate_recovery_pending_ = false;
    bool annotation_open_pending_ = false;
    bool str_start_pending_ = false;
    char pending_str_start_time_[AC_STREAM_FRAME_START_TIME_MAX] = {};
    char pending_mask_event_start_time_[AC_STREAM_FRAME_START_TIME_MAX] = {};
    bool annotation_open_synced_ = false;
    bool numeric_open_synced_ = false;
    EdfStorageOpenHandle eve_open_handle_;
    EdfStorageOpenHandle csl_open_handle_;
    uint16_t numeric_segment_day_ = 0;
    As11ClockTransform session_clock_;
    bool session_clock_frozen_ = false;
    int32_t session_timezone_offset_minutes_ = 0;
    bool session_timezone_frozen_ = false;

    // session metadata and numeric schemas
    ColdState *cold_ = nullptr;

    // STR/identification state
    PendingRpc str_settings_rpc_;
    PendingRpc str_summary_rpc_;
    PendingRpc identification_rpc_;
    bool str_record_pending_write_ = false;

    // owned subsystems/status
    EdfStrSessionAccumulator str_;
    EdfRecorderStatus status_;
    EdfStreamAssembler assembler_;

    StreamFrameRef pending_stream_frame_;
    FixedQueue<StreamFrameRef, AC_EDF_NUMERIC_OPEN_FRAME_BUFFER_DEPTH>
        numeric_open_frame_buffer_;
};

}  // namespace aircannect
