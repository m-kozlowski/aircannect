#include <Arduino.h>

#include <new>

#include "app_config.h"
#include "background_worker.h"
#include "board.h"
#include "board_report.h"
#include "can_driver.h"
#include "debug_log.h"
#include "edf_recorder_manager.h"
#include "edf_storage_worker.h"
#include "management_console.h"
#include "memory_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "report_cache_writer_job.h"
#include "provisioning.h"
#include "report_manager.h"
#include "report_prefetch_job.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_archive_job.h"
#include "storage_delete_job.h"
#include "storage_manager.h"
#include "storage_sync_job.h"
#include "storage_writer.h"
#include "system_status_snapshot.h"
#include "tcp_bridge.h"
#include "telnet_console.h"
#include "time_sync_service.h"
#include "version.h"
#include "web_ui.h"
#include "wifi_manager.h"

using namespace aircannect;

static CanDriver can_driver;
static RpcArbiter rpc_arbiter(can_driver);
static ManagementConsole serial_management_console;
static WifiManager wifi_manager;
static TcpBridge tcp_bridge;
static TelnetConsole telnet_console;
static AppConfig app_config;
static WebUI web_ui;
static TimeSyncService time_sync_service;
static OtaManager ota_manager;
static ResmedOtaManager resmed_ota_manager;
static SessionManager session_manager;
static SinkManager sink_manager;
static EdfRecorderManager edf_recorder_manager;
static OximetryManager oximetry_manager;
static ReportManager report_manager;
static BackgroundWorker bg_worker;
static StorageArchiveJob storage_archive_job;
static StorageDeleteJob storage_delete_job;
static StorageSyncJob *storage_sync_job = nullptr;
static ReportCacheWriterJob report_cache_writer_job(report_manager);
static ReportPrefetchJob report_prefetch_job(report_manager);
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MS = 30;
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MIN_INTERVAL_MS = 1000;
static ConsoleContext console_ctx{
    rpc_arbiter,
    tcp_bridge,
    wifi_manager,
    app_config,
    time_sync_service,
    ota_manager,
    resmed_ota_manager,
    session_manager,
    sink_manager,
    edf_recorder_manager,
    oximetry_manager,
    report_manager,
    &web_ui,
};

static bool is_rpc_event(RpcEventKind kind) {
    return kind == RpcEventKind::RpcResponse ||
           kind == RpcEventKind::RpcNotification ||
           kind == RpcEventKind::RpcUnmatched;
}

static void note_session_stream_frame(void *context,
                                      const StreamFrameData &frame,
                                      uint32_t now_ms) {
    static_cast<SessionManager *>(context)->note_stream_frame(frame, now_ms);
}

static void sync_network_services() {
    const bool should_run_tcp =
        app_config.data().tcp_bridge_enabled &&
        wifi_manager.network_available();
    const bool should_run_telnet =
        app_config.data().telnet_console_enabled &&
        wifi_manager.network_available();

    if (should_run_tcp) {
        if (!tcp_bridge.started()) {
            tcp_bridge.begin(app_config.data().tcp_bridge_port);
        } else if (tcp_bridge.port() != app_config.data().tcp_bridge_port) {
            tcp_bridge.restart(app_config.data().tcp_bridge_port);
        }
    } else if (tcp_bridge.started()) {
        tcp_bridge.stop();
    }

    if (should_run_telnet) {
        if (!telnet_console.started()) {
            telnet_console.begin(app_config.data().telnet_console_port);
        } else if (telnet_console.port() !=
                   app_config.data().telnet_console_port) {
            telnet_console.restart(app_config.data().telnet_console_port,
                                   &rpc_arbiter);
        }
    } else if (telnet_console.started()) {
        telnet_console.stop(&rpc_arbiter);
    }
}

