#include <Arduino.h>

#include <freertos/task.h>

#include <new>
#include <string.h>

#include "as11_ble_recovery.h"
#include "as11_ble_rpc_link.h"
#include "as11_device_service.h"
#include "as11_service_manager.h"
#include "as11_settings.h"
#include "as11_settings_manager.h"
#include "arduino_ota_source.h"
#include "ble_sensor_source.h"
#include "board.h"
#include "can_driver.h"
#include "can_rpc_link.h"
#include "config_http_controller.h"
#include "config_service.h"
#include "console_command_router.h"
#include "console_commands.h"
#include "debug_log.h"
#include "device_http_controller.h"
#include "display_manager.h"
#include "display_snapshot.h"
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
#include "local_input_controller.h"
#include "management_console.h"
#include "memory_manager.h"
#include "ota_http_controller.h"
#include "ble_runtime.h"
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
#include "rpc_link_selector.h"
#include "rpc_quiesce_coordinator.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "settings_http_controller.h"
#include "storage_http_controller.h"
#include "storage_upload_http_controller.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "status_http_controller.h"
#include "string_util.h"
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
static CanRpcLink can_rpc_link(can_driver);
static BleRuntime ble_runtime;
static As11BleRpcLink as11_ble_rpc_link(ble_runtime);
static RpcLinkSelector rpc_link_selector(can_rpc_link, as11_ble_rpc_link);
static EventBroker event_broker;
static StreamBroker stream_broker;
static RpcTransport rpc_transport(rpc_link_selector);
static As11ServiceManager as11_service_manager(can_driver);
static RpcQuiesceCoordinator rpc_quiesce_coordinator(
    rpc_transport, can_rpc_link, event_broker, stream_broker);
static As11DeviceService as11_device_service;
static As11BleRecovery as11_ble_recovery;
static As11SettingsManager as11_settings_manager;
static ManagementConsole serial_management_console;
static WifiManager wifi_manager;
static TcpBridge tcp_bridge(as11_service_manager);
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
static DisplayManager display_manager;
static LocalInputController local_inputs;
static SinkManager sink_manager;
static EdfRecorderManager edf_recorder_manager(rpc_transport);
static OximetryHub oximetry_hub;
static UdpOximeterSource oximetry_udp_source;
static BleSensorSource oximetry_sensor_source(ble_runtime);
static PlxPeripheral plx_peripheral(ble_runtime);
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
    rpc_transport, can_rpc_link, can_driver, event_broker, stream_broker);
static As11DeviceConsoleCommands as11_device_console_commands(
    rpc_transport, rpc_transport, as11_device_service, time_sync_service,
    as11_ble_rpc_link);
static RpcConsoleCommands rpc_console_commands(
    rpc_transport, rpc_transport, as11_device_service, as11_settings_manager);
static StreamConsoleCommands stream_console_commands(stream_broker);
static NetworkConsoleCommands network_console_commands(wifi_manager,
                                                       tcp_bridge);
static CoreDiagnosticsConsoleCommands core_console_commands;
static SystemConsoleCommands system_console_commands(
    firmware_installer, rpc_transport, as11_device_service);
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
    &system_console_commands,
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
static constexpr uint32_t AC_REPORT_CATALOG_SESSION_SETTLE_MS = 5000;
static constexpr uint32_t AC_REPORT_CATALOG_RECONCILE_IDLE_MS =
    5 * 60 * 1000;
static uint32_t report_catalog_seen_sessions_ended = 0;
static bool report_catalog_target_pending = false;
static NightCatalogRefreshTarget report_catalog_target;
static uint32_t report_catalog_target_due_ms = 0;
static uint32_t report_catalog_target_generation = 0;
static bool report_catalog_reconcile_pending = true;
static bool report_catalog_reconcile_is_post_therapy = false;
static uint32_t report_catalog_reconcile_due_ms = 0;
static uint32_t report_catalog_reconcile_generation = 0;
static uint32_t report_catalog_post_therapy_generation = 0;
static uint32_t report_catalog_timezone_revision = 0;
static uint32_t report_catalog_request_generation = 0;
static uint32_t rpc_transport_generation_seen = 0;
static ActivitySnapshot storage_activity;
static NetworkSnapshot runtime_network;
static bool runtime_activity_published = false;
static bool ota_storage_upload_active_published = false;
static bool runtime_network_published = false;
static uint32_t export_config_due_ms = 0;
static bool local_poweroff_requested = false;
static bool local_poweroff_attempted = false;
static bool local_as11_disconnect_requested = false;
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

