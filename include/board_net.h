#pragma once

// Network: ports, identity/auth defaults, TCP/telnet/web/log sizes, WiFi.

#include <stddef.h>
#include <stdint.h>

#ifndef AC_TCP_BRIDGE_PORT
#define AC_TCP_BRIDGE_PORT 39011
#endif

#ifndef AC_TELNET_CONSOLE_PORT
#define AC_TELNET_CONSOLE_PORT 23
#endif

#ifndef AC_SERIAL_BAUD
#define AC_SERIAL_BAUD 921600
#endif

#ifndef AC_ARDUINO_OTA_PORT
#define AC_ARDUINO_OTA_PORT 3232
#endif

#ifndef AC_HOSTNAME
#define AC_HOSTNAME "aircannect"
#endif

#ifndef AC_DEV_SOFTAP_SSID
#define AC_DEV_SOFTAP_SSID "AirCANnect"
#endif

#ifndef AC_DEV_SOFTAP_PASS
#define AC_DEV_SOFTAP_PASS "aircannect"
#endif

#ifndef AC_ENABLE_SOFTAP_FALLBACK
#define AC_ENABLE_SOFTAP_FALLBACK 1
#endif

#ifndef AC_DEFAULT_TCP_BRIDGE_ENABLED
#define AC_DEFAULT_TCP_BRIDGE_ENABLED 1
#endif

#ifndef AC_DEFAULT_TELNET_CONSOLE_ENABLED
#define AC_DEFAULT_TELNET_CONSOLE_ENABLED 1
#endif

#ifndef AC_DEFAULT_OTA_PASSWORD
#define AC_DEFAULT_OTA_PASSWORD "aircannect"
#endif

#ifndef AC_DEFAULT_HTTP_USER
#define AC_DEFAULT_HTTP_USER "admin"
#endif

#ifndef AC_DEFAULT_HTTP_PASSWORD
#define AC_DEFAULT_HTTP_PASSWORD "aircannect"
#endif

#ifndef AC_DEFAULT_WIFI_COUNTRY
#define AC_DEFAULT_WIFI_COUNTRY "01"
#endif

#ifndef AC_DEFAULT_TIMEZONE
#define AC_DEFAULT_TIMEZONE "UTC"
#endif

#ifndef AC_DEFAULT_SYSLOG_ENABLED
#define AC_DEFAULT_SYSLOG_ENABLED 0
#endif

static constexpr size_t AC_MAX_TCP_CLIENTS = 4;
static constexpr size_t AC_MAX_TELNET_CLIENTS = 2;
static constexpr size_t AC_WIFI_PROFILE_MAX = 4;
static constexpr size_t AC_TCP_LINE_MAX = 2048;
static constexpr size_t AC_TCP_TX_QUEUE_DEPTH = 24;
static constexpr size_t AC_TCP_WRITE_CHUNK = 512;
static constexpr size_t AC_TCP_READ_BYTES_PER_POLL = 512;
static constexpr size_t AC_TELNET_TX_QUEUE_DEPTH = 16;
static constexpr size_t AC_TELNET_READ_BYTES_PER_POLL = 256;
static constexpr size_t AC_TELNET_AUTH_LINE_MAX = 96;
static constexpr size_t AC_WEB_MAX_POST_BODY = 4096;
static constexpr size_t AC_WEB_COMMAND_QUEUE_DEPTH = 12;
static constexpr size_t AC_WEB_COMMANDS_PER_POLL = 4;
static constexpr uint32_t AC_WEB_SSE_PUSH_INTERVAL_MS = 3000;
static constexpr uint32_t AC_WEB_LIVE_PUSH_INTERVAL_MS = 250;
static constexpr size_t AC_WEB_SSE_CLIENTS_MAX = 3;
static constexpr size_t AC_WEB_SSE_CLIENT_PENDING_MAX = 3;
static constexpr size_t AC_WEB_SSE_CLIENT_PENDING_HARD_MAX = 16;
static constexpr uint32_t AC_WEB_SSE_BACKPRESSURE_STATUS_MS = 15000;
static constexpr size_t AC_WEB_LIVE_FRAME_BUDGET = 4;
static constexpr size_t AC_WEB_LIVE_BATCH_SAMPLES_MAX = 96;
static constexpr size_t AC_WEB_CONSOLE_LOG_MAX = 4096;

static constexpr size_t AC_WEB_STATUS_JSON_RESERVE = 5120;
static constexpr size_t AC_WEB_STREAM_JSON_RESERVE = 1536;
static constexpr size_t AC_WEB_WIFI_JSON_RESERVE = 1024;
static constexpr size_t AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE = 2048;
static constexpr size_t AC_WEB_OTA_JSON_RESERVE = 768;
static constexpr size_t AC_WEB_RESMED_OTA_JSON_RESERVE = 1024;
static constexpr size_t AC_WEB_SETTINGS_JSON_RESERVE = 4096;
static constexpr size_t AC_WEB_SETTINGS_CATALOG_JSON_RESERVE = 8192;

static constexpr size_t AC_LOG_LINE_MAX = 192;
static constexpr size_t AC_SYSLOG_QUEUE_DEPTH = 32;
static constexpr size_t AC_SYSLOG_SEND_BUDGET = 4;
static constexpr uint16_t AC_SYSLOG_PORT = 514;

static constexpr uint32_t AC_WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_WIFI_PMF_RETRY_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_WIFI_SOFTAP_RETRY_MS = 60000;
static constexpr size_t AC_WIFI_SCAN_CANDIDATES_MAX = 16;
static constexpr uint32_t AC_WIFI_ROAM_CHECK_INTERVAL_MS = 60000;
static constexpr int32_t AC_WIFI_ROAM_RSSI_THRESHOLD_DBM = -73;
static constexpr uint8_t AC_WIFI_ROAM_CONSECUTIVE_LOW = 3;
static constexpr int32_t AC_WIFI_ROAM_HYSTERESIS_DB = 8;
static constexpr uint32_t AC_WIFI_ROAM_STREAM_QUIET_MS = 30000;
static constexpr size_t AC_WIFI_BSSID_TEXT_MAX = 18;
static constexpr uint32_t AC_WIFI_MANUAL_SCAN_RESULT_TTL_MS = 30000;