static void drain_rpc_events() {
    RpcEvent event;
    while (rpc_arbiter.next_event(event)) {
        serial_management_console.handle_event(Serial, event);
        telnet_console.handle_event(event);
        web_ui.handle_event(event);
        if (event.kind == RpcEventKind::BootNotification) {
            session_manager.note_device_boot(millis());
        }
        if (is_rpc_event(event.kind) && event.payload) {
            tcp_bridge.broadcast_rpc_payload(event.payload);
        }
    }
}

static bool main_loop_drain_timing_active() {
    return session_manager.status().state == SessionState::Active ||
           rpc_arbiter.as11_state().therapy_state() ==
               As11TherapyState::Running;
}

static void drain_can_rx_after(const char *section) {
    static uint32_t last_checkpoint_ms = 0;
    static uint32_t last_warn_ms = 0;

    const uint32_t before_ms = millis();
    const uint32_t gap_ms = last_checkpoint_ms == 0
                                ? 0
                                : before_ms - last_checkpoint_ms;
    const size_t drained = rpc_arbiter.drain_can_rx();
    const uint32_t after_drain_ms = millis();
    const uint32_t drain_ms = after_drain_ms - before_ms;

    if (gap_ms > AC_MAIN_LOOP_CAN_DRAIN_WARN_MS &&
        main_loop_drain_timing_active() &&
        (last_warn_ms == 0 ||
         after_drain_ms - last_warn_ms >=
             AC_MAIN_LOOP_CAN_DRAIN_WARN_MIN_INTERVAL_MS)) {
        last_warn_ms = after_drain_ms;
        Log::logf(CAT_CAN, LOG_WARN,
                  "main-loop CAN drain gap section=%s gap_ms=%u "
                  "drained=%u drain_ms=%u\n",
                  section ? section : "--",
                  static_cast<unsigned>(gap_ms),
                  static_cast<unsigned>(drained),
                  static_cast<unsigned>(drain_ms));
    }

    last_checkpoint_ms = millis();
}

static StorageSyncJob *create_storage_sync_job() {
    void *memory = Memory::alloc_large(sizeof(StorageSyncJob), false);
    if (!memory) {
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[SYNC] failed to allocate sync job in PSRAM\n");
        return nullptr;
    }
    return new (memory) StorageSyncJob();
}

struct PostTherapyReportSyncHandoff {
    As11TherapyState last_state = As11TherapyState::Unknown;
    uint32_t summary_refresh_due_ms = 0;
    bool sync_pending = false;
    bool sync_grace_armed = false;
    uint32_t sync_due_ms = 0;
    uint32_t sync_deadline_ms = 0;

    void poll(RpcArbiter &arbiter,
              ReportManager &report,
              StorageSyncJob *sync_job,
              uint32_t now_ms) {
        const As11TherapyState state =
            arbiter.as11_state().therapy_state();

        if (state == As11TherapyState::Running) {
            reset_after_running();
            last_state = state;
            return;
        }

        if (last_state == As11TherapyState::Running &&
            state != As11TherapyState::Running) {
            arm_after_stop(sync_job, now_ms);
        }

        maybe_refresh_summary(arbiter, report, sync_job, now_ms);
        maybe_queue_sync(arbiter, report, sync_job, now_ms);
        last_state = state;
    }

private:
    static uint32_t due_after(uint32_t now_ms, uint32_t delay_ms) {
        uint32_t due = now_ms + delay_ms;
        if (due == 0) due = 1;
        return due;
    }

    void reset_after_running() {
        summary_refresh_due_ms = 0;
        sync_pending = false;
        sync_grace_armed = false;
        sync_due_ms = 0;
        sync_deadline_ms = 0;
    }

    void arm_after_stop(StorageSyncJob *sync_job, uint32_t now_ms) {
        summary_refresh_due_ms =
            due_after(now_ms, AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS);
        sync_pending = sync_job != nullptr;
        sync_grace_armed = false;
        sync_due_ms = 0;
        sync_deadline_ms =
            due_after(now_ms, AC_REPORT_POST_THERAPY_SYNC_MAX_WAIT_MS);
        if (sync_job) {
            sync_job->defer_idle_work_until(summary_refresh_due_ms);
        }
    }

