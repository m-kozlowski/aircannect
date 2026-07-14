#pragma once

#include <Arduino.h>

#include "memory_manager.h"
#include "rpc_arbiter.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "tcp_bridge.h"
#include "wifi_manager.h"

namespace aircannect {
namespace ConsoleFormat {

void print_can_status(Print &out, const CanDriver &can_driver);
void print_can_stats(Print &out, const CanDriver &can_driver);
void print_rpc_status(Print &out, const RpcArbiter &arbiter);
void print_rpc_stats(Print &out, const RpcArbiter &arbiter);
void print_as11_status(Print &out, const As11DeviceState &state);
void print_stream_status(Print &out, const RpcArbiter &arbiter);
void print_log_status(Print &out);
void print_log_stats(Print &out);
void print_memory_status(Print &out, const MemoryStatus &status);
void print_memory_detail_status(Print &out,
                                const MemoryDetailStatus &status);
void print_storage_status(Print &out, const StorageStatus &status);
void print_session_status(Print &out, const SessionStatus &status);
void print_sink_status(Print &out, const SinkManager &sink_manager);
void print_tcp_status(Print &out, TcpBridge &tcp_bridge);
void print_tcp_stats(Print &out, TcpBridge &tcp_bridge);
void print_wifi_status(Print &out, const WifiManager &wifi_manager);

}  // namespace ConsoleFormat
}  // namespace aircannect
