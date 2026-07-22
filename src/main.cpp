#include <Arduino.h>

#include <freertos/task.h>

#include <new>
#include <string.h>

#include "app_config.h"
#include "as11_device_service.h"
#include "as11_settings_manager.h"
#include "background_worker.h"
#include "board.h"
#include "can_driver.h"
#include "debug_log.h"
#include "edf_report_catalog_job.h"
#include "edf_recorder_manager.h"
#include "event_broker.h"
#include "export_coordinator.h"
#include "management_console.h"
#include "memory_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "report_cache_writer_job.h"
#include "provisioning.h"
#include "report_manager.h"
#include "report_plot_prebuild_job.h"
#include "report_prefetch_job.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "sleephq_sync_job.h"
#include "storage_diagnostic_job.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "storage_sync_job.h"
#include "stream_broker.h"
#if AC_STACK_PROFILE_ENABLED
#include "stack_profiler.h"
#endif
#include "system_status_snapshot.h"
#include "tcp_bridge.h"
#include "telnet_console.h"
#include "time_sync_service.h"
#include "tls_memory.h"
#include "version.h"
#include "web_ui.h"
#include "wifi_manager.h"

using namespace aircannect;

static CanDriver can_driver;
static EventBroker event_broker;
static StreamBroker stream_broker;
static RpcArbiter rpc_arbiter(can_driver, event_broker, stream_broker);
static As11DeviceService as11_device_service;
static As11SettingsManager as11_settings_manager;
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
static EdfReportCatalogJob edf_report_catalog_job;
static OximetryManager oximetry_manager;
static ReportManager report_manager;
static BackgroundWorker bg_worker;
static StorageDiagnosticJob storage_diagnostic_job;
static StorageSyncJob *storage_sync_job = nullptr;
static SleepHqSyncJob *sleephq_sync_job = nullptr;
static ExportCoordinator export_coordinator;
static ReportCacheWriterJob report_cache_writer_job(report_manager);
static ReportPrefetchJob report_prefetch_job(report_manager);
static ReportPlotPrebuildJob report_plot_prebuild_job(report_manager);
#if AC_STACK_PROFILE_ENABLED
static StackProfiler stack_profiler;
#endif
static uint32_t edf_report_catalog_seen_sessions_ended = 0;
static bool edf_report_catalog_post_session_pending = false;
static uint32_t edf_report_catalog_refresh_due_ms = 0;
static uint32_t edf_report_catalog_timezone_revision = 0;
static ActivitySnapshot storage_activity;
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MS = 30;
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MIN_INTERVAL_MS = 1000;
static ConsoleContext console_ctx{
    rpc_arbiter,
    event_broker,
    stream_broker,
    as11_device_service,
    as11_settings_manager,
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
    StorageService::read_port(),
    &storage_diagnostic_job,
    nullptr,
    nullptr,
    nullptr,
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

static void note_as11_device_event(void *context,
                                   const As11EventFrame &frame,
                                   uint32_t now_ms) {
    static_cast<As11DeviceService *>(context)->apply_activity_event_frame(
        frame, now_ms);
}

static void note_as11_settings_history(void *context, uint32_t now_ms) {
    (void)now_ms;
    static_cast<As11SettingsManager *>(context)->note_history_change();
}

#if AC_STACK_PROFILE_ENABLED
static void poll_stack_profiler(uint32_t now_ms) {
    static TaskHandle_t async_tcp_task = nullptr;
    if (!async_tcp_task) {
        async_tcp_task = xTaskGetHandle("async_tcp");
    }
    const uint32_t oxi_stack =
        oximetry_manager.sensor_task_stack_high_water_bytes();

    StackProfileSample samples[] = {
        {StackProfileTask::Loop, true, uxTaskGetStackHighWaterMark(nullptr)},
        {StackProfileTask::AsyncTcp,
         async_tcp_task != nullptr,
         async_tcp_task ? uxTaskGetStackHighWaterMark(async_tcp_task) : 0},
        {StackProfileTask::BackgroundWorker,
         true,
         bg_worker.stack_high_water_bytes()},
        {StackProfileTask::EdfStorage,
         true,
         StorageService::stack_high_water_bytes()},
        {StackProfileTask::OximetrySensor, oxi_stack != 0, oxi_stack},
    };
    stack_profiler.poll(now_ms, samples, sizeof(samples) / sizeof(samples[0]));
}

#else
static void poll_stack_profiler(uint32_t now_ms) {
    (void)now_ms;
}
#endif

static void publish_storage_activity(bool foreground_report_demand,
                                     bool realtime_stream_active,
                                     bool export_active,
                                     bool ota_install_active,
                                     bool therapy_active) {
    const bool changed =
        storage_activity.foreground_report_demand != foreground_report_demand ||
        storage_activity.realtime_stream_active != realtime_stream_active ||
        storage_activity.export_active != export_active ||
        storage_activity.ota_install_active != ota_install_active ||
        storage_activity.therapy_active != therapy_active;
    if (!changed) return;

    storage_activity.foreground_report_demand = foreground_report_demand;
    storage_activity.realtime_stream_active = realtime_stream_active;
    storage_activity.export_active = export_active;
    storage_activity.ota_install_active = ota_install_active;
    storage_activity.therapy_active = therapy_active;
    storage_activity.generation++;
    if (storage_activity.generation == 0) storage_activity.generation++;

    StorageService::publish_activity(storage_activity);
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
                                   &stream_broker);
        }
    } else if (telnet_console.started()) {
        telnet_console.stop(&stream_broker);
    }
}