static bool toggle_display_backlight(void *, uint32_t) {
    if (!display_manager.available()) return false;
    display_manager.toggle_backlight();
    return true;
}

static bool display_previous_page(void *, uint32_t) {
    return display_manager.previous_page();
}

static bool display_next_page(void *, uint32_t) {
    return display_manager.next_page();
}

static bool toggle_therapy(void *, uint32_t now_ms) {
    const As11TherapyState therapy =
        as11_device_service.state().therapy_state();
    const As11TherapyTarget target =
        therapy == As11TherapyState::Running
            ? As11TherapyTarget::Standby
            : As11TherapyTarget::Running;
    const OperationSubmission submission =
        as11_device_service.request_therapy(
            rpc_transport, target, RpcSource::Internal, now_ms);

    return submission.accepted();
}

static bool power_off_aircannect(void *, uint32_t) {
    if (!board_power_off_supported() || local_poweroff_requested ||
        firmware_installer.active() || resmed_ota_manager.active()) {
        return false;
    }

    local_poweroff_requested = true;
    local_poweroff_attempted = false;
    return true;
}

static bool restart_aircannect(void *, uint32_t) {
    if (firmware_installer.active() || resmed_ota_manager.active()) {
        return false;
    }

    firmware_installer.schedule_reboot(500);
    return true;
}

static bool trigger_export_sync(void *, uint32_t) {
    return export_coordinator.request_sync();
}

static bool disconnect_cpap(void *, uint32_t) {
    if (rpc_link_selector.selected() != As11Transport::Ble ||
        !rpc_link_selector.status().ready ||
        rpc_quiesce_coordinator.requested() ||
        firmware_installer.active() || resmed_ota_manager.active()) {
        return false;
    }

    local_as11_disconnect_requested = true;
    return true;
}

static void note_as11_device_event(void *context,
                                   const As11EventFrame &frame,
                                   uint32_t now_ms) {
    static_cast<As11DeviceService *>(context)->apply_activity_event_frame(
        frame, now_ms);
}

static void note_as11_ble_recovery_event(void *context,
                                         const As11EventFrame &frame,
                                         uint32_t now_ms) {
    (void)now_ms;
    static_cast<As11BleRecovery *>(context)->observe_event(frame);
}

static void note_as11_settings_history(void *context, uint32_t now_ms) {
    (void)now_ms;
    static_cast<As11SettingsManager *>(context)->note_history_change();
}

static void route_oximetry_sample(void *context,
                                  const OximetrySample &sample) {
    EdfRecorderManager *recorder =
        static_cast<EdfRecorderManager *>(context);
    if (!recorder) return;

    recorder->accept_oximetry_sample(sample);
}

static void route_event_notification(void *context,
                                     RpcPayloadView payload,
                                     uint32_t now_ms) {
    EventBroker *events = static_cast<EventBroker *>(context);
    if (!events) return;

    As11EventFrame frame;
    (void)events->publish_notification(payload, now_ms, frame);
}

static void route_stream_notification(void *context,
                                      RpcPayloadView payload,
                                      uint32_t now_ms) {
    StreamBroker *stream = static_cast<StreamBroker *>(context);
    if (!stream) return;

    (void)stream->publish_stream_data(payload, now_ms);
}

static void route_spool_notification(void *context,
                                     const RpcPayloadRef &payload,
                                     uint32_t now_ms) {
    (void)now_ms;
    ReportSpoolService *spool = static_cast<ReportSpoolService *>(context);
    if (!spool) return;

    (void)spool->enqueue_notification(payload);
}

static void route_tcp_raw_request(void *context,
                                  const char *payload,
                                  size_t payload_len,
                                  uint32_t now_ms) {
    StreamBroker *stream = static_cast<StreamBroker *>(context);
    if (!stream) return;

    stream->observe_external_request(RpcPayloadView(payload, payload_len),
                                     now_ms);
}