    void maybe_refresh_summary(RpcArbiter &arbiter,
                               ReportManager &report,
                               StorageSyncJob *sync_job,
                               uint32_t now_ms) {
        if (summary_refresh_due_ms == 0 ||
            static_cast<int32_t>(now_ms - summary_refresh_due_ms) < 0) {
            return;
        }
        if (arbiter.stream_activity_active()) {
            summary_refresh_due_ms =
                due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
            if (sync_job) {
                sync_job->defer_idle_work_until(summary_refresh_due_ms);
            }
            return;
        }
        if (report.request_summary_refresh()) {
            summary_refresh_due_ms = 0;
            sync_grace_armed = false;
            sync_due_ms = 0;
        } else {
            summary_refresh_due_ms =
                due_after(now_ms, AC_BG_WORKER_BUSY_RECHECK_MS);
        }
    }

    void maybe_queue_sync(RpcArbiter &arbiter,
                          ReportManager &report,
                          StorageSyncJob *sync_job,
                          uint32_t now_ms) {
        if (!sync_pending || summary_refresh_due_ms != 0) return;

        if (!sync_job) {
            sync_pending = false;
            sync_due_ms = 0;
            sync_deadline_ms = 0;
            return;
        }
        if (arbiter.stream_activity_active()) {
            sync_due_ms = 0;
            sync_job->defer_idle_work_until(
                due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS));
            return;
        }
        if (report.background_work_active()) {
            const bool deadline_reached =
                sync_deadline_ms != 0 &&
                static_cast<int32_t>(now_ms - sync_deadline_ms) >= 0;
            if (!deadline_reached) {
                sync_due_ms = 0;
                sync_job->defer_idle_work_until(
                    due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS));
                return;
            }
            Log::logf(CAT_STORAGE,
                      LOG_WARN,
                      "[SYNC] post-therapy sync fallback after report wait\n");
            queue_sync(sync_job);
            return;
        }
        if (!sync_grace_armed) {
            sync_grace_armed = true;
            sync_due_ms =
                due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
            sync_job->defer_idle_work_until(sync_due_ms);
            return;
        }
        if (sync_due_ms == 0) {
            sync_due_ms =
                due_after(now_ms, AC_BG_WORKER_ACTIVITY_GRACE_MS);
            sync_job->defer_idle_work_until(sync_due_ms);
            return;
        }
        if (static_cast<int32_t>(now_ms - sync_due_ms) >= 0) {
            queue_sync(sync_job);
        }
    }

    void queue_sync(StorageSyncJob *sync_job) {
        (void)sync_job->request_post_therapy_sync();
        sync_pending = false;
        sync_due_ms = 0;
        sync_deadline_ms = 0;
    }
};

static PostTherapyReportSyncHandoff post_therapy_handoff;

