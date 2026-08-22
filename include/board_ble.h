#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef AC_BLE_ENABLED
#define AC_BLE_ENABLED 1
#endif

static constexpr uint16_t AC_BLE_SCAN_DUP_CACHE = 16;
static constexpr size_t AC_BLE_DEVICE_NAME_MAX = 22;

static constexpr uint32_t AC_AS11_BLE_TASK_STACK = 8192;
static constexpr uint8_t AC_AS11_BLE_TASK_PRIO = 3;
static constexpr uint32_t AC_AS11_BLE_SCAN_MS = 5000;
static constexpr uint32_t AC_AS11_BLE_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t AC_AS11_BLE_SESSION_TIMEOUT_MS = 10000;
static constexpr uint32_t AC_AS11_BLE_RECONNECT_MIN_MS = 5000;
static constexpr uint32_t AC_AS11_BLE_RECONNECT_MAX_MS = 60000;
static constexpr uint32_t AC_AS11_BLE_PAIRING_PASSKEY_TIMEOUT_MS = 120000;
static constexpr size_t AC_AS11_BLE_ADDRESS_MAX = 17;
static constexpr size_t AC_AS11_BLE_CLIENT_ID_MAX = 64;
static constexpr size_t AC_AS11_BLE_MASTER_KEY_HEX_LENGTH = 64;
static constexpr size_t AC_AS11_BLE_PAIRING_DEVICE_MAX = 4;
static constexpr size_t AC_AS11_BLE_PAIRING_DEVICE_NAME_MAX = 31;
static constexpr size_t AC_AS11_BLE_PAIRING_COMMAND_QUEUE_DEPTH = 4;
static constexpr size_t AC_AS11_BLE_CREDENTIAL_QUEUE_DEPTH = 2;
static constexpr size_t AC_AS11_BLE_NOTIFICATION_QUEUE_DEPTH = 48;
static constexpr size_t AC_AS11_BLE_REQUEST_QUEUE_DEPTH = 4;
static constexpr size_t AC_AS11_BLE_EVENT_QUEUE_DEPTH = 16;
