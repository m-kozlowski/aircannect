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
class BleSensorSource;
class OximetryHub;
class PlxPeripheral;
class ReportTask;
class ResmedFirmwareRepository;
class ResmedOtaManager;
class RpcDiagnosticsPort;
class RpcPassthroughPort;
class RpcRequestPort;
class SessionManager;
class SinkManager;
class StreamBroker;
class TcpBridge;
class TimeSyncService;
class UdpOximeterSource;
class UpdateChecker;
class WebUI;
class WifiManager;

class As11DeviceConsoleCommands final : public ConsoleCommandGroup {
public:
    As11DeviceConsoleCommands(RpcRequestPort &rpc,
                              RpcPassthroughPort &passthrough,
                              As11DeviceService &device,
                              TimeSyncService &time_sync);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_status(Print &out) override;

private:
    RpcRequestPort &rpc_;
    RpcPassthroughPort &passthrough_;
    As11DeviceService &device_;
    TimeSyncService &time_sync_;
};

class RpcConsoleCommands final : public ConsoleCommandGroup {
public:
    RpcConsoleCommands(RpcRequestPort &rpc,
                       RpcPassthroughPort &passthrough,
                       As11DeviceService &device,
                       As11SettingsManager &settings);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    RpcRequestPort &rpc_;
    RpcPassthroughPort &passthrough_;
    As11DeviceService &device_;
    As11SettingsManager &settings_;
};

class StreamConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit StreamConsoleCommands(StreamBroker &stream);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void stop(ConsoleCommandSession &session) override;
    void print_memory_detail(Print &out) override;

private:
    struct StreamSessionState {
        uint32_t session_id = 0;
        int8_t handle = -1;
    };

    StreamSessionState *stream_session(uint32_t session_id, bool create);

    StreamBroker &stream_;
    StreamSessionState stream_sessions_[AC_CONSOLE_COMMAND_SESSION_CAPACITY];
};

class CanConsoleCommands final : public ConsoleCommandGroup {
public:
    CanConsoleCommands(RpcDiagnosticsPort &diagnostics,
                       CanDriver &can,
                       EventBroker &events,
                       StreamBroker &stream);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;
    void reset_stats() override;

private:
    RpcDiagnosticsPort &diagnostics_;
    CanDriver &can_;
    EventBroker &events_;
    StreamBroker &stream_;
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
    OximetryConsoleCommands(OximetryHub &hub,
                            UdpOximeterSource &udp,
                            BleSensorSource &sensor,
                            PlxPeripheral &peripheral,
                            ConfigService &config);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;
    void print_memory_detail(Print &out) override;

private:
    OximetryHub &hub_;
    UdpOximeterSource &udp_;
    BleSensorSource &sensor_;
    PlxPeripheral &peripheral_;
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
                       ResmedOtaManager &resmed_ota,
                       ResmedFirmwareRepository &resmed_repository);

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
    ResmedFirmwareRepository &resmed_repository_;
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