static void poll_edf_report_catalog_refresh(uint32_t now_ms) {
    const EdfRecorderStatus recorder = edf_recorder_manager.status();
    if (recorder.sessions_ended != edf_report_catalog_seen_sessions_ended) {
        edf_report_catalog_seen_sessions_ended = recorder.sessions_ended;
        edf_report_catalog_post_session_pending = true;
        edf_report_catalog_refresh_due_ms = now_ms + 5000;
    }

    if (!edf_report_catalog_post_session_pending) return;
    if (static_cast<int32_t>(now_ms - edf_report_catalog_refresh_due_ms) < 0) {
        return;
    }

    const StorageServiceStatus storage =
        edf_recorder_manager.storage_status();
    if (storage.busy || storage.edf_queued > 0 ||
        storage.open_file_count > 0) {
        edf_report_catalog_refresh_due_ms = now_ms + 1000;
        return;
    }

    if (edf_report_catalog_job.request_refresh_after_current()) {
        edf_report_catalog_post_session_pending = false;
    } else {
        edf_report_catalog_refresh_due_ms = now_ms + 2000;
    }
}

static void drain_rpc_events() {
    RpcEvent event;
    while (rpc_arbiter.next_event(event)) {
        if (event.kind == RpcEventKind::BootNotification) {
            as11_device_service.device_reset(rpc_arbiter, millis());
            as11_settings_manager.device_reset(rpc_arbiter);
        }

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
           as11_device_service.state().therapy_state() ==
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

static SleepHqSyncJob *create_sleephq_sync_job() {
    void *memory = Memory::alloc_large(sizeof(SleepHqSyncJob), false);
    if (!memory) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "failed to allocate sync job in PSRAM\n");
        return nullptr;
    }
    return new (memory) SleepHqSyncJob();
}