void setup() {
    Serial.begin(AC_SERIAL_BAUD);
    delay(500);
    while (Serial.available()) Serial.read();

    Memory::begin();
    Log::init();
    app_config.begin();
    app_config.apply_log_config();
    const MemoryStatus mem = Memory::status();
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[BOOT] version=%s build=%s reset_reason=%s\n",
              aircannect_version(),
              aircannect_build_date(),
              system_reset_reason_name());
    Log::logf(CAT_GENERAL, LOG_INFO,
              "[INIT] chip=%s heap_free=%u heap_total=%u\n",
              ESP.getChipModel(),
              static_cast<unsigned>(mem.heap_free),
              static_cast<unsigned>(mem.heap_total));
    if (mem.psram_available) {
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[INIT] psram=yes psram_free=%u psram_total=%u\n",
                  static_cast<unsigned>(mem.psram_free),
                  static_cast<unsigned>(mem.psram_total));
    } else {
        Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] psram=no\n");
    }
    if (!rpc_arbiter.reserve_reassembly_buffers()) {
        Log::logf(CAT_RPC, LOG_WARN,
                  "[INIT] datagram reassembly buffer prealloc failed\n");
    }
    Storage::begin();
    StorageWriter::begin();
    EdfStorageWorker::begin();
    const StorageStatus storage = Storage::status();
    if (storage.mounted) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "[INIT] storage=%s/%s free_bytes=%llu\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state),
                  static_cast<unsigned long long>(storage.free_bytes));
    } else if (storage.last_error[0]) {
        Log::logf(CAT_STORAGE, LOG_INFO,
                  "[INIT] storage=%s/%s error=%s\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state),
                  storage.last_error);
    } else {
        Log::logf(CAT_STORAGE, LOG_INFO, "[INIT] storage=%s/%s\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state));
    }

    serial_management_console.begin(Serial);
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] management CLI ready\n");

    storage_sync_job = create_storage_sync_job();
    apply_storage_provisioning(app_config, wifi_manager);
    session_manager.begin();
    rpc_arbiter.set_stream_frame_observer(note_session_stream_frame,
                                          &session_manager);
    sink_manager.begin(rpc_arbiter, session_manager);
    edf_recorder_manager.begin(rpc_arbiter, session_manager);
    edf_recorder_manager.set_enabled(app_config.data().edf_capture_enabled);
    oximetry_manager.begin(app_config);
    report_manager.begin();
    resmed_ota_manager.begin(rpc_arbiter);
    time_sync_service.begin(app_config, wifi_manager, rpc_arbiter);
    ota_manager.begin(app_config);
    wifi_manager.set_hostname(app_config.data().hostname);
    wifi_manager.set_softap_mode(app_config.data().softap_mode);
    wifi_manager.set_country_code(app_config.data().wifi_country);

    if (!can_driver.begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] CAN failed to start; management CLI still active "
                  "on serial\n");
    }

    wifi_manager.begin();
    if (!app_config.data().tcp_bridge_enabled) {
        Log::logf(CAT_TCP, LOG_INFO,
                  "raw bridge disabled by config\n");
    }
    sync_network_services();
    web_ui.begin(rpc_arbiter, wifi_manager, tcp_bridge, app_config,
                 time_sync_service, ota_manager, resmed_ota_manager,
                 session_manager, sink_manager, oximetry_manager,
                 report_manager,
                 storage_archive_job,
                 storage_delete_job,
                 storage_sync_job,
                 console_ctx);

    // Idle-time background storage worker (prefetch + plot build). Started last,
    // once every subsystem it gates on (report, CAN, OTA) is up.
    storage_archive_job.begin();
    storage_delete_job.begin();
    if (storage_sync_job) {
        storage_sync_job->begin(app_config.data());
    }
    bg_worker.add_job(&storage_archive_job);
    bg_worker.add_job(&storage_delete_job);
    bg_worker.add_job(&report_cache_writer_job);
    bg_worker.add_job(&report_prefetch_job);
    if (storage_sync_job) {
        bg_worker.add_job(storage_sync_job);
    }
    bg_worker.begin();
}

