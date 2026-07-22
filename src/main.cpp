#include <Arduino.h>

#include <freertos/task.h>

#include <new>
#include <string.h>

#include "as11_device_service.h"
#include "as11_settings_manager.h"
#include "arduino_ota_source.h"
#include "ble_sensor_source.h"
#include "board.h"
#include "can_driver.h"
#include "config_http_controller.h"
#include "config_service.h"
#include "console_command_router.h"
#include "console_commands.h"
#include "debug_log.h"
#include "device_http_controller.h"
#include "edf_recorder_manager.h"
#include "event_broker.h"
#include "export_coordinator.h"
#include "export_endpoint_config.h"
#include "export_http_controller.h"
#include "export_task.h"
#include "firmware_installer.h"
#include "firmware_url_source.h"
#include "http_route_module.h"
#include "live_http_controller.h"
#include "management_console.h"
#include "memory_manager.h"
#include "ota_http_controller.h"
#include "oximetry_ble_runtime.h"
#include "oximetry_hub.h"
#include "oximetry_http_controller.h"
#include "plx_peripheral.h"
#include "provisioning.h"
#include "report_http_controller.h"
#include "report_spool_service.h"
#include "report_task.h"
#include "resmed_firmware_http_controller.h"
#include "resmed_firmware_preparer.h"
#include "resmed_firmware_repository.h"
#include "resmed_ota_manager.h"
#include "rpc_transport.h"
#include "rpc_quiesce_coordinator.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "settings_http_controller.h"
#include "storage_http_controller.h"
#include "storage_upload_http_controller.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "status_http_controller.h"
#include "stream_broker.h"
#if AC_STACK_PROFILE_ENABLED
#include "stack_profiler.h"
#endif
#include "system_status_snapshot.h"
#include "tcp_bridge.h"
#include "telnet_console.h"
#include "time_sync_service.h"
#include "tls_memory.h"
#include "update_checker.h"
#include "udp_oximeter_source.h"
#include "version.h"
#include "web_ui.h"
#include "wifi_manager.h"
#include "wifi_http_controller.h"

using namespace aircannect;

static CanDriver can_driver;
static EventBroker event_broker;
static StreamBroker stream_broker;
static RpcTransport rpc_transport(can_driver);
static RpcQuiesceCoordinator rpc_quiesce_coordinator(
    rpc_transport, event_broker, stream_broker);
static As11DeviceService as11_device_service;
static As11SettingsManager as11_settings_manager;
static ManagementConsole serial_management_console;
static WifiManager wifi_manager;
static TcpBridge tcp_bridge;
static TelnetConsole telnet_console;
static ConfigService config_service;
static WebUI web_ui;
static TimeSyncService time_sync_service;
static FirmwareInstaller firmware_installer;
static FirmwareUrlSource firmware_url_source(firmware_installer);
static ArduinoOtaSource arduino_ota_source(firmware_installer);
static UpdateChecker update_checker;
static ResmedOtaManager resmed_ota_manager;
static ResmedFirmwarePreparer resmed_firmware_preparer;
static SessionManager session_manager;
static SinkManager sink_manager;
static EdfRecorderManager edf_recorder_manager(rpc_transport);
static OximetryBleRuntime oximetry_ble_runtime;
static OximetryHub oximetry_hub;
static UdpOximeterSource oximetry_udp_source;
static BleSensorSource oximetry_sensor_source(oximetry_ble_runtime);
static PlxPeripheral plx_peripheral(oximetry_ble_runtime);
static ReportSpoolService report_spool_service(rpc_transport);
static ReportTask report_task;
static ReportHttpController report_http_controller;
static StorageHttpController storage_http_controller;
static StorageUploadHttpController storage_upload_http_controller;
static ResmedFirmwareRepository resmed_firmware_repository;
static ResmedFirmwareHttpController resmed_firmware_http_controller;
static ExportHttpController export_http_controller;
static OtaHttpController ota_http_controller;
static SettingsHttpController settings_http_controller;
static ConfigHttpController config_http_controller;
static DeviceHttpController device_http_controller;
static OximetryHttpController oximetry_http_controller;
static WifiHttpController wifi_http_controller;
static StatusHttpController status_http_controller;
static LiveHttpController live_http_controller;
static HttpRouteModule *web_route_modules[] = {
    &report_http_controller,
    &storage_http_controller,
    &storage_upload_http_controller,
    &resmed_firmware_http_controller,
    &export_http_controller,
    &ota_http_controller,
    &settings_http_controller,
    &config_http_controller,
    &device_http_controller,
    &oximetry_http_controller,
    &wifi_http_controller,
    &status_http_controller,
    &live_http_controller,
};
static ExportTask export_task;
static ExportCoordinator export_coordinator;
static CanConsoleCommands can_console_commands(
    rpc_transport, can_driver, event_broker, stream_broker);
