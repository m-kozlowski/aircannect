#pragma once

#include <stddef.h>
#include <stdint.h>

#include "as11_event_frame.h"
#include "edf_storage_catalog.h"
#include "edf_storage_worker.h"
#include "edf_stream_assembler.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "stream_frame.h"

namespace aircannect {

static constexpr size_t AC_EDF_STREAM_FRAME_BUDGET = 8;
static constexpr uint32_t AC_EDF_ATTACH_RETRY_MS = 1000;
static constexpr uint32_t AC_EDF_SESSION_RETRY_MS = 5000;

struct EdfRecorderStatus {
    bool enabled = false;
    bool active = false;
    bool stream_attached = false;
    bool files_open = false;
    bool event_observer_registered = false;
    StreamConsumerHandle stream_handle = STREAM_CONSUMER_INVALID;
    uint32_t session_id = 0;
    uint32_t sessions_started = 0;
    uint32_t sessions_ended = 0;
    uint32_t attach_attempts = 0;
    uint32_t attach_failures = 0;
    uint32_t frames = 0;
    uint32_t frame_drops = 0;
    uint32_t event_frames = 0;
    uint32_t event_records = 0;
    uint32_t respiratory_events = 0;
    uint32_t csr_events = 0;
    uint32_t brp_records = 0;
    uint32_t pld_records = 0;
    uint32_t sa2_records = 0;
    uint32_t record_enqueue_failures = 0;
    uint32_t file_open_failures = 0;
    uint32_t last_frame_ms = 0;
    uint32_t last_event_ms = 0;
    char brp_path[80] = {};
    char pld_path[80] = {};
    char sa2_path[80] = {};
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
    static void event_frame_observer(void *context,
                                     const As11EventFrame &frame,
                                     uint32_t now_ms);
    static void record_observer(void *context,
                                const EdfCompletedRecordView &record);

    void dispatch_session_edges(uint32_t now_ms);
    void start_session(const SessionStatus &session,
                       uint32_t now_ms,
                       const char *reason);
    void end_session(const SessionStatus &session,
                     uint32_t now_ms,
                     const char *reason);
    bool open_numeric_files(const SessionStatus &session);
    bool enqueue_file_open(EdfFileKind kind,
                           const EdfLocalDateTime &start,
                           const EdfHeaderInfo &info,
                           char *path,
                           size_t path_size);
    void close_numeric_files();
    void attach_stream(uint32_t now_ms);
    void release_stream();
    void drain_stream(uint32_t now_ms);
    void handle_completed_record(const EdfCompletedRecordView &record);
    void set_error(const char *error);
    static void copy_text(char *dst, size_t size, const char *src);

    RpcArbiter *arbiter_ = nullptr;
    SessionManager *session_ = nullptr;
    uint32_t seen_session_starts_ = 0;
    uint32_t seen_session_ends_ = 0;
    uint32_t last_queue_drops_ = 0;
    uint32_t next_attach_ms_ = 0;
    uint32_t next_session_start_ms_ = 0;
    bool initialized_ = false;
    bool files_open_ = false;
    EdfRecorderStatus status_;
    EdfStreamAssembler assembler_;
};

}  // namespace aircannect
