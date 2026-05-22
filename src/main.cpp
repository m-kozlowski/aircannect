#include <Arduino.h>

#include "app_config.h"
#include "board.h"
#include "can_driver.h"
#include "debug_log.h"
#include "management_console.h"
#include "memory_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "provisioning.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
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
static OximetryManager oximetry_manager;
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
    oximetry_manager,
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
        if (is_rpc_event(event.kind)) {
            tcp_bridge.broadcast_rpc_payload(event.payload);
        }
    }
}

void setup() {
    Serial.begin(AC_SERIAL_BAUD);
    delay(500);
    while (Serial.available()) Serial.read();

    Memory::begin();
    Log::init();
    const MemoryStatus mem = Memory::status();
    Log::logf(CAT_GENERAL, LOG_INFO, "\n=== AirCANnect %s ===\n",
              aircannect_version());
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] build=%s\n",
              aircannect_build_date());
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
    Storage::begin();
    StorageWriter::begin();
    const StorageStatus storage = Storage::status();
    if (storage.mounted) {
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[INIT] storage=%s/%s free_bytes=%llu\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state),
                  static_cast<unsigned long long>(storage.free_bytes));
    } else if (storage.last_error[0]) {
        Log::logf(CAT_GENERAL, LOG_INFO,
                  "[INIT] storage=%s/%s error=%s\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state),
                  storage.last_error);
    } else {
        Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] storage=%s/%s\n",
                  Storage::type_name(storage.type),
                  Storage::state_name(storage.state));
    }

    serial_management_console.begin(Serial);
    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] management CLI ready\n");

    app_config.begin();
    apply_storage_provisioning(app_config, wifi_manager);
    app_config.apply_log_config();
    session_manager.begin();
    rpc_arbiter.set_stream_frame_observer(note_session_stream_frame,
                                          &session_manager);
    sink_manager.begin(rpc_arbiter, session_manager);
    oximetry_manager.begin(app_config);
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
                  "[TCP] raw bridge disabled by config\n");
    }
    sync_network_services();
    web_ui.begin(rpc_arbiter, wifi_manager, tcp_bridge, app_config,
                 time_sync_service, ota_manager, resmed_ota_manager,
                 session_manager, sink_manager, oximetry_manager,
                 console_ctx);

    Log::logf(CAT_GENERAL, LOG_INFO, "[INIT] architecture baseline ready\n");
}

void loop() {
    const bool resmed_ota_transport_active =
        resmed_ota_manager.transport_active();
    rpc_arbiter.set_background_polls_suspended(
        resmed_ota_transport_active);
    rpc_arbiter.poll();
    resmed_ota_manager.poll();
    // First drain handles events produced by CAN/RPC/OTA work before services
    // that depend on fresh state run below.
    drain_rpc_events();
    session_manager.poll(rpc_arbiter.as11_state(), millis());
    sink_manager.poll();
    oximetry_manager.poll(wifi_manager.network_available());
    wifi_manager.set_roaming_suspended(rpc_arbiter.stream_activity_active() ||
                                       ota_manager.active() ||
                                       resmed_ota_transport_active);
    wifi_manager.poll();
    sync_network_services();
    Log::poll(wifi_manager.network_available());
    if (!resmed_ota_transport_active) {
        time_sync_service.poll();
    }
    ota_manager.poll(wifi_manager);
    resmed_ota_manager.poll();
    Storage::poll();
    web_ui.poll();
    tcp_bridge.poll(rpc_arbiter);
    telnet_console.poll(console_ctx);
    serial_management_console.poll(Serial, Serial, console_ctx);
    // Second drain handles events produced by network and console frontends
    // during this loop turn.
    drain_rpc_events();
    StorageWriter::poll();

    if (resmed_ota_transport_active) {
        delay(0);
    } else {
        delay(2);
    }
}