static As11DeviceConsoleCommands as11_device_console_commands(
    rpc_transport, rpc_transport, as11_device_service, time_sync_service);
static RpcConsoleCommands rpc_console_commands(
    rpc_transport, rpc_transport, as11_device_service, as11_settings_manager);
static StreamConsoleCommands stream_console_commands(stream_broker);
static NetworkConsoleCommands network_console_commands(wifi_manager,
                                                       tcp_bridge);
static CoreDiagnosticsConsoleCommands core_console_commands;
static StorageConsoleCommands storage_console_commands(
    config_service, StorageService::read_port());
static RuntimeConsoleCommands runtime_console_commands(session_manager,
                                                       sink_manager);
static EdfConsoleCommands edf_console_commands(edf_recorder_manager,
                                               config_service);
static OximetryConsoleCommands oximetry_console_commands(
    oximetry_hub, oximetry_udp_source, oximetry_sensor_source,
    plx_peripheral, config_service);
static ReportConsoleCommands report_console_commands(report_task);
static ExportConsoleCommands export_console_commands(export_coordinator);
static ConfigConsoleCommands config_console_commands(config_service,
                                                     wifi_manager);
static OtaConsoleCommands ota_console_commands(firmware_installer,
                                               firmware_url_source,
                                               arduino_ota_source,
                                               update_checker,
                                               resmed_firmware_preparer,
                                               resmed_ota_manager,
                                               resmed_firmware_repository);
static WebDiagnosticsConsoleCommands web_console_commands(web_ui);
static ConsoleCommandGroup *console_command_groups[] = {
    &can_console_commands,
    &as11_device_console_commands,
    &rpc_console_commands,
    &stream_console_commands,
    &network_console_commands,
    &core_console_commands,
    &storage_console_commands,
    &runtime_console_commands,
    &edf_console_commands,
    &oximetry_console_commands,
    &report_console_commands,
    &export_console_commands,
    &config_console_commands,
    &ota_console_commands,
    &web_console_commands,
};
static ConsoleCommandRouter console_router;
#if AC_STACK_PROFILE_ENABLED
static StackProfiler stack_profiler;
#endif
static uint32_t report_catalog_seen_sessions_ended = 0;
static bool report_catalog_refresh_pending = true;
static uint32_t report_catalog_refresh_due_ms = 0;
static uint32_t report_catalog_timezone_revision = 0;
static uint32_t report_catalog_request_generation = 0;
static uint32_t rpc_transport_generation_seen = 0;
static ActivitySnapshot storage_activity;
static NetworkSnapshot runtime_network;
static bool runtime_activity_published = false;
static bool runtime_network_published = false;
static uint32_t export_config_due_ms = 0;
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MS = 30;
static constexpr uint32_t AC_MAIN_LOOP_CAN_DRAIN_WARN_MIN_INTERVAL_MS = 1000;
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

static void route_event_notification(void *context,
                                     const char *payload,
                                     size_t payload_len,
                                     uint32_t now_ms) {
    EventBroker *events = static_cast<EventBroker *>(context);
    if (!events) return;

    As11EventFrame frame;
    (void)events->publish_notification(payload, payload_len, now_ms, frame);
}

static void route_stream_notification(void *context,
                                      const char *payload,
                                      size_t payload_len,
                                      uint32_t now_ms) {
    StreamBroker *stream = static_cast<StreamBroker *>(context);
    if (!stream) return;
    (void)stream->publish_stream_data(payload, payload_len, now_ms);
}

static void route_spool_notification(void *context,
                                     const char *payload,
                                     size_t payload_len,
                                     uint32_t now_ms) {
    (void)now_ms;
    ReportSpoolService *spool = static_cast<ReportSpoolService *>(context);
    if (!spool) return;
    (void)spool->enqueue_notification(payload, payload_len);
}

static void route_tcp_raw_request(void *context,
                                  const char *payload,
                                  size_t payload_len,
                                  uint32_t now_ms) {
    StreamBroker *stream = static_cast<StreamBroker *>(context);
    if (!stream) return;

    stream->observe_external_request(payload, payload_len, now_ms);
}