void setup() {
    // Serial bootstrap
    Serial.begin(AC_SERIAL_BAUD);
    delay(500);
    while (Serial.available()) Serial.read();

    // Core services
    Memory::begin();
    Log::init();

    const bool tls_allocator_ready = TlsMemory::begin();
    const TlsMemoryStatus tls_mem = TlsMemory::status();

    Log::logf(CAT_GENERAL,
              tls_allocator_ready ? LOG_INFO : LOG_WARN,
              "[TLS] mbedTLS allocator installed=%u psram=%u "
              "large_threshold=%u result=%d\n",
              tls_mem.installed ? 1u : 0u,
              tls_mem.psram_enabled ? 1u : 0u,
              static_cast<unsigned>(tls_mem.large_threshold),
              tls_mem.install_result);

    app_config.begin();
    app_config.apply_log_config();

    // Boot diagnostics
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

    // Persistent storage
    Storage::begin();
    StorageService::begin();

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

    // Management console
    serial_management_console.begin(Serial);
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] management CLI ready\n");

    // Export jobs
    storage_sync_job = create_storage_sync_job();
    sleephq_sync_job = create_sleephq_sync_job();
    export_coordinator.begin(storage_sync_job, sleephq_sync_job);

    console_ctx.storage_sync_job = storage_sync_job;
    console_ctx.sleephq_sync_job = sleephq_sync_job;
    console_ctx.export_coordinator = &export_coordinator;

    apply_storage_provisioning(app_config,
                               wifi_manager,
                               StorageService::read_port(),
                               StorageService::path_port());

    // Device/session/report managers
    session_manager.begin();

    stream_broker.set_frame_observer(note_session_stream_frame,
                                     &session_manager);
    (void)event_broker.add_frame_observer(note_as11_device_event,
                                          &as11_device_service);
    event_broker.set_settings_history_observer(
        note_as11_settings_history, &as11_settings_manager);

    sink_manager.begin(stream_broker, as11_device_service.state(),
                       session_manager);

    edf_recorder_manager.begin(rpc_arbiter, event_broker, stream_broker,
                               as11_device_service.state(), session_manager);
    edf_recorder_manager.set_enabled(app_config.data().edf_capture_enabled);

    edf_report_catalog_job.begin();
    report_manager.set_edf_report_catalog(&edf_report_catalog_job);

    oximetry_manager.begin(app_config);
    report_manager.begin();
    resmed_ota_manager.begin(rpc_arbiter, as11_device_service,
                             StorageService::atomic_write_port());
    time_sync_service.begin(app_config, wifi_manager, rpc_arbiter,
                            as11_device_service);
    if (edf_report_catalog_job.set_posix_timezone(
            app_config.data().timezone.c_str())) {
        edf_report_catalog_timezone_revision =
            time_sync_service.timezone_revision();
    }
    ota_manager.begin(app_config);

    // Network configuration
    wifi_manager.set_hostname(app_config.data().hostname);
    wifi_manager.set_softap_mode(app_config.data().softap_mode);
    wifi_manager.set_country_code(app_config.data().wifi_country);

    // CAN and network frontends
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

    web_ui.begin(rpc_arbiter, stream_broker, as11_device_service,
                 as11_settings_manager,
                 wifi_manager, tcp_bridge, app_config,
                 time_sync_service, ota_manager, resmed_ota_manager,
                 session_manager, sink_manager, oximetry_manager,
                 report_manager,
                 StorageService::read_port(),
                 StorageService::browser_port(),
                 StorageService::archive_port(),
                 StorageService::delete_port(),
                 export_coordinator,
                 storage_sync_job,
                 sleephq_sync_job,
                 console_ctx);

    // Background jobs
    storage_diagnostic_job.begin();

    if (storage_sync_job) {
        storage_sync_job->begin(app_config.data(),
                                StorageService::scan_port(),
                                StorageService::read_port(),
                                StorageService::stream_port(),
                                StorageService::atomic_write_port(),
                                StorageService::path_port());
    }

    if (sleephq_sync_job) {
        sleephq_sync_job->begin(app_config.data(),
                               StorageService::scan_port(),
                               StorageService::read_port(),
                               StorageService::stream_port(),
                               StorageService::atomic_write_port(),
                               StorageService::path_port());
    }

    bg_worker.add_job(&storage_diagnostic_job);
    bg_worker.add_job(&edf_report_catalog_job);
    bg_worker.add_job(&report_cache_writer_job);
    bg_worker.add_job(&report_prefetch_job);
    if (storage_sync_job) {
        bg_worker.add_job(storage_sync_job);
    }

    if (sleephq_sync_job) {
        bg_worker.add_job(sleephq_sync_job);
    }

    bg_worker.add_job(&report_plot_prebuild_job);
    bg_worker.begin();

    (void)edf_report_catalog_job.request_refresh();
}