void loop() {
    static uint32_t next_active_file_log_drain_ms = 0;

    const bool esp_ota_quiesce_requested =
        ota_manager.as11_quiesce_required();
    rpc_arbiter.set_esp_ota_quiesce(esp_ota_quiesce_requested);
    const bool resmed_ota_transport_active =
        resmed_ota_manager.transport_active();
    rpc_arbiter.set_background_polls_suspended(
        resmed_ota_transport_active);
    rpc_arbiter.set_raw_rpc_events_enabled(
        tcp_bridge.raw_client_connected());
    rpc_arbiter.poll();
    ota_manager.poll_http_upload_prepare(
        esp_ota_quiesce_requested &&
            rpc_arbiter.esp_ota_quiesce_complete(),
        esp_ota_quiesce_requested &&
            rpc_arbiter.esp_ota_quiesce_timed_out());
    drain_can_rx_after("arbiter_ota_prepare");
    report_manager.poll(rpc_arbiter);
    drain_can_rx_after("report");
    resmed_ota_manager.poll();
    // Long loop sections can otherwise let TWAI RX overflow during bursty
    // stream/RPC traffic. Keep service points bounded and owned by RpcArbiter.
    drain_can_rx_after("resmed_ota");
    // First drain handles events produced by CAN/RPC/OTA work before services
    // that depend on fresh state run below.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_pre_state");
    const uint32_t now_ms = millis();
    session_manager.poll(rpc_arbiter.as11_state(), now_ms);
    edf_recorder_manager.poll(now_ms);
    post_therapy_handoff.poll(rpc_arbiter,
                              report_manager,
                              storage_sync_job,
                              now_ms);
    drain_can_rx_after("session_edf");
    sink_manager.poll();
    oximetry_manager.poll(wifi_manager.network_available());
    wifi_manager.set_roaming_suspended(rpc_arbiter.stream_activity_active() ||
                                       ota_manager.active() ||
                                       resmed_ota_transport_active);
    wifi_manager.poll();
    sync_network_services();
    const bool storage_writer_idle_allowed =
        !rpc_arbiter.stream_activity_active() &&
        rpc_arbiter.as11_state().therapy_state() !=
            As11TherapyState::Running;
    const bool active_file_log_drain_allowed =
        !storage_writer_idle_allowed &&
        !rpc_arbiter.background_backpressure_active() &&
        (next_active_file_log_drain_ms == 0 ||
         static_cast<int32_t>(now_ms - next_active_file_log_drain_ms) >= 0);
    if (active_file_log_drain_allowed) {
        next_active_file_log_drain_ms =
            now_ms + AC_FILE_LOG_ACTIVE_DRAIN_INTERVAL_MS;
    }
    Log::poll(wifi_manager.network_available(),
              storage_writer_idle_allowed || active_file_log_drain_allowed,
              storage_writer_idle_allowed ? 0
                                          : AC_FILE_LOG_ACTIVE_DRAIN_BUDGET);
    if (!resmed_ota_transport_active) {
        time_sync_service.poll();
    }
    drain_can_rx_after("network_services");
    const bool esp_reboot_allowed =
        !ota_manager.status().reboot_pending ||
        rpc_arbiter.esp_ota_reboot_allowed();
    ota_manager.poll(wifi_manager, esp_reboot_allowed,
                     !resmed_ota_transport_active);
    resmed_ota_manager.poll();
    Storage::poll(storage_writer_idle_allowed);
    if (storage_sync_job) {
        storage_sync_job->set_network_available(
            wifi_manager.mode_state() == WifiModeState::StaConnected);
        storage_sync_job->refresh_config(app_config.data(), now_ms);
    }
    drain_can_rx_after("ota_storage_sync");
    // Publish the worker's gate inputs from here (the owner thread) so the
    // background task reads a coherent snapshot instead of these managers.
    bg_worker.publish_gate(
        report_manager.foreground_busy(),
        rpc_arbiter.as11_state().status_valid(),
        rpc_arbiter.stream_activity_active(),
        resmed_ota_manager.transport_active(),
        ota_manager.active(),
        rpc_arbiter.as11_state().therapy_state() == As11TherapyState::Running);
    drain_can_rx_after("bgworker_gate");
    web_ui.poll(drain_can_rx_after);
    drain_can_rx_after("web_ui");
    tcp_bridge.poll(rpc_arbiter);
    telnet_console.poll(console_ctx);
    serial_management_console.poll(Serial, Serial, console_ctx);
    drain_can_rx_after("frontends");
    // Second drain handles events produced by network and console frontends
    // during this loop turn.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_post_frontends");
    if (storage_writer_idle_allowed) {
        StorageWriter::poll();
    } else if (active_file_log_drain_allowed &&
               !rpc_arbiter.background_backpressure_active()) {
        StorageWriter::poll_limited(AC_FILE_LOG_ACTIVE_WRITE_BUDGET_ITEMS,
                                    AC_FILE_LOG_ACTIVE_WRITE_BUDGET_BYTES);
    }
    drain_can_rx_after("storage_writer");

    delay(0);
}
