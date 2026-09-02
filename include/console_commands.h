#pragma once

#include "board_net.h"
#include "console_command_router.h"
#include "large_scratch_array.h"
#include "storage_browser_port.h"
#include "storage_delete_port.h"
#include "storage_path_port.h"
#include "storage_read_port.h"

namespace aircannect {

static constexpr size_t AC_CONSOLE_COMMAND_SESSION_CAPACITY =
    AC_MAX_TELNET_CLIENTS + 2;

class As11DeviceService;
class As11BleRpcLink;
class As11SettingsManager;
class CanDriver;
class ConfigService;
class EdfRecorderManager;
class EventBroker;
class ExportCoordinator;
class ArduinoOtaSource;
class FirmwareInstaller;
class FirmwareUrlSource;
class LiveChartService;
class BleSensorSource;
class OximetryHub;
class PlxPeripheral;
class ReportTask;
class ResmedFirmwareRepository;
class ResmedFirmwarePreparer;
class ResmedOtaManager;
class RpcDiagnosticsPort;
class CanControlPort;
class RpcLinkSelector;
class RpcPassthroughPort;
class RpcRequestPort;
class SessionManager;
class StorageStatusPort;
class StreamBroker;
class TherapyTelemetryBroker;
class TimeSyncService;
class UdpOximeterSource;
class UpdateChecker;
class WebUI;
class WifiManager;

class As11DeviceConsoleCommands final : public ConsoleCommandGroup {
public:
    using BleConnectionCommand = bool (*)(void *context, uint32_t now_ms);

    As11DeviceConsoleCommands(RpcRequestPort &rpc,
                              RpcPassthroughPort &passthrough,
                              As11DeviceService &device,
                              As11SettingsManager &settings,
                              TimeSyncService &time_sync,
                              As11BleRpcLink &ble_link,
                              RpcLinkSelector &link_selector,
                              BleConnectionCommand connect_ble,
                              BleConnectionCommand disconnect_ble,
                              void *ble_connection_context = nullptr);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void print_summary(Print &out) override;
    void print_status(Print &out) override;

private:
    bool execute_rpc(const String &command,
                     String rest,
                     Print &out);

    RpcRequestPort &rpc_;
    RpcPassthroughPort &passthrough_;
    As11DeviceService &device_;
    As11SettingsManager &settings_;
    TimeSyncService &time_sync_;
    As11BleRpcLink &ble_link_;
    RpcLinkSelector &link_selector_;
    BleConnectionCommand connect_ble_ = nullptr;
    BleConnectionCommand disconnect_ble_ = nullptr;
    void *ble_connection_context_ = nullptr;
};

class CanConsoleCommands final : public ConsoleCommandGroup {
public:
    CanConsoleCommands(RpcDiagnosticsPort &diagnostics,
                       CanControlPort &can_control,
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
    void print_memory_detail(Print &out) override;

private:
    RpcDiagnosticsPort &diagnostics_;
    CanControlPort &can_control_;
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
    void print_memory_detail(Print &out) override;
};

class SystemConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit SystemConsoleCommands(FirmwareInstaller &installer);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    FirmwareInstaller &installer_;
};

class RuntimeConsoleCommands final : public ConsoleCommandGroup {
public:
    RuntimeConsoleCommands(SessionManager &session,
                           LiveChartService &live,
                           TherapyTelemetryBroker &telemetry);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &console_session) override;
    void print_summary(Print &out) override;
    void print_status(Print &out) override;
    void print_stats(Print &out) override;
    void reset_stats() override;

private:
    SessionManager &session_;
    LiveChartService &live_;
    TherapyTelemetryBroker &telemetry_;
};

class EdfConsoleCommands final : public ConsoleCommandGroup {
public:
    explicit EdfConsoleCommands(EdfRecorderManager &recorder);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void poll_pending(Print &out, ConsoleCommandSession &session) override;
    bool pending_output(
        const ConsoleCommandSession &session) const override;
    void cancel_pending(ConsoleCommandSession &session) override;
    void stop(ConsoleCommandSession &session) override;
    void print_summary(Print &out) override;
    void print_status(Print &out) override;
    bool print_scoped_stats(const String &scope, Print &out) override;

private:
    EdfRecorderManager &recorder_;
    uint32_t refresh_generation_ = 0;
    uint32_t refresh_session_id_ = 0;
    uint32_t refresh_wait_generation_ = 0;
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
    void print_summary(Print &out) override;
    void print_status(Print &out) override;
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
    bool print_scoped_stats(const String &scope, Print &out) override;

private:
    ReportTask &report_;
    uint32_t request_generation_ = 0;
};

class StorageConsoleCommands final : public ConsoleCommandGroup {
public:
    StorageConsoleCommands(ConfigService &config,
                           StorageReadPort &storage_read,
                           StorageBrowserPort &storage_browser,
                           StoragePathPort &storage_path,
                           StorageDeletePort &storage_delete,
                           StorageStatusPort &storage_status);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;
    void poll_pending(Print &out, ConsoleCommandSession &session) override;
    bool pending_output(
        const ConsoleCommandSession &session) const override;
    void cancel_pending(ConsoleCommandSession &session) override;
    void stop(ConsoleCommandSession &session) override;

private:
    enum class StorageOperation : uint8_t {
        None,
        List,
        ChangeDirectory,
        Rename,
        Delete,
    };

    struct CommandSessionState {
        uint32_t session_id = 0;
        char cwd[AC_STORAGE_PATH_MAX] = {};

        OperationTicket tail_ticket;
        StoragePreparedRead tail_prepared;
        size_t tail_offset = 0;
        uint32_t tail_generation = 0;

        StorageOperation storage_operation = StorageOperation::None;
        OperationTicket storage_ticket;
        uint32_t storage_generation = 0;
        uint32_t delete_id = 0;
        std::shared_ptr<const StorageDirectorySnapshot> listing_snapshot;
        size_t listing_offset = 0;
        char operation_path[AC_STORAGE_PATH_MAX] = {};
        char operation_destination[AC_STORAGE_PATH_MAX] = {};

        bool pending() const {
            return tail_ticket.valid() || tail_prepared.valid() ||
                   storage_operation != StorageOperation::None;
        }
    };

    CommandSessionState *command_session(uint32_t session_id, bool create);
    const CommandSessionState *command_session(uint32_t session_id) const;
    bool ensure_command_sessions();

    void execute_storage(String rest,
                         Print &out,
                         CommandSessionState &state);
    void poll_storage(Print &out, CommandSessionState &state);
    void cancel_storage(CommandSessionState &state);

    ConfigService &config_;
    StorageReadPort &storage_read_;
    StorageBrowserPort &storage_browser_;
    StoragePathPort &storage_path_;
    StorageDeletePort &storage_delete_;
    StorageStatusPort &storage_status_;
    LargeScratchArray<CommandSessionState> command_sessions_;
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
    explicit NetworkConsoleCommands(WifiManager &wifi);

    bool execute(const String &command,
                 const String &rest,
                 Print &out,
                 ConsoleCommandSession &session) override;

private:
    WifiManager &wifi_;
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
                       ResmedFirmwarePreparer &resmed_preparer,
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
    ResmedFirmwarePreparer &resmed_preparer_;
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