void loop() {
    const uint32_t now_ms = millis();

    // RPC and OTA ingress
    const bool esp_ota_quiesce_requested =
        ota_manager.as11_quiesce_required();
    rpc_arbiter.set_esp_ota_quiesce(esp_ota_quiesce_requested);

    const bool resmed_ota_transport_active =
        resmed_ota_manager.transport_active();

    rpc_arbiter.set_raw_rpc_events_enabled(
        tcp_bridge.raw_client_connected());

    rpc_arbiter.poll();
    stream_broker.poll(rpc_arbiter, now_ms);
    event_broker.poll(rpc_arbiter, now_ms,
                      resmed_ota_transport_active);
    as11_device_service.poll(
        rpc_arbiter, now_ms,
        esp_ota_quiesce_requested || resmed_ota_transport_active);
    as11_settings_manager.poll(
        rpc_arbiter, now_ms,
        esp_ota_quiesce_requested || resmed_ota_transport_active);

    ota_manager.poll_http_upload_prepare(
        esp_ota_quiesce_requested &&
            rpc_arbiter.esp_ota_quiesce_complete(),
        esp_ota_quiesce_requested &&
            rpc_arbiter.esp_ota_quiesce_timed_out());

    drain_can_rx_after("arbiter_ota_prepare");

    // Reports and ResMed OTA
    report_manager.poll(
        rpc_arbiter,
        as11_device_service.state().therapy_state() ==
            As11TherapyState::Running,
        stream_broker.realtime_active());
    drain_can_rx_after("report");

    resmed_ota_manager.poll();
    drain_can_rx_after("resmed_ota");

    // RPC event fanout before services that depend on fresh state.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_pre_state");

    // Session and EDF capture
    session_manager.poll(as11_device_service.state(), now_ms);
    edf_recorder_manager.poll(now_ms);

    if (as11_device_service.state().timezone_offset_valid()) {
        (void)edf_report_catalog_job.set_timezone_offset_minutes(
            as11_device_service.state().timezone_offset_minutes());
    }

    poll_edf_report_catalog_refresh(now_ms);
    drain_can_rx_after("session_edf");

    // Live sinks and oximetry
    sink_manager.poll();
    oximetry_manager.poll(wifi_manager.network_available());
    drain_can_rx_after("oximetry");

    // Wi-Fi and network services
    const bool stream_activity_active = stream_broker.activity_active(
        now_ms, AC_WIFI_ROAM_STREAM_QUIET_MS);

    wifi_manager.set_roaming_suspended(stream_activity_active ||
                                       ota_manager.active() ||
                                       resmed_ota_transport_active);

    wifi_manager.poll();
    drain_can_rx_after("wifi.poll");

    sync_network_services();
    drain_can_rx_after("network_services.sync");

    // Log and time services
    const bool storage_capacity_update_allowed =
        !stream_activity_active &&
        as11_device_service.state().therapy_state() !=
            As11TherapyState::Running;

    Log::poll(wifi_manager.sta_ipv4_online());
    drain_can_rx_after("log");

    if (!resmed_ota_transport_active) {
        time_sync_service.poll();
    }

    if (edf_report_catalog_timezone_revision !=
            time_sync_service.timezone_revision() &&
        edf_report_catalog_job.set_posix_timezone(
            app_config.data().timezone.c_str())) {
        edf_report_catalog_timezone_revision =
            time_sync_service.timezone_revision();
    }

    drain_can_rx_after("time_sync");

    // ESP/Arduino OTA
    const bool esp_reboot_allowed =
        !ota_manager.status().reboot_pending ||
        rpc_arbiter.esp_ota_reboot_allowed();

    const bool arduino_ota_poll_allowed =
        as11_device_service.state().therapy_state() !=
            As11TherapyState::Running;
    const bool update_check_allowed =
        arduino_ota_poll_allowed &&
        !export_coordinator.endpoint_work_active() &&
        !report_manager.foreground_busy();

    ota_manager.poll(wifi_manager, esp_reboot_allowed,
                     !resmed_ota_transport_active,
                     arduino_ota_poll_allowed,
                     update_check_allowed);

    drain_can_rx_after("arduino_ota");

    resmed_ota_manager.poll();
    drain_can_rx_after("resmed_ota_post");

    // Storage and exports
    Storage::poll(storage_capacity_update_allowed);
    drain_can_rx_after("storage_poll");

    export_coordinator.poll(
        report_manager,
        app_config.data(),
        wifi_manager.sta_ipv4_online(),
        stream_activity_active,
        as11_device_service.state().therapy_state(),
        resmed_ota_manager.transport_active(),
        ota_manager.active(),
        now_ms);

    drain_can_rx_after("export_coordinator");

    // Activity snapshots
    const bool foreground_report_active = report_manager.foreground_busy();
    const bool export_active = export_coordinator.endpoint_work_active();
    const bool esp_ota_install_active = ota_manager.active();
    const bool storage_ota_active =
        esp_ota_install_active || resmed_ota_manager.transport_active();
    const bool therapy_active =
        as11_device_service.state().therapy_state() ==
            As11TherapyState::Running;

    publish_storage_activity(foreground_report_active,
                             stream_activity_active,
                             export_active,
                             storage_ota_active,
                             therapy_active);

    bg_worker.publish_gate(foreground_report_active,
                           as11_device_service.state().status_valid(),
                           stream_activity_active,
                           resmed_ota_manager.transport_active(),
                           esp_ota_install_active,
                           therapy_active);

    drain_can_rx_after("bgworker_gate");

    // Web, TCP, and console frontends
    web_ui.poll(drain_can_rx_after);
    drain_can_rx_after("web_ui");

    tcp_bridge.poll(rpc_arbiter);
    telnet_console.poll(console_ctx);
    serial_management_console.poll(Serial, Serial, console_ctx);

    drain_can_rx_after("frontends");

    // RPC event fanout after network and console frontends.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_post_frontends");

    poll_stack_profiler(now_ms);

    delay(0);
}
