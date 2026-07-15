#pragma once

#include <Arduino.h>
#include <string>

#include "app_config.h"
#include "edf_recorder_manager.h"
#include "ota_manager.h"
#include "oximetry_manager.h"
#include "report_manager.h"
#include "resmed_ota_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "sleephq_sync_job.h"
#include "storage_sync_job.h"
#include "tcp_bridge.h"
#include "time_sync_service.h"
#include "wifi_manager.h"

namespace aircannect {

class ExportCoordinator;
class StorageDiagnosticJob;
class WebUI;

struct ConsoleContext {
    RpcArbiter &arbiter;
    TcpBridge &tcp_bridge;
    WifiManager &wifi_manager;
    AppConfig &app_config;

    TimeSyncService &time_sync_service;
    OtaManager &ota_manager;
    ResmedOtaManager &resmed_ota_manager;
    SessionManager &session_manager;
    SinkManager &sink_manager;
    EdfRecorderManager &edf_recorder_manager;
    OximetryManager &oximetry_manager;
    ReportManager &report_manager;
    StorageDiagnosticJob *storage_diagnostic_job = nullptr;
    StorageSyncJob *storage_sync_job = nullptr;
    SleepHqSyncJob *sleephq_sync_job = nullptr;
    ExportCoordinator *export_coordinator = nullptr;
    WebUI *web_ui = nullptr;
};

class ManagementConsole {
public:
    void begin(Print &out);
    void stop(RpcArbiter &arbiter);
    void poll(Stream &input, Print &out, ConsoleContext &ctx);
    void execute_line(String line, Print &out, ConsoleContext &ctx);

    void handle_event(Print &out, const RpcEvent &event);
    void print_help(Print &out, const String &topic = "");
    static bool event_has_output(const RpcEvent &event);

private:
    void handle_help_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_status_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_stats_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_memory_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_session_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_sink_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_edf_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_oximetry_command(Print &out, String rest,
                                 ConsoleContext &ctx);
    void handle_report_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_storage_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_smb_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_sleephq_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_as11_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_therapy_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_config_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_wifi_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_tcp_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_ota_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_resmed_ota_command(Print &out, String rest,
                                   ConsoleContext &ctx);
    void handle_log_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_restart_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_can_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_version_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_time_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_get_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_set_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_stream_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_rpc_command(Print &out, String rest, ConsoleContext &ctx);
    void handle_raw_command(Print &out, String rest, ConsoleContext &ctx);

    void handle_stream(Print &out, String rest, RpcArbiter &arbiter);
    void handle_as11(Print &out, String rest, RpcArbiter &arbiter);
    void handle_therapy(Print &out, String rest, RpcArbiter &arbiter);
    void handle_time(Print &out, String rest, RpcArbiter &arbiter,
                     TimeSyncService &time_sync_service);
    void handle_ota(Print &out, String rest, OtaManager &ota_manager,
                    ResmedOtaManager &resmed_ota_manager);
    void handle_resmed_ota(Print &out, String rest,
                           ResmedOtaManager &resmed_ota_manager);
    void handle_sink(Print &out, String rest, SinkManager &sink_manager);
    void handle_oximetry(Print &out, String rest,
                         OximetryManager &oximetry_manager);
    void handle_log(Print &out, String rest, AppConfig &app_config);
    void handle_wifi(Print &out, String rest, WifiManager &wifi_manager,
                     TcpBridge &tcp_bridge, const AppConfig &app_config);
    void handle_config(Print &out, String rest, AppConfig &app_config,
                       WifiManager &wifi_manager, TcpBridge &tcp_bridge,
                       OtaManager &ota_manager,
                       EdfRecorderManager &edf_recorder_manager);
    bool handle_config_key(Print &out, String rest, AppConfig &app_config,
                           WifiManager &wifi_manager, TcpBridge &tcp_bridge,
                           OtaManager &ota_manager,
                           EdfRecorderManager &edf_recorder_manager);

    void print_oximetry_status(Print &out,
                               const OximetryManager &oximetry_manager) const;

    void apply_runtime_config(const AppConfig &app_config,
                              WifiManager &wifi_manager,
                              TcpBridge &tcp_bridge);

    String line_;
    StreamConsumerHandle stream_handle_ = STREAM_CONSUMER_INVALID;
};

}  // namespace aircannect