static void sync_rpc_transport_generation(uint32_t now_ms) {
    const uint32_t generation = rpc_transport.transport_generation();
    if (generation == rpc_transport_generation_seen) return;

    event_broker.transport_reset(rpc_transport, now_ms);
    stream_broker.transport_reset(rpc_transport, now_ms);
    rpc_transport_generation_seen = generation;
}

#if AC_STACK_PROFILE_ENABLED
static void poll_stack_profiler(uint32_t now_ms) {
    static TaskHandle_t async_tcp_task = nullptr;
    if (!async_tcp_task) {
        async_tcp_task = xTaskGetHandle("async_tcp");
    }
    const uint32_t oxi_stack =
        oximetry_sensor_source.task_stack_high_water_bytes();

    StackProfileSample samples[] = {
        {StackProfileTask::Loop, true, uxTaskGetStackHighWaterMark(nullptr)},
        {StackProfileTask::AsyncTcp,
         async_tcp_task != nullptr,
         async_tcp_task ? uxTaskGetStackHighWaterMark(async_tcp_task) : 0},
        {StackProfileTask::ExportTask,
         true,
         export_task.stack_high_water_bytes()},
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

static void publish_runtime_activity(bool foreground_report_demand,
                                     bool realtime_stream_active,
                                     bool export_work_claimed,
                                     bool ota_install_active,
                                     bool therapy_active) {
    const bool changed =
        !runtime_activity_published ||
        storage_activity.foreground_report_demand != foreground_report_demand ||
        storage_activity.realtime_stream_active != realtime_stream_active ||
        storage_activity.export_work_claimed != export_work_claimed ||
        storage_activity.ota_install_active != ota_install_active ||
        storage_activity.therapy_active != therapy_active;
    if (!changed) return;

    storage_activity.foreground_report_demand = foreground_report_demand;
    storage_activity.realtime_stream_active = realtime_stream_active;
    storage_activity.export_work_claimed = export_work_claimed;
    storage_activity.ota_install_active = ota_install_active;
    storage_activity.therapy_active = therapy_active;
    storage_activity.generation++;
    if (storage_activity.generation == 0) storage_activity.generation++;
    runtime_activity_published = true;

    StorageService::publish_activity(storage_activity);
    report_task.publish_activity(storage_activity);
    export_task.publish_activity(storage_activity);
    storage_http_controller.publish_activity(storage_activity);
    storage_upload_http_controller.publish_activity(storage_activity);
    resmed_firmware_preparer.publish_activity(storage_activity);
    resmed_firmware_repository.publish_activity(storage_activity);
}

static void publish_runtime_network() {
    NetworkSnapshot next;
    const WifiModeState mode = wifi_manager.mode_state();

    next.associated = mode == WifiModeState::StaConnected ||
                      mode == WifiModeState::StaAssociated ||
                      mode == WifiModeState::StaRoamScanning;
    next.ipv4_ready = wifi_manager.sta_ipv4_online();
    next.management_reachable = wifi_manager.management_reachable();
    next.active_profile = wifi_manager.active_profile_index();
    if (next.associated) (void)wifi_manager.copy_bssid(next.bssid);

    const bool changed =
        !runtime_network_published ||
        runtime_network.associated != next.associated ||
        runtime_network.ipv4_ready != next.ipv4_ready ||
        runtime_network.management_reachable != next.management_reachable ||
        runtime_network.active_profile != next.active_profile ||
        memcmp(runtime_network.bssid, next.bssid,
               sizeof(runtime_network.bssid)) != 0;
    if (!changed) return;

    next.generation = runtime_network.generation + 1;
    if (next.generation == 0) next.generation = 1;
    runtime_network = next;
    runtime_network_published = true;
    export_task.publish_network(runtime_network);
}

static void poll_storage_upload_publication() {
    char path[AC_STORAGE_PATH_MAX] = {};
    if (StorageService::take_uploaded_path(path, sizeof(path))) {
        resmed_firmware_repository.notify_file_published(path);
    }
}

static void publish_export_config(uint32_t now_ms) {
    if (export_config_due_ms != 0 &&
        static_cast<int32_t>(now_ms - export_config_due_ms) < 0) {
        return;
    }

    export_task.publish_config(
        make_export_endpoint_config(config_service.data()));
    export_config_due_ms = now_ms + 1000;
    if (export_config_due_ms == 0) export_config_due_ms = 1;
}

static void sync_network_services() {
    const bool should_run_tcp =
        config_service.data().tcp_bridge_enabled &&
        wifi_manager.network_available();
    const bool should_run_telnet =
        config_service.data().telnet_console_enabled &&
        wifi_manager.network_available();

    if (should_run_tcp) {
        if (!tcp_bridge.started()) {
            tcp_bridge.begin(config_service.data().tcp_bridge_port);
        } else if (tcp_bridge.port() !=
                   config_service.data().tcp_bridge_port) {
            tcp_bridge.restart(config_service.data().tcp_bridge_port);
        }
    } else if (tcp_bridge.started()) {
        tcp_bridge.stop();
    }

    if (should_run_telnet) {
        if (!telnet_console.started()) {
            telnet_console.begin(config_service.data().telnet_console_port);
        } else if (telnet_console.port() !=
                   config_service.data().telnet_console_port) {
            telnet_console.restart(config_service.data().telnet_console_port,
                                   console_router);
        }
    } else if (telnet_console.started()) {
        telnet_console.stop(console_router);
    }
}

static void configure_oximetry(const AppConfigData &config) {
    oximetry_hub.set_enabled(config.oximetry_enabled);
    oximetry_udp_source.configure(config.oximetry_enabled,
                                  config.oximetry_udp_port);
    oximetry_sensor_source.configure(config.oximetry_enabled,
                                     config.hostname.c_str());
    plx_peripheral.configure(config.oximetry_enabled,
                             config.oximetry_advertise_mode,
                             config.hostname.c_str());
}

static const char *oximetry_source_name(OximetrySource source) {
    switch (source) {
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        case OximetrySource::None:
        default:
            return "none";
    }
}

static void poll_oximetry(bool network_available, uint32_t now_ms) {
    const OximetryHubSnapshot before = oximetry_hub.snapshot(now_ms);
    OximetryHubAction actions = oximetry_udp_source.poll(
        network_available, now_ms, oximetry_hub);

    BleSensorEvent event;
    while (oximetry_sensor_source.take_event(event)) {
        if (event.kind == BleSensorEventKind::Sample) {
            OximetryHubAction sample_actions = OximetryHubAction::None;
            (void)oximetry_hub.ingest(event.sample, now_ms, sample_actions);
            actions = actions | sample_actions;
        } else if (event.kind == BleSensorEventKind::Disconnected) {
            oximetry_hub.source_disconnected(OximetrySource::Ble);
        }
    }

    actions = actions | oximetry_hub.poll(now_ms);
    const OximetryHubSnapshot after = oximetry_hub.snapshot(now_ms);

    oximetry_sensor_source.set_auto_allowed(
        after.enabled &&
        (!after.source_present || after.source == OximetrySource::Ble));

    if (oximetry_action_has(actions, OximetryHubAction::DisconnectBleSensor)) {
        (void)oximetry_sensor_source.request_disconnect(true);
    }
    if (oximetry_action_has(actions, OximetryHubAction::SourceBecameStale)) {
        Log::logf(CAT_OXI, LOG_INFO, "source stale\n");
    }
    if (after.source_present &&
        (!before.source_present || before.source != after.source)) {
        Log::logf(CAT_OXI, LOG_INFO,
                  "source active type=%s detail=%s valid=%s\n",
                  oximetry_source_name(after.source),
                  after.source_detail[0] ? after.source_detail : "--",
                  after.reading.valid ? "yes" : "no");
    }

    plx_peripheral.poll(after, now_ms);
}

static void apply_config_runtime_effects(void *,
                                         const AppConfigData &config,
                                         uint32_t dirty) {
    bool reconnect_wifi = false;

    if (dirty & AC_CONFIG_DIRTY_HOSTNAME) {
        wifi_manager.set_hostname(config.hostname);
        arduino_ota_source.mark_config_dirty();
    }
    if (dirty & AC_CONFIG_DIRTY_SOFTAP) {
        const bool retry_sta =
            wifi_manager.mode_state() == WifiModeState::SoftAp &&
            wifi_manager.has_sta_config() &&
            config.softap_mode == SoftApMode::Auto;

        wifi_manager.set_softap_mode(config.softap_mode);
        wifi_manager.apply_softap_mode();
        reconnect_wifi = reconnect_wifi || retry_sta;
    }
    if (dirty & AC_CONFIG_DIRTY_WIFI_COUNTRY) {
        wifi_manager.set_country_code(config.wifi_country);
        reconnect_wifi = true;
    }
    if (dirty & AC_CONFIG_DIRTY_EDF_CAPTURE) {
        edf_recorder_manager.set_enabled(config.edf_capture_enabled);
    }
    if (dirty & (AC_CONFIG_DIRTY_HOSTNAME |
                 AC_CONFIG_DIRTY_OXIMETRY)) {
        configure_oximetry(config);
    }
    if (dirty & AC_CONFIG_DIRTY_OTA_PASSWORD) {
        arduino_ota_source.mark_config_dirty();
    }
    if (dirty & AC_CONFIG_DIRTY_UPDATE_URL) {
        update_checker.mark_config_dirty();
    }
    if (dirty & (AC_CONFIG_DIRTY_SMB_SYNC |
                 AC_CONFIG_DIRTY_SLEEPHQ_SYNC)) {
        export_config_due_ms = 0;
    }
    if (dirty & (AC_CONFIG_DIRTY_HTTP_AUTH |
                 AC_CONFIG_DIRTY_AUTH_WHITELIST)) {
        web_ui.apply_auth_config(config);
    }

    if (reconnect_wifi) wifi_manager.reconnect();
    if (dirty & (AC_CONFIG_DIRTY_TCP | AC_CONFIG_DIRTY_TELNET)) {
        sync_network_services();
    }
}

static uint32_t next_report_catalog_generation() {
    const uint32_t published =
        report_task.control_snapshot().catalog_generation;
    if (report_catalog_request_generation < published) {
        report_catalog_request_generation = published;
    }

    report_catalog_request_generation++;
    if (report_catalog_request_generation == 0) {
        report_catalog_request_generation = 1;
    }
    return report_catalog_request_generation;
}

static void poll_report_catalog_refresh(uint32_t now_ms) {
    const uint32_t sessions_ended = edf_recorder_manager.sessions_ended();
    if (sessions_ended != report_catalog_seen_sessions_ended) {
        report_catalog_seen_sessions_ended = sessions_ended;
        report_catalog_refresh_pending = true;
        report_catalog_refresh_due_ms = now_ms + 5000;
    }

    const uint32_t timezone_revision = time_sync_service.timezone_revision();
    if (timezone_revision != report_catalog_timezone_revision) {
        report_catalog_timezone_revision = timezone_revision;
        report_catalog_refresh_pending = true;
        report_catalog_refresh_due_ms = now_ms;
    }

    if (!report_catalog_refresh_pending) return;
    if (static_cast<int32_t>(now_ms - report_catalog_refresh_due_ms) < 0) {
        return;
    }

    const StorageWorkloadSnapshot storage =
        StorageService::workload_snapshot();

    if (!storage.valid || storage.busy || storage.edf_queued > 0 ||
        storage.open_file_count > 0) {
        report_catalog_refresh_due_ms = now_ms + 1000;
        return;
    }

    const bool offset_valid =
        as11_device_service.state().timezone_offset_valid();
    const int32_t offset_minutes = offset_valid
        ? as11_device_service.state().timezone_offset_minutes()
        : 0;
    const OperationAdmission admitted = report_task.request_catalog_refresh(
        offset_valid, offset_minutes, next_report_catalog_generation());
    if (admitted == OperationAdmission::Accepted) {
        report_catalog_refresh_pending = false;
    } else {
        report_catalog_refresh_due_ms = now_ms + 2000;
    }
}

static void drain_rpc_events() {
    RpcEvent event;
    while (rpc_transport.next_event(event)) {
        if (event.kind == RpcEventKind::BootNotification) {
            as11_device_service.device_reset(rpc_transport, millis());
            as11_settings_manager.device_reset(rpc_transport);
        }

        if (event.kind == RpcEventKind::RpcResponse &&
            event.source == RpcSource::Tcp && event.payload) {
            stream_broker.observe_external_response(
                event.payload->data(), event.payload->size(), millis());
        }

        // Framing failures already reach Serial and persistent sinks through
        // Log. Keep the event for Telnet and WebUI without printing it twice.
        if (event.kind != RpcEventKind::FramingError) {
            serial_management_console.handle_event(Serial, event);
        }
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
    const size_t drained = rpc_transport.drain_can_rx();
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

static void refresh_status_http_snapshot(uint32_t now_ms) {
    const uint32_t device_revision = as11_device_service.revision();
    const uint32_t config_revision = config_service.revision();
    if (!status_http_controller.refresh_due(device_revision,
                                            config_revision,
                                            now_ms)) {
        return;
    }

    const AppConfigData &config = config_service.data();
    const SystemStatusSnapshot snapshot = collect_system_status(
        {
            as11_device_service,
            wifi_manager,
            config,
            time_sync_service,
            firmware_installer,
            update_checker,
            oximetry_hub,
            oximetry_udp_source,
            plx_peripheral,
        },
        drain_can_rx_after);

    (void)status_http_controller.publish_snapshot(
        snapshot, config.hostname.c_str(), device_revision, config_revision);
}

void setup() {
    // Serial bootstrap
    Serial.begin(AC_SERIAL_BAUD);
    delay(500);
    while (Serial.available()) Serial.read();

    // Core services
    Memory::begin();
    Log::init();
    Log::bind_file_log_sink(StorageService::file_log_port());

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

    config_service.begin();

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
    if (!rpc_transport.reserve_reassembly_buffers()) {
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
    if (!console_router.begin(
            console_command_groups,
            sizeof(console_command_groups) /
                sizeof(console_command_groups[0]))) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] management CLI router failed to start\n");
    }
    serial_management_console.begin(Serial);
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] management CLI ready\n");

    // Export task
    const ExportEndpointConfig export_config =
        make_export_endpoint_config(config_service.data());

    const bool export_started = export_task.begin(
        export_config,
        StorageService::scan_port(),
        StorageService::read_port(),
        StorageService::stream_port(),
        StorageService::atomic_write_port(),
        StorageService::path_port());
    if (!export_started) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] export task failed to start\n");
    }

    export_coordinator.begin(export_task);
    export_http_controller.begin(export_coordinator);

    const bool storage_http_started = storage_http_controller.begin(
        StorageService::read_port(),
        StorageService::browser_port(),
        StorageService::archive_port(),
        StorageService::delete_port(),
        StorageService::status_port());
    if (!storage_http_started) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] storage HTTP controller failed to start\n");
    }

    const bool storage_upload_http_started =
        storage_upload_http_controller.begin(
            StorageService::upload_port(),
            StorageService::status_port());
    if (!storage_upload_http_started) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] storage upload HTTP controller failed to start\n");
    }

    if (!resmed_firmware_repository.begin(StorageService::scan_port(),
                                          StorageService::path_port())) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] ResMed firmware repository failed to start\n");
    }
    resmed_firmware_http_controller.begin(resmed_firmware_repository);
    if (!resmed_firmware_preparer.begin(StorageService::stream_port(),
                                        StorageService::upload_port(),
                                        StorageService::path_port())) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] ResMed firmware preparer failed to start\n");
    }

    apply_storage_provisioning(config_service,
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
    rpc_transport.set_event_notification_observer(route_event_notification,
                                                  &event_broker);
    rpc_transport.set_stream_notification_observer(route_stream_notification,
                                                   &stream_broker);
    rpc_transport.set_spool_notification_observer(route_spool_notification,
                                                  &report_spool_service);
    tcp_bridge.set_raw_request_observer(route_tcp_raw_request,
                                        &stream_broker);
    rpc_transport_generation_seen = rpc_transport.transport_generation();

    sink_manager.begin(stream_broker, as11_device_service.state(),
                       session_manager);

    edf_recorder_manager.begin(event_broker, stream_broker,
                               as11_device_service.state(), session_manager,
                               time_sync_service,
                               StorageService::atomic_write_port());
    edf_recorder_manager.set_enabled(
        config_service.data().edf_capture_enabled);

    const AppConfigData &config = config_service.data();
    if (!oximetry_ble_runtime.begin()) {
        Log::logf(CAT_OXI, LOG_ERROR, "BLE runtime mutex init failed\n");
    }
    oximetry_hub.set_enabled(config.oximetry_enabled);
    oximetry_udp_source.configure(config.oximetry_enabled,
                                  config.oximetry_udp_port);
    (void)oximetry_sensor_source.begin(config.oximetry_enabled,
                                       config.hostname.c_str());
    if (!plx_peripheral.begin(config.oximetry_enabled,
                              config.oximetry_advertise_mode,
                              config.hostname.c_str())) {
        Log::logf(CAT_OXI, LOG_ERROR,
                  "PLX BLE peripheral failed to start\n");
    }

    if (!report_spool_service.begin()) {
        Log::logf(CAT_REPORT, LOG_ERROR,
                  "report spool service failed to start\n");
    }
    if (!report_task.begin(StorageService::read_port(),
                           StorageService::atomic_write_port(),
                           StorageService::scan_port(),
                           report_spool_service,
                           StorageService::delete_port())) {
        Log::logf(CAT_REPORT, LOG_ERROR,
                  "report task failed to start\n");
    }
    report_http_controller.begin(report_task, StorageService::stream_port());

    if (!resmed_ota_manager.begin(rpc_transport, as11_device_service,
                                  StorageService::stream_port(),
                                  StorageService::path_port())) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ResMed OTA manager failed to start\n");
    }
    time_sync_service.begin(config_service.data(), wifi_manager, rpc_transport,
                            as11_device_service);
    report_catalog_timezone_revision = time_sync_service.timezone_revision();
    firmware_installer.begin();
    firmware_url_source.begin();
    arduino_ota_source.begin(config_service.data());
    update_checker.begin(config_service.data());
    if (!config_http_controller.begin(config_service)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] config HTTP controller failed to start\n");
    }
    if (!ota_http_controller.begin(firmware_installer, firmware_url_source,
                                   arduino_ota_source, update_checker,
                                   resmed_firmware_preparer,
                                   resmed_ota_manager)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] OTA HTTP controller failed to start\n");
    }
    if (!settings_http_controller.begin(rpc_transport,
                                        as11_device_service,
                                        as11_settings_manager)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] settings HTTP controller failed to start\n");
    }
    if (!device_http_controller.begin(rpc_transport,
                                      as11_device_service,
                                      time_sync_service)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] device HTTP controller failed to start\n");
    }
    if (!oximetry_http_controller.begin(oximetry_hub,
                                        oximetry_sensor_source,
                                        plx_peripheral,
                                        config_service)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] oximetry HTTP controller failed to start\n");
    }
    // Network configuration
    wifi_manager.set_hostname(config_service.data().hostname);
    wifi_manager.set_softap_mode(config_service.data().softap_mode);
    wifi_manager.set_country_code(config_service.data().wifi_country);

    // CAN and network frontends
    if (!can_driver.begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] CAN failed to start; management CLI still active "
                  "on serial\n");
    }

    wifi_manager.begin();
    if (!wifi_http_controller.begin(wifi_manager)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] Wi-Fi HTTP controller failed to start\n");
    }

    if (!config_service.data().tcp_bridge_enabled) {
        Log::logf(CAT_TCP, LOG_INFO,
                  "raw bridge disabled by config\n");
    }

    sync_network_services();

    config_service.set_runtime_effects(apply_config_runtime_effects, nullptr);
    config_service.activate_runtime_effects(false);

    if (!status_http_controller.begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] status HTTP controller failed to start\n");
    }
    refresh_status_http_snapshot(millis());

    if (!live_http_controller.begin(stream_broker, sink_manager)) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] live HTTP controller failed to start\n");
    }

    web_ui.begin(status_http_controller, live_http_controller, console_router,
                 config_service.data(),
                 web_route_modules,
                 sizeof(web_route_modules) / sizeof(web_route_modules[0]));
}