static void route_as11_service_frame(void *context,
                                     const RawCanFrame &frame,
                                     uint32_t now_ms) {
    As11ServiceManager *service =
        static_cast<As11ServiceManager *>(context);
    if (!service) return;

    service->accept_can_frame(frame, now_ms);
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
                                     bool ota_storage_upload_active,
                                     bool therapy_active,
                                     bool as11_rpc_available) {
    const bool changed =
        !runtime_activity_published ||
        storage_activity.foreground_report_demand != foreground_report_demand ||
        storage_activity.realtime_stream_active != realtime_stream_active ||
        storage_activity.export_work_claimed != export_work_claimed ||
        storage_activity.ota_install_active != ota_install_active ||
        ota_storage_upload_active_published != ota_storage_upload_active ||
        storage_activity.therapy_active != therapy_active ||
        storage_activity.as11_rpc_available != as11_rpc_available;
    if (!changed) return;

    storage_activity.foreground_report_demand = foreground_report_demand;
    storage_activity.realtime_stream_active = realtime_stream_active;
    storage_activity.export_work_claimed = export_work_claimed;
    storage_activity.ota_install_active = ota_install_active;
    storage_activity.therapy_active = therapy_active;
    storage_activity.as11_rpc_available = as11_rpc_available;
    ota_storage_upload_active_published = ota_storage_upload_active;
    storage_activity.generation++;
    if (storage_activity.generation == 0) storage_activity.generation++;
    runtime_activity_published = true;

    StorageService::publish_activity(storage_activity,
                                     ota_storage_upload_active);
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
    static char pending_path[AC_STORAGE_PATH_MAX] = {};
    if (!pending_path[0]) {
        (void)StorageService::take_uploaded_path(pending_path,
                                                 sizeof(pending_path));
    }
    if (pending_path[0] &&
        resmed_firmware_repository.consume_file_published(pending_path)) {
        pending_path[0] = '\0';
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
    const bool local_sa2 = config.as11_transport == As11Transport::Ble;

    edf_recorder_manager.set_sa2_input(
        local_sa2 ? EdfSa2Input::LocalOximetry
                  : EdfSa2Input::As11Stream);
    oximetry_hub.set_enabled(config.oximetry_enabled);
    oximetry_udp_source.configure(config.oximetry_enabled,
                                  config.oximetry_udp_port);
    oximetry_sensor_source.configure(config.oximetry_enabled,
                                     config.hostname.c_str());
    plx_peripheral.configure(config.oximetry_enabled, !local_sa2,
                             config.oximetry_advertise_mode,
                             config.hostname.c_str());
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
    const PlxPeripheralStatus peripheral = plx_peripheral.status(now_ms);

    sink_manager.update_local_oximetry(after, peripheral.subscribed);
}

static bool store_as11_ble_credentials(void *context,
                                       const char *address,
                                       const char *client_id,
                                       const char *master_key_hex) {
    auto *config = static_cast<ConfigService *>(context);
    return config && config->replace_as11_ble_credentials(
                         address, client_id, master_key_hex);
}

static bool configure_as11_transport(const AppConfigData &config) {
    if (!as11_transport_supported(config.as11_transport)) return false;

    const bool use_ble = config.as11_transport == As11Transport::Ble;

    as11_ble_rpc_link.configure(
        use_ble, config.hostname.c_str(), config.as11_ble_address.c_str(),
        config.as11_ble_client_id.c_str(),
        config.as11_ble_master_key.c_str());

    const bool selected = rpc_link_selector.select(config.as11_transport);
    time_sync_service.note_as11_connection_reset();
    if (use_ble) {
        resmed_ota_manager.set_can_available(false);
        as11_service_manager.set_available(false);
        can_rpc_link.set_application_enabled(false);
        const bool stopped = can_rpc_link.set_physical_enabled(false);
        return selected && stopped;
    }

    const bool started = can_rpc_link.set_physical_enabled(true);
    can_rpc_link.set_application_enabled(started);
    as11_service_manager.set_available(started);
    resmed_ota_manager.set_can_available(started);
    return selected && started;
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
    if (dirty & AC_CONFIG_DIRTY_KEYBINDINGS) {
        if (!local_inputs.apply_config(config.keybindings, millis())) {
            Log::logf(CAT_CONFIG, LOG_ERROR,
                      "failed to apply local input bindings\n");
        }
    }
    if (dirty & AC_CONFIG_DIRTY_DISPLAY) {
        display_manager.configure(config.display_orientation,
                                  config.display_auto_rotate);
    }
    if (dirty & (AC_CONFIG_DIRTY_AS11_TRANSPORT |
                 AC_CONFIG_DIRTY_HOSTNAME)) {
        if (!configure_as11_transport(config)) {
            Log::logf(CAT_RPC, LOG_ERROR,
                      "AS11 transport failed to start mode=%s\n",
                      as11_transport_name(config.as11_transport));
        }
    }
    if (dirty & (AC_CONFIG_DIRTY_HOSTNAME |
                 AC_CONFIG_DIRTY_OXIMETRY |
                 AC_CONFIG_DIRTY_AS11_TRANSPORT)) {
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

static bool report_catalog_generation_reached(uint32_t completed,
                                              uint32_t requested) {
    if (requested == 0) return true;
    return static_cast<int32_t>(completed - requested) >= 0;
}

static void poll_report_catalog_refresh(uint32_t now_ms) {
    const uint32_t sessions_ended = edf_recorder_manager.sessions_ended();
    if (sessions_ended != report_catalog_seen_sessions_ended) {
        report_catalog_seen_sessions_ended = sessions_ended;

        EdfCatalogRefreshHint hint;
        report_catalog_target = {};
        report_catalog_target_pending =
            edf_recorder_manager.latest_catalog_refresh_hint(hint);
        if (report_catalog_target_pending) {
            report_catalog_target.sleep_day = hint.sleep_day;
            copy_cstr(report_catalog_target.datalog_sleep_day,
                      sizeof(report_catalog_target.datalog_sleep_day),
                      hint.datalog_sleep_day);
            report_catalog_target_due_ms =
                now_ms + AC_REPORT_CATALOG_SESSION_SETTLE_MS;
            report_catalog_target_generation = 0;
        }

        report_catalog_reconcile_pending = true;
        report_catalog_reconcile_is_post_therapy =
            !report_catalog_target_pending;
        report_catalog_reconcile_due_ms = now_ms +
            (report_catalog_target_pending
                 ? AC_REPORT_CATALOG_RECONCILE_IDLE_MS
                 : AC_REPORT_CATALOG_SESSION_SETTLE_MS);
        report_catalog_reconcile_generation = 0;
        report_catalog_post_therapy_generation = 0;
    }

    const uint32_t timezone_revision = time_sync_service.timezone_revision();
    if (timezone_revision != report_catalog_timezone_revision) {
        report_catalog_timezone_revision = timezone_revision;
        report_catalog_reconcile_pending = true;
        report_catalog_reconcile_due_ms = now_ms;
        report_catalog_reconcile_generation = 0;
    }

    const ReportTaskControlSnapshot report_status =
        report_task.control_snapshot();
    if (report_catalog_post_therapy_generation != 0 &&
        report_status.catalog_refresh_generation ==
            report_catalog_post_therapy_generation &&
        report_status.catalog_refresh_state ==
            NightCatalogRefreshState::Error &&
        !report_status.catalog_refresh_retryable) {
        report_catalog_post_therapy_generation = 0;
        report_catalog_reconcile_pending = true;
        report_catalog_reconcile_is_post_therapy = true;
        report_catalog_reconcile_due_ms = now_ms;
        report_catalog_reconcile_generation = 0;
    }

    const bool target_due = report_catalog_target_pending &&
        static_cast<int32_t>(now_ms - report_catalog_target_due_ms) >= 0;
    const bool target_settle_active =
        report_catalog_post_therapy_generation != 0 &&
        !report_catalog_generation_reached(
            report_status.durable_catalog_generation,
            report_catalog_post_therapy_generation);
    const bool reconcile_due = !report_catalog_target_pending &&
        !target_settle_active &&
        report_catalog_reconcile_pending &&
        static_cast<int32_t>(now_ms - report_catalog_reconcile_due_ms) >= 0;
    if (!target_due && !reconcile_due) {
        return;
    }

    const StorageWorkloadSnapshot storage =
        StorageService::workload_snapshot();

    if (!storage.valid || storage.busy || storage.edf_queued > 0 ||
        storage.open_file_count > 0) {
        if (target_due) {
            report_catalog_target_due_ms = now_ms + 1000;
        } else {
            report_catalog_reconcile_due_ms = now_ms + 1000;
        }
        return;
    }

    const bool offset_valid =
        as11_device_service.state().timezone_offset_valid();
    const int32_t offset_minutes = offset_valid
        ? as11_device_service.state().timezone_offset_minutes()
        : 0;
    uint32_t &generation = target_due
        ? report_catalog_target_generation
        : report_catalog_reconcile_generation;
    if (generation == 0) generation = next_report_catalog_generation();

    const OperationAdmission admitted = report_task.request_catalog_refresh(
        offset_valid,
        offset_minutes,
        generation,
        target_due ? report_catalog_target : NightCatalogRefreshTarget{});
    if (admitted == OperationAdmission::Accepted) {
        if (target_due) {
            report_catalog_target_pending = false;
            report_catalog_target = {};
            report_catalog_target_generation = 0;
            report_catalog_post_therapy_generation = generation;
        } else {
            report_catalog_reconcile_pending = false;
            report_catalog_reconcile_generation = 0;
            if (report_catalog_reconcile_is_post_therapy) {
                report_catalog_post_therapy_generation = generation;
                report_catalog_reconcile_is_post_therapy = false;
            }
        }
    } else {
        if (target_due) {
            report_catalog_target_due_ms = now_ms + 2000;
        } else {
            report_catalog_reconcile_due_ms = now_ms + 2000;
        }
    }
}

static void drain_rpc_events() {
    RpcEvent event;
    while (rpc_transport.next_event(event)) {
        if (event.kind == RpcEventKind::BootNotification) {
            as11_service_manager.note_device_boot(millis());
            as11_device_service.device_reset(rpc_transport, millis());
            time_sync_service.note_as11_connection_reset();
            rpc_transport.set_as11_unavailable(false);
            as11_settings_manager.device_reset(rpc_transport);
        }

        if (event.kind == RpcEventKind::RpcResponse &&
            event.source == RpcSource::Tcp && event.payload) {
            stream_broker.observe_external_response(
                rpc_payload_view(event.payload), millis());
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

static void poll_as11_ble_recovery(uint32_t now_ms) {
    const bool ble_selected =
        rpc_link_selector.selected() == As11Transport::Ble;
    const bool authenticated =
        ble_selected && as11_ble_rpc_link.ble_status().authenticated;

    as11_ble_recovery.observe_link(
        ble_selected, authenticated, as11_device_service.state(), now_ms);
    if (rpc_quiesce_coordinator.requested()) return;

    as11_ble_recovery.poll(now_ms);

    const As11BleRecoveryActions actions =
        as11_ble_recovery.take_actions();
    if (actions.boot_confirmed) {
        rpc_transport.accept_boot_notification(
            "AS11 PowerUp after BLE reconnect");
        return;
    }
    if (!actions.refresh) return;

    rpc_transport.set_as11_unavailable(false);
    time_sync_service.note_as11_connection_reset();
    (void)as11_device_service.request_healthcheck(
        rpc_transport, RpcSource::Scheduler, now_ms);
    as11_settings_manager.invalidate(
        rpc_transport, RpcSource::Scheduler, now_ms);
}

static bool main_loop_drain_timing_active() {
    return session_manager.status().state == SessionState::Active ||
           as11_device_service.state().therapy_state() ==
               As11TherapyState::Running;
}

static void drain_can_side_events() {
    CanSideEvent event;
    while (can_rpc_link.take_side_event(event)) {
        switch (event.kind) {
            case CanSideEventKind::DebugPayload:
                rpc_transport.accept_debug_payload(event.payload);
                break;
            case CanSideEventKind::DebugFramingError:
                rpc_transport.accept_debug_framing_error(event.detail.c_str());
                break;
            case CanSideEventKind::BootNotification:
                rpc_transport.accept_boot_notification(event.detail.c_str());
                break;
            case CanSideEventKind::ApplicationReset:
                if (rpc_link_selector.can_selected()) {
                    rpc_transport.accept_link_reset(event.detail.c_str());
                }
                break;
        }
    }
}

static void drain_can_rx_after(const char *section) {
    static uint32_t last_checkpoint_ms = 0;
    static uint32_t last_warn_ms = 0;

    if (!can_rpc_link.physical_enabled()) {
        last_checkpoint_ms = 0;
        return;
    }

    const uint32_t before_ms = millis();
    const uint32_t gap_ms = last_checkpoint_ms == 0
                                ? 0
                                : before_ms - last_checkpoint_ms;
    const size_t drained = can_rpc_link.drain_rx();
    drain_can_side_events();
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
    SystemStatusSnapshot snapshot = collect_system_status(
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

    snapshot.as11.transport = rpc_link_selector.selected();
    if (snapshot.as11.transport == As11Transport::Ble) {
        const As11BleLinkStatus link = as11_ble_rpc_link.ble_status();
        snapshot.as11.link_state = as11_ble_link_state_name(link.state);
        snapshot.as11.link_connected = link.connected;
        snapshot.as11.link_authenticated = link.authenticated;
        snapshot.as11.link_rssi = link.rssi;
        strncpy(snapshot.as11.link_error, link.error,
                sizeof(snapshot.as11.link_error) - 1);
    }

    if (display_manager.available()) {
        const int therapy_mode = as11_mode_index_from_value(
            as11_device_service.state().active_therapy_profile());
        const DisplaySnapshot display_snapshot = compose_display_snapshot(
            snapshot,
            session_manager.status(),
            sink_manager.therapy_telemetry_snapshot(snapshot.now_ms),
            export_coordinator.status_snapshot(),
            therapy_mode,
            report_task.display_summary_snapshot());
        display_manager.publish(display_snapshot);
    }

    (void)status_http_controller.publish_snapshot(
        snapshot, config.hostname.c_str(), device_revision, config_revision);
}

void setup() {
    board_power_begin();

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
    as11_ble_rpc_link.set_credential_store(
        store_as11_ble_credentials, &config_service);
    time_sync_service.initialize_timezone(config_service.data());

    if (!ble_runtime.begin()) {
        Log::logf(CAT_BLE, LOG_ERROR, "runtime init failed\n");
    }

    display_manager.configure(config_service.data().display_orientation,
                              config_service.data().display_auto_rotate);
    if (!display_manager.begin()) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] display manager failed to start\n");
    }

    (void)local_inputs.register_action(
        LocalActionId::DisplayPreviousPage,
        display_previous_page, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::DisplayNextPage,
        display_next_page, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::DisplayToggleBacklight,
        toggle_display_backlight, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::TherapyToggle, toggle_therapy, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::PowerOff, power_off_aircannect, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::RestartAirCANnect, restart_aircannect, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::TriggerSync, trigger_export_sync, nullptr);
    (void)local_inputs.register_action(
        LocalActionId::DisconnectCpap, disconnect_cpap, nullptr);
    if (!local_inputs.begin() ||
        !local_inputs.apply_config(config_service.data().keybindings,
                                   millis())) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] local inputs failed to start\n");
    }

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
        StorageService::path_port(),
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
                                          StorageService::read_port(),
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
    (void)event_broker.add_frame_observer(
        note_as11_ble_recovery_event, &as11_ble_recovery);
    event_broker.set_settings_history_observer(
        note_as11_settings_history, &as11_settings_manager);
    rpc_transport.set_event_notification_observer(route_event_notification,
                                                  &event_broker);
    rpc_transport.set_stream_notification_observer(route_stream_notification,
                                                   &stream_broker);
    rpc_transport.set_spool_notification_observer(route_spool_notification,
                                                  &report_spool_service);
    can_rpc_link.set_service_frame_observer(
        route_as11_service_frame, &as11_service_manager);
    tcp_bridge.set_raw_request_observer(route_tcp_raw_request,
                                        &stream_broker);
    rpc_transport_generation_seen = rpc_transport.transport_generation();

    sink_manager.begin(stream_broker, as11_device_service.state(),
                       session_manager);
    sink_manager.set_therapy_telemetry_enabled(display_manager.available());

    if (!report_spool_service.begin()) {
        Log::logf(CAT_REPORT, LOG_ERROR,
                  "report spool service failed to start\n");
    }

    edf_recorder_manager.begin(event_broker, stream_broker,
                               as11_device_service.state(), session_manager,
                               time_sync_service,
                               StorageService::read_port(),
                               StorageService::atomic_write_port(),
                               StorageService::path_port(),
                               report_spool_service);
    oximetry_hub.set_sample_observer(route_oximetry_sample,
                                     &edf_recorder_manager);
    edf_recorder_manager.set_enabled(
        config_service.data().edf_capture_enabled);

    const AppConfigData &config = config_service.data();
    const bool local_sa2 = config.as11_transport == As11Transport::Ble;

    edf_recorder_manager.set_sa2_input(
        local_sa2 ? EdfSa2Input::LocalOximetry
                  : EdfSa2Input::As11Stream);
    oximetry_hub.set_enabled(config.oximetry_enabled);
    oximetry_udp_source.configure(config.oximetry_enabled,
                                  config.oximetry_udp_port);
    (void)oximetry_sensor_source.begin(config.oximetry_enabled,
                                       config.hostname.c_str());
    if (!plx_peripheral.begin(config.oximetry_enabled, !local_sa2,
                              config.oximetry_advertise_mode,
                              config.hostname.c_str())) {
        Log::logf(CAT_OXI, LOG_ERROR,
                  "PLX BLE peripheral failed to start\n");
    }

    if (!as11_ble_recovery.begin(report_spool_service)) {
        Log::logf(CAT_BLE, LOG_ERROR,
                  "AS11 reconnect recovery failed to start\n");
    }
    if (!report_task.begin(StorageService::read_port(),
                           StorageService::atomic_write_port(),
                           StorageService::scan_port(),
                           report_spool_service,
                           StorageService::delete_port())) {
        Log::logf(CAT_REPORT, LOG_ERROR,
                  "report task failed to start\n");
    }
    report_http_controller.begin(report_task);

    if (!resmed_ota_manager.begin(rpc_transport, as11_device_service,
                                  as11_service_manager,
                                  resmed_firmware_preparer,
                                  StorageService::stream_port(),
                                  StorageService::path_port(),
                                  StorageService::upload_port(),
                                  config.as11_ota_key)) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "ResMed OTA manager failed to start\n");
    }
    time_sync_service.begin(config_service.data(), wifi_manager, rpc_transport,
                            as11_device_service);
    report_catalog_timezone_revision = time_sync_service.timezone_revision();
    report_catalog_reconcile_due_ms =
        millis() + AC_REPORT_CATALOG_RECONCILE_IDLE_MS;
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
                                      time_sync_service,
                                      as11_ble_rpc_link)) {
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

    // AS11 transport and network frontends
    if (!configure_as11_transport(config_service.data())) {
        Log::logf(CAT_GENERAL, LOG_ERROR,
                  "[INIT] AS11 transport failed to start; management CLI "
                  "still active on serial\n");
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
    const bool esp_reboot_pending = firmware_installer.reboot_pending();
    const bool as11_service_exclusive =
        as11_service_manager.exclusive_requested();
    const bool local_shutdown_requested =
        local_poweroff_requested || local_as11_disconnect_requested;
    const bool as11_application_quiesce_requested =
        esp_ota_quiesce_requested || as11_service_exclusive ||
        local_shutdown_requested;
    rpc_quiesce_coordinator.update(
        as11_application_quiesce_requested,
        esp_reboot_pending || local_shutdown_requested,
        now_ms);

    const bool resmed_ota_transport_active =
        resmed_ota_manager.transport_active();

    const bool raw_tcp_connected = tcp_bridge.raw_client_connected();
    rpc_transport.set_raw_rpc_forwarding_enabled(raw_tcp_connected);
    stream_broker.set_external_transport_connected(raw_tcp_connected,
                                                   now_ms);

    can_rpc_link.poll_physical(now_ms);
    rpc_transport.poll();
    const bool as11_link_ready = rpc_link_selector.status().ready;
    poll_as11_ble_recovery(now_ms);
    drain_can_side_events();
    as11_service_manager.poll_entry(
        rpc_transport, rpc_quiesce_coordinator.complete(),
        rpc_quiesce_coordinator.timed_out(), now_ms);
    as11_service_manager.poll(now_ms);
    sync_rpc_transport_generation(now_ms);
    stream_broker.poll(rpc_transport, now_ms);
    event_broker.poll(rpc_transport, now_ms,
                      resmed_ota_transport_active ||
                          !as11_link_ready ||
                          as11_device_service.unavailable());
    as11_device_service.poll(
        rpc_transport, now_ms,
        as11_application_quiesce_requested ||
            resmed_ota_transport_active || !as11_link_ready);
    const bool as11_device_unavailable =
        as11_device_service.unavailable();
    const bool as11_rpc_available =
        as11_link_ready && !as11_device_unavailable;
    rpc_transport.set_as11_unavailable(!as11_rpc_available);

    resmed_firmware_preparer.publish_device_identifier(
        as11_device_service.state().software_identifier().c_str());
    as11_settings_manager.poll(
        rpc_transport, now_ms,
        as11_application_quiesce_requested ||
            resmed_ota_transport_active || !as11_rpc_available);
    config_http_controller.poll();
    settings_http_controller.poll();
    ota_http_controller.poll();
    device_http_controller.poll();
    oximetry_http_controller.poll();
    wifi_http_controller.poll();

    firmware_installer.poll_prepare(
        esp_ota_quiesce_requested &&
            !as11_service_exclusive &&
            rpc_quiesce_coordinator.complete(),
        esp_ota_quiesce_requested &&
            !as11_service_exclusive &&
            rpc_quiesce_coordinator.timed_out());

    drain_can_rx_after("rpc_ota_prepare");

    // Report RPC adapter and ResMed OTA
    report_spool_service.poll(
        as11_rpc_available,
        rpc_transport.background_backpressure_active(),
        can_driver.stats().rx_queue_full_alerts);
    poll_as11_ble_recovery(now_ms);
    drain_can_rx_after("report");

    resmed_ota_manager.poll();
    drain_can_rx_after("resmed_ota");

    // Oximetry samples must reach the recorder before a closing ZLE event.
    poll_oximetry(wifi_manager.network_available(), now_ms);
    drain_can_rx_after("oximetry");

    // RPC event fanout before services that depend on fresh state.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_pre_state");

    // Session and EDF capture
    session_manager.poll(as11_device_service.state(), now_ms);

    local_inputs.poll(now_ms);
    drain_can_rx_after("session");

    edf_recorder_manager.poll(now_ms);
    drain_can_rx_after("edf");

    poll_report_catalog_refresh(now_ms);
    drain_can_rx_after("report_catalog");

    // Live sinks
    sink_manager.poll(now_ms);
    TherapyTelemetrySnapshot telemetry;
    if (display_manager.available() &&
        sink_manager.take_therapy_telemetry_update(now_ms, telemetry)) {
        const int therapy_mode = as11_mode_index_from_value(
            as11_device_service.state().active_therapy_profile());
        display_manager.publish_therapy_telemetry(
            telemetry, therapy_mode);
    }

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

    if (!resmed_ota_transport_active &&
        !as11_application_quiesce_requested) {
        time_sync_service.poll();
    }

    drain_can_rx_after("time_sync");

    // ESP/Arduino OTA
    const FirmwareInstallStatus install_status = firmware_installer.status();
    const bool esp_reboot_allowed =
        !install_status.reboot_pending ||
        rpc_quiesce_coordinator.reboot_allowed();

    if (local_poweroff_requested && !local_poweroff_attempted &&
        rpc_quiesce_coordinator.shutdown_allowed()) {
        local_poweroff_attempted = true;
        Log::logf(CAT_GENERAL, LOG_INFO, "[POWER] powering off\n");
        delay(50);
        (void)board_power_off();
    }

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
    ExportReportActivity report_activity;
    report_activity.foreground_active = report_status.foreground_active;
    report_activity.background_active =
        report_status.background_active || report_catalog_target_pending;
    report_activity.post_therapy_settle_pending =
        report_catalog_target_pending ||
        report_catalog_reconcile_is_post_therapy ||
        !report_catalog_generation_reached(
            report_status.durable_catalog_generation,
            report_catalog_post_therapy_generation);

    const bool foreground_report_active = report_status.foreground_active;
    const bool export_work_claimed =
        export_coordinator.endpoint_work_claimed();
    const bool esp_ota_install_active = firmware_installer.active();
    const bool storage_ota_active =
        esp_ota_install_active || resmed_ota_manager.transport_active();
    const bool ota_storage_upload_active =
        resmed_ota_manager.storage_upload_active();
    const bool therapy_active =
        session_manager.status().state == SessionState::Active ||
        as11_device_service.state().therapy_state() ==
            As11TherapyState::Running;

    publish_runtime_activity(foreground_report_active,
                             stream_activity_active,
                             export_work_claimed,
                             storage_ota_active,
                             ota_storage_upload_active,
                             therapy_active,
                             as11_rpc_available);

    publish_export_config(now_ms);

    export_coordinator.poll(report_activity, storage_activity, now_ms);
    drain_can_rx_after("export_coordinator");

    poll_storage_upload_publication();
    resmed_firmware_repository.poll();
    drain_can_rx_after("resmed_firmware_repository");

    // Web, TCP, and console frontends
    refresh_status_http_snapshot(now_ms);
    drain_can_rx_after("status_http");

    report_http_controller.poll();
    drain_can_rx_after("report_http");

    storage_http_controller.poll();
    drain_can_rx_after("storage_http");

    web_ui.poll(drain_can_rx_after);
    drain_can_rx_after("web_ui");

    const bool service_entry_allowed =
        !as11_application_quiesce_requested &&
        !firmware_installer.active() &&
        !resmed_ota_manager.active();
    tcp_bridge.poll(rpc_transport, service_entry_allowed);
    telnet_console.poll(config_service.data(), console_router);
    serial_management_console.poll(Serial, Serial, console_router);

    drain_can_rx_after("frontends");

    // RPC event fanout after network and console frontends.
    drain_rpc_events();
    drain_can_rx_after("rpc_events_post_frontends");

    poll_stack_profiler(now_ms);

    delay(0);
}
