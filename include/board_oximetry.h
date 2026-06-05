#pragma once

// Oximetry: defaults plus UDP/BLE source and BLE-sensor runtime.

#include <stddef.h>
#include <stdint.h>

#ifndef AC_DEFAULT_OXIMETRY_ENABLED
#define AC_DEFAULT_OXIMETRY_ENABLED 1
#endif

#ifndef AC_DEFAULT_OXIMETRY_ADVERTISE_MODE
// OximetryAdvertiseMode: 0=auto, 1=manual.
#define AC_DEFAULT_OXIMETRY_ADVERTISE_MODE 0
#endif

#ifndef AC_OXIMETRY_UDP_PORT
#define AC_OXIMETRY_UDP_PORT 8025
#endif

#ifndef AC_OXIMETRY_BLE_ENABLED
#define AC_OXIMETRY_BLE_ENABLED 1
#endif

static constexpr uint32_t AC_OXIMETRY_SAMPLE_STALE_MS = 1200;
static constexpr uint32_t AC_OXIMETRY_SOURCE_TIMEOUT_MS = 30000;
static constexpr uint32_t AC_OXIMETRY_NOTIFY_INTERVAL_MS = 1000;
static constexpr uint32_t AC_OXIMETRY_PAIRING_WINDOW_MS = 120000;
static constexpr size_t AC_OXIMETRY_BLE_NAME_MAX = 22;
static constexpr uint16_t AC_OXIMETRY_BLE_SCAN_DUP_CACHE = 16;
static constexpr size_t AC_OXIMETRY_UDP_PACKET_SIZE = 7;
static constexpr size_t AC_OXIMETRY_UDP_READ_BUDGET = 8;
static constexpr size_t AC_OXIMETRY_SENSOR_MAX_KNOWN = 4;
static constexpr size_t AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS = 8;
static constexpr size_t AC_OXIMETRY_SENSOR_NAME_MAX = 24;
static constexpr uint32_t AC_OXIMETRY_SENSOR_TASK_STACK = 6144;
static constexpr uint8_t AC_OXIMETRY_SENSOR_TASK_PRIO = 3;
static constexpr uint32_t AC_OXIMETRY_SENSOR_SCAN_MS = 5000;
static constexpr uint32_t AC_OXIMETRY_SENSOR_SCAN_IDLE_MS = 10000;
static constexpr uint32_t AC_OXIMETRY_SENSOR_NOTIFY_TIMEOUT_MS = 10000;
static constexpr uint32_t AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS = 30000;
static constexpr uint32_t AC_OXIMETRY_SENSOR_RECONNECT_HOLDOFF_MS = 180000;
static constexpr uint16_t AC_OXIMETRY_SENSOR_CONN_INTERVAL_MIN = 12;
static constexpr uint16_t AC_OXIMETRY_SENSOR_CONN_INTERVAL_MAX = 12;
static constexpr uint16_t AC_OXIMETRY_SENSOR_CONN_LATENCY = 0;
static constexpr uint16_t AC_OXIMETRY_SENSOR_CONN_TIMEOUT = 400;