void loop() {
    const uint32_t now_ms = millis();

    // RPC and OTA ingress
    const bool esp_ota_quiesce_requested =
        firmware_installer.as11_quiesce_required();
    rpc_quiesce_coordinator.update(esp_ota_quiesce_requested, now_ms);

    const bool resmed_ota_transport_active =
        resmed_ota_manager.transport_active();

    const bool raw_tcp_connected = tcp_bridge.raw_client_connected();
    rpc_transport.set_raw_rpc_forwarding_enabled(raw_tcp_connected);
    stream_broker.set_external_transport_connected(raw_tcp_connected,
                                                   now_ms);

    rpc_transport.poll();
    sync_rpc_transport_generation(now_ms);
    stream_broker.poll(rpc_transport, now_ms);
    event_broker.poll(rpc_transport, now_ms,
                      resmed_ota_transport_active);
    as11_device_service.poll(
        rpc_transport, now_ms,
        esp_ota_quiesce_requested || resmed_ota_transport_active);
    resmed_firmware_preparer.publish_device_identifier(
        as11_device_service.state().software_identifier().c_str());
    as11_settings_manager.poll(
        rpc_transport, now_ms,
        esp_ota_quiesce_requested || resmed_ota_transport_active);
    config_http_controller.poll();
    settings_http_controller.poll();
    ota_http_controller.poll();
    device_http_controller.poll();
    oximetry_http_controller.poll();
    wifi_http_controller.poll();

    firmware_installer.poll_prepare(
        esp_ota_quiesce_requested &&
            rpc_quiesce_coordinator.complete(),
        esp_ota_quiesce_requested &&
            rpc_quiesce_coordinator.timed_out());

    drain_can_rx_after("rpc_ota_prepare");

    // Report RPC adapter and ResMed OTA
    report_spool_service.poll(
        rpc_transport.background_backpressure_active(),
        can_driver.stats().rx_queue_full_alerts);
    drain_can_rx_after("report");

    resmed_ota_manager.poll();
    if (!resmed_ota_manager.active()) {
        ResmedPreparedFirmware prepared_firmware;
        bool preparation_cancelled = false;
        if (resmed_firmware_preparer.take_result(
                prepared_firmware, preparation_cancelled)) {
            const bool accepted = preparation_cancelled
                ? resmed_ota_manager.discard_prepared_firmware(
                      prepared_firmware)
                : resmed_ota_manager.begin_prepared_upload(
                      prepared_firmware);
            if (!accepted) {
                Log::logf(CAT_OTA, LOG_WARN,
                          "[RESMED] prepared firmware handoff rejected\n");
            }
        }
    }
    drain_can_rx_after("resmed_ota");

    // RPC event fanout before services that depend on fresh state.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_pre_state");

    // Session and EDF capture
    session_manager.poll(as11_device_service.state(), now_ms);
    edf_recorder_manager.poll(now_ms);

    poll_report_catalog_refresh(now_ms);
    drain_can_rx_after("session_edf");

    // Live sinks and oximetry
    sink_manager.poll();
    poll_oximetry(wifi_manager.network_available(), now_ms);
    drain_can_rx_after("oximetry");

    // Wi-Fi and network services
    const bool stream_activity_active = stream_broker.activity_active(
        now_ms, AC_WIFI_ROAM_STREAM_QUIET_MS);

    wifi_manager.set_roaming_suspended(stream_activity_active ||
                                       firmware_installer.active() ||
                                       resmed_ota_transport_active);

    wifi_manager.poll();
    drain_can_rx_after("wifi.poll");

    publish_runtime_network();

    sync_network_services();
    drain_can_rx_after("network_services.sync");

    // Log and time services
    Log::poll(wifi_manager.sta_ipv4_online());
    drain_can_rx_after("log");

    if (!resmed_ota_transport_active) {
        time_sync_service.poll();
    }

    drain_can_rx_after("time_sync");

    // ESP/Arduino OTA
    const FirmwareInstallStatus install_status = firmware_installer.status();
    const bool esp_reboot_allowed =
        !install_status.reboot_pending ||
        rpc_quiesce_coordinator.reboot_allowed();

    const bool arduino_ota_poll_allowed =
        as11_device_service.state().therapy_state() !=
            As11TherapyState::Running;
    const ReportTaskControlSnapshot report_status =
        report_task.control_snapshot();
    const bool update_check_allowed =
        arduino_ota_poll_allowed &&
        !export_coordinator.endpoint_work_claimed() &&
        !report_status.foreground_active;

    update_checker.poll(runtime_network,
                        update_check_allowed &&
                            !resmed_ota_transport_active,
                        firmware_installer.active());
    arduino_ota_source.poll(runtime_network,
                            !resmed_ota_transport_active,
                            arduino_ota_poll_allowed);
    firmware_installer.poll(esp_reboot_allowed);

    drain_can_rx_after("arduino_ota");

    resmed_ota_manager.poll();
    drain_can_rx_after("resmed_ota_post");

    // Storage and exports
    const ExportReportActivity report_activity{
        report_status.foreground_active,
        report_status.background_active || report_catalog_refresh_pending,
    };

    const bool foreground_report_active = report_status.foreground_active;
    const bool export_work_claimed =
        export_coordinator.endpoint_work_claimed();
    const bool esp_ota_install_active = firmware_installer.active();
    const bool storage_ota_active =
        esp_ota_install_active || resmed_ota_manager.transport_active();
    const bool therapy_active =
        session_manager.status().state == SessionState::Active ||
        as11_device_service.state().therapy_state() ==
            As11TherapyState::Running;

    publish_runtime_activity(foreground_report_active,
                             stream_activity_active,
                             export_work_claimed,
                             storage_ota_active,
                             therapy_active);

    publish_export_config(now_ms);

    export_coordinator.poll(report_activity, storage_activity, now_ms);
    drain_can_rx_after("export_coordinator");

    poll_storage_upload_publication();
    resmed_firmware_repository.poll();
    drain_can_rx_after("resmed_firmware_repository");

    // Web, TCP, and console frontends
    refresh_status_http_snapshot(now_ms);
    drain_can_rx_after("status_http");

    web_ui.poll(drain_can_rx_after);
    drain_can_rx_after("web_ui");

    tcp_bridge.poll(rpc_transport);
    telnet_console.poll(config_service.data(), console_router);
    serial_management_console.poll(Serial, Serial, console_router);

    drain_can_rx_after("frontends");

    // RPC event fanout after network and console frontends.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_post_frontends");

    poll_stack_profiler(now_ms);

    delay(0);
}
