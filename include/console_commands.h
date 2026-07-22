#pragma once

#include "board_net.h"
#include "console_command_router.h"
#include "storage_read_port.h"

namespace aircannect {

static constexpr size_t AC_CONSOLE_COMMAND_SESSION_CAPACITY =
    AC_MAX_TELNET_CLIENTS + 2;

class As11DeviceService;
class As11SettingsManager;
class CanDriver;
class ConfigService;
class EdfRecorderManager;
class EventBroker;
class ExportCoordinator;
class ArduinoOtaSource;
class FirmwareInstaller;
class FirmwareUrlSource;
class OximetryManager;
class ReportTask;
class ResmedOtaManager;
class RpcDiagnosticsPort;
class RpcPassthroughPort;
class RpcRequestPort;
class SessionManager;
class SinkManager;
class StreamBroker;
class TcpBridge;
class TimeSyncService;
class UpdateChecker;
class WebUI;
class WifiManager;

class As11ConsoleCommands final : public ConsoleCommandGroup {
public:
    As11ConsoleCommands(RpcRequestPort &rpc,
                        RpcPassthroughPort &passthrough,
                        RpcDiagnosticsPort &diagnostics,
                        CanDriver &can,
                        EventBroker &events,
                        StreamBroker &stream,
                        As11DeviceService &device,
                        As11SettingsManager &settings,
                        TimeSyncService &time_sync);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void stop(ConsoleCommandSession &session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;
    void reset_stats() override;
    void print_memory_detail(Print &out) override;

private:
    struct StreamSessionState {
        uint32_t session_id = 0;
        int8_t handle = -1;
    };

    StreamSessionState *stream_session(uint32_t session_id, bool create);

    RpcRequestPort &rpc_;
    RpcPassthroughPort &passthrough_;
    RpcDiagnosticsPort &diagnostics_;
    CanDriver &can_;
    EventBroker &events_;
    StreamBroker &stream_;
    As11DeviceService &device_;
    As11SettingsManager &settings_;
    TimeSyncService &time_sync_;
    StreamSessionState stream_sessions_[AC_CONSOLE_COMMAND_SESSION_CAPACITY];
};

class CoreDiagnosticsConsoleCommands final : public ConsoleCommandGroup {
public:
    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_stats(Print &out) override;
    void print_memory_detail(Print &out) override;
};

class RuntimeConsoleCommands final : public ConsoleCommandGroup {
public:
    RuntimeConsoleCommands(SessionManager &session, SinkManager &sink);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &console_session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;

private:
    SessionManager &session_;
    SinkManager &sink_;
};

class EdfConsoleCommands final : public ConsoleCommandGroup {
public:
    EdfConsoleCommands(EdfRecorderManager &recorder, ConfigService &config);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;

private:
    EdfRecorderManager &recorder_;
    ConfigService &config_;
};

class OximetryConsoleCommands final : public ConsoleCommandGroup {
public:
    OximetryConsoleCommands(OximetryManager &oximetry,
                            ConfigService &config);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;
    void print_memory_detail(Print &out) override;

private:
    OximetryManager &oximetry_;
    ConfigService &config_;
};

class ReportConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit ReportConsoleCommands(ReportTask &report);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    ReportTask &report_;
    uint32_t request_generation_ = 0;
};

class StorageConsoleCommands final : public ConsoleCommandGroup {
public:
    StorageConsoleCommands(ConfigService &config,
                           StorageReadPort &storage_read);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void poll_pending(Print &out, ConsoleCommandSession &session) override;
    bool pending_output(
        const ConsoleCommandSession &session) const override;
    void cancel_pending(ConsoleCommandSession &session) override;
    void stop(ConsoleCommandSession &session) override;
    void print_stats(Print &out) override;

private:
    struct TailSessionState {
        uint32_t session_id = 0;
        OperationTicket ticket;
        StoragePreparedRead prepared;
        size_t offset = 0;
        uint32_t generation = 0;

        bool pending() const {
            return ticket.valid() || prepared.valid();
        }
    };

    TailSessionState *tail_session(uint32_t session_id, bool create);
    const TailSessionState *tail_session(uint32_t session_id) const;

    ConfigService &config_;
    StorageReadPort &storage_read_;
    TailSessionState tail_sessions_[AC_CONSOLE_COMMAND_SESSION_CAPACITY];
};

class ExportConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit ExportConsoleCommands(ExportCoordinator &exports);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    ExportCoordinator &exports_;
};

class NetworkConsoleCommands final : public ConsoleCommandGroup {
public:
    NetworkConsoleCommands(WifiManager &wifi, TcpBridge &tcp);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_stats(Print &out) override;

private:
    WifiManager &wifi_;
    TcpBridge &tcp_;
};

class ConfigConsoleCommands final : public ConsoleCommandGroup {
public:
    ConfigConsoleCommands(ConfigService &config, WifiManager &wifi);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    ConfigService &config_;
    WifiManager &wifi_;
};

class OtaConsoleCommands final : public ConsoleCommandGroup {
public:
    OtaConsoleCommands(FirmwareInstaller &installer,
                       FirmwareUrlSource &url_source,
                       ArduinoOtaSource &arduino_source,
                       UpdateChecker &update_checker,
                       ResmedOtaManager &resmed_ota);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    FirmwareInstaller &installer_;
    FirmwareUrlSource &url_source_;
    ArduinoOtaSource &arduino_source_;
    UpdateChecker &update_checker_;
    ResmedOtaManager &resmed_ota_;
};

class WebDiagnosticsConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit WebDiagnosticsConsoleCommands(WebUI &web_ui);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_memory_detail(Print &out) override;

private:
    WebUI &web_ui_;
};

}  // namespace aircannect
