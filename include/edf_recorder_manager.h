#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_event_frame.h"
#include "edf_numeric_file_layout.h"
#include "edf_series.h"
#include "edf_storage_catalog.h"
#include "edf_str_session.h"
#include "edf_storage_worker.h"
#include "edf_stream_assembler.h"
#include "fixed_queue.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "stream_frame.h"

namespace aircannect {

static constexpr size_t AC_EDF_STREAM_FRAME_BUDGET = 8;
static constexpr uint32_t AC_EDF_ATTACH_RETRY_MS = 1000;
static constexpr uint32_t AC_EDF_SESSION_RETRY_MS = 5000;
static constexpr uint32_t AC_EDF_NUMERIC_FLUSH_CHUNK_MS =
    AC_EDF_RECORD_MS / 10;
static constexpr uint32_t AC_EDF_NUMERIC_START_MAX_WAIT_MS =
    AC_EDF_NUMERIC_FLUSH_CHUNK_MS * 2;
static constexpr uint32_t AC_EDF_NUMERIC_START_STABLE_MS = 4000;
static constexpr float AC_EDF_NUMERIC_START_MIN_NONZERO_PRESSURE_CM_H2O =
    0.05f;
static constexpr size_t AC_EDF_NUMERIC_OPEN_FRAME_BUFFER_DEPTH = 32;

struct EdfRecorderStatus {
    bool enabled = false;
    bool active = false;
    bool stream_attached = false;
    bool files_open = false;
    bool rpc_observer_registered = false;
    bool event_observer_registered = false;
    bool event_attached = false;
    bool event_coverage_uncertain = false;
    StreamConsumerHandle stream_handle = STREAM_CONSUMER_INVALID;
    EventConsumerHandle event_handle = EVENT_CONSUMER_INVALID;
    uint32_t session_id = 0;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t segment_rollovers = 0;
    uint32_t attach_attempts = 0;
    uint32_t attach_failures = 0;
    uint32_t frames = 0;
    uint32_t frame_drops = 0;
    uint32_t event_frames = 0;
    uint32_t event_records = 0;
    uint32_t event_subscription_generation = 0;
    uint32_t event_coverage_gap_count = 0;
    uint32_t event_coverage_session_gaps = 0;
    uint32_t respiratory_events = 0;
    uint32_t csr_events = 0;
    uint32_t brp_records = 0;
    uint32_t pld_records = 0;
    uint32_t sa2_records = 0;
    uint32_t eve_records = 0;
    uint32_t csl_records = 0;
    uint32_t str_records = 0;
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
    uint32_t record_enqueue_failures = 0;
    uint32_t annotation_enqueue_failures = 0;
    uint32_t str_enqueue_failures = 0;
    uint32_t file_open_failures = 0;
    uint32_t numeric_record_drops = 0;
    uint32_t numeric_start_deferred_frames = 0;
    uint32_t numeric_start_forced = 0;
    uint32_t numeric_open_buffered_frames = 0;
    uint32_t numeric_open_buffer_drops = 0;
    uint32_t last_frame_ms = 0;
    uint32_t last_event_ms = 0;
    char brp_path[80] = {};
    char pld_path[80] = {};
    char sa2_path[80] = {};
    char eve_path[80] = {};
    char csl_path[80] = {};
    char str_path[80] = {};
    char last_event_data_id[64] = {};
    char last_event_name[48] = {};
    char last_error[80] = {};
};

class EdfRecorderManager {
public:
    void begin(RpcArbiter &arbiter, SessionManager &session);
    void poll(uint32_t now_ms);

    void set_enabled(bool enabled);
    bool enabled() const { return status_.enabled; }
    const EdfRecorderStatus &status() const { return status_; }
    EdfStorageWorkerStatus storage_status() const;
    const EdfStreamAssemblerStatus &assembler_status() const {
        return assembler_.status();
    }

    void handle_event_frame(const As11EventFrame &frame, uint32_t now_ms);

private:
    struct NumericSchemaState {
        bool open = false;
        EdfNumericFileLayout layout;
    };

