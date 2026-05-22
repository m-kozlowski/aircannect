#pragma once

#include <Arduino.h>

#include "memory_manager.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
#include "tcp_bridge.h"

namespace aircannect {
namespace ConsoleFormat {

void print_memory_status(Print &out, const MemoryStatus &status);
void print_storage_status(Print &out, const StorageStatus &status);
void print_storage_writer_status(Print &out,
                                 const StorageWriterStatus &status);
void print_session_status(Print &out, const SessionStatus &status);
void print_sink_status(Print &out, const SinkManager &sink_manager);
void print_tcp_status(Print &out, TcpBridge &tcp_bridge);
void print_tcp_stats(Print &out, TcpBridge &tcp_bridge);

}  // namespace ConsoleFormat
}  // namespace aircannect
