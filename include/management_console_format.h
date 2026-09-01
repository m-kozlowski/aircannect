#pragma once

#include <Arduino.h>

#include "can_driver.h"
#include "memory_manager.h"
#include "rpc_transport_ports.h"
#include "session_manager.h"
#include "live_chart_service.h"
#include "storage_manager.h"
#include "tcp_bridge.h"
#include "therapy_telemetry_broker.h"
#include "wifi_manager.h"

namespace aircannect {

class EventBroker;
class TimeSyncService;

namespace ConsoleFormat {

void print_can_status(Print &out, const CanDriver &can_driver);
void print_rpc_status(Print &out,
                      const RpcDiagnosticsPort &rpc,
                      const CanDriver &can_driver);
void print_rpc_stats(Print &out,
                     const RpcDiagnosticsPort &rpc,
                     const CanDriver &can_driver,
                     const EventBroker &events,
                     const StreamBroker &stream);
void print_as11_summary(Print &out, const As11DeviceState &state);
void print_as11_status(Print &out, const As11DeviceState &state);
void print_time_summary(Print &out,
                        const As11DeviceState &state,
                        const TimeSyncService &time_sync);
void print_time_status(Print &out,
                       const As11DeviceState &state,
                       const TimeSyncService &time_sync);
void print_stream_status(Print &out, const StreamBroker &stream);
void print_log_status(Print &out);
void print_memory_status(Print &out, const MemoryStatus &status);
void print_memory_detail_status(Print &out,
                                const MemoryDetailStatus &status);
void print_storage_status(Print &out, const StorageStatus &status);
void print_session_summary(Print &out, const SessionStatus &status);
void print_session_status(Print &out, const SessionStatus &status);
void print_therapy_telemetry_status(
    Print &out,
    const TherapyTelemetryRuntimeStatus &status);
void print_live_summary(Print &out, const LiveChartService &live_service);
void print_live_status(Print &out, const LiveChartService &live_service);
void print_tcp_status(Print &out, TcpBridge &tcp_bridge);
void print_wifi_status(Print &out, const WifiManager &wifi_manager);

}  // namespace ConsoleFormat
}  // namespace aircannect