    static void event_frame_observer(void *context,
                                     const As11EventFrame &frame,
                                     uint32_t now_ms);
    static void rpc_event_observer(void *context, const RpcEvent &event);
    static void record_observer(void *context,
                                const EdfCompletedRecordView &record);
    void dispatch_session_edges(uint32_t now_ms);
    void start_session(const SessionStatus &session,
                       uint32_t now_ms,
                       const char *reason);
    void end_session(const SessionStatus &session,
                     uint32_t now_ms,
                     const char *reason);
    bool open_session_annotation_files(const char *annotation_start_time);
    bool open_session_annotation_files_at(const EdfLocalDateTime &start,
                                          int64_t annotation_start_ms);
    bool open_numeric_files_from_stream(uint32_t now_ms);
    bool ensure_numeric_files_open(uint32_t now_ms,
                                   const char *numeric_start_time);
    bool numeric_stream_ready() const;
    bool numeric_start_frame_has_data(const StreamFrameData &frame) const;
    bool numeric_start_frame_ready(const StreamFrameData &frame);
    bool numeric_start_wait_expired(const StreamFrameData &frame);
    bool build_numeric_schemas();
    void reset_numeric_schemas();
    bool enqueue_numeric_file_open(const EdfFileSchema &schema,
                                   const EdfLocalDateTime &start,
                                   const EdfHeaderInfo &info,
                                   char *path,
                                   size_t path_size);
    bool enqueue_annotation_file_open(EdfAnnotationKind kind,
                                      const EdfLocalDateTime &start,
                                      const EdfHeaderInfo &info,
                                      char *path,
                                      size_t path_size);
    bool build_recording_id(const EdfLocalDateTime &start,
                            char *dst,
                            size_t dst_size) const;
    void close_session_files();
    void sync_annotation_open_status();
    bool sync_numeric_open_status(uint32_t now_ms);
    void buffer_numeric_open_stream(uint32_t now_ms);
    bool storage_file_matches(const EdfStorageOpenFileStatus &file,
                              const char *path) const;
    bool begin_str_session(const char *session_start_time);
    bool begin_str_session_at(const EdfLocalDateTime &start,
                              uint32_t now_ms);
    bool finish_str_session(const SessionStatus &session, uint32_t now_ms);
    bool finish_str_session_at(const EdfLocalDateTime &end,
                               uint32_t now_ms,
                               bool request_summary);
    bool request_str_settings(uint32_t now_ms);
    bool request_str_summary(uint32_t now_ms);
    void note_str_get_timeouts(uint32_t now_ms);
    void handle_rpc_event(const RpcEvent &event);
    void handle_str_settings_response(const std::string &payload);
    void handle_str_summary_response(const std::string &payload);
    bool write_str_day_record();
    bool parse_session_local_time(const char *text, EdfLocalDateTime &out) const;
    void attach_events();
    void release_events();
    void snapshot_event_coverage();
    void update_event_coverage();
    void attach_stream(uint32_t now_ms);
    void release_stream();
    void update_stream_queue_drops();
    void drain_stream(uint32_t now_ms);
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
    static void copy_text(char *dst, size_t size, const char *src);

    RpcArbiter *arbiter_ = nullptr;
    SessionManager *session_ = nullptr;
    uint32_t seen_session_starts_ = 0;
    uint32_t seen_session_ends_ = 0;
    uint32_t last_queue_drops_ = 0;
    uint32_t next_attach_ms_ = 0;
    uint32_t next_session_start_ms_ = 0;
    uint32_t next_numeric_open_ms_ = 0;
    int64_t numeric_start_first_frame_epoch_ms_ = 0;
    int64_t numeric_start_ready_since_epoch_ms_ = 0;
    int64_t annotation_start_epoch_ms_ = 0;
    uint32_t session_event_subscription_generation_ = 0;
    uint32_t session_event_coverage_gap_count_ = 0;
    bool initialized_ = false;
    bool files_open_ = false;
    bool numeric_files_open_ = false;
    bool annotation_open_synced_ = false;
    bool numeric_open_synced_ = false;
    bool numeric_segment_day_valid_ = false;
    uint16_t numeric_segment_day_ = 0;
    bool str_settings_pending_ = false;
    uint32_t str_settings_request_id_ = 0;
    uint32_t str_settings_request_ms_ = 0;
    bool str_summary_pending_ = false;
    uint32_t str_summary_request_id_ = 0;
    uint32_t str_summary_request_ms_ = 0;
    bool str_record_pending_write_ = false;
    NumericSchemaState brp_schema_;
    NumericSchemaState pld_schema_;
    NumericSchemaState sa2_schema_;
    EdfStrSessionAccumulator str_;
    EdfRecorderStatus status_;
    EdfStreamAssembler assembler_;
    StreamFrameRef pending_stream_frame_;
    FixedQueue<StreamFrameRef, AC_EDF_NUMERIC_OPEN_FRAME_BUFFER_DEPTH>
        numeric_open_frame_buffer_;
};

}  // namespace aircannect
