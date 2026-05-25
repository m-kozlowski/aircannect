#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef AIRCANNECT_VERSION
#define AIRCANNECT_VERSION "dev"
#endif

#ifndef AIRCANNECT_BUILD_DATE
#define AIRCANNECT_BUILD_DATE __DATE__ " " __TIME__
#endif

#ifndef AC_CAN_TX_GPIO
#define AC_CAN_TX_GPIO 26
#endif

#ifndef AC_CAN_RX_GPIO
#define AC_CAN_RX_GPIO 36
#endif

#ifndef AC_CAN_TX_ID
#define AC_CAN_TX_ID 0x383
#endif

#ifndef AC_CAN_RX_ID
#define AC_CAN_RX_ID 0x382
#endif

#ifndef AC_CAN_LOG_ID
#define AC_CAN_LOG_ID 0x796
#endif

#ifndef AC_CAN_BOOT_ID
#define AC_CAN_BOOT_ID 0x2c8
#endif

#ifndef AC_CAN_BITRATE
#define AC_CAN_BITRATE 1000000
#endif

#define AC_CAN_TIMING_STOCK 0
#define AC_CAN_TIMING_20TQ_SP80 1

// ESP32-S3 framework defaults may resolve to 8 TQ / 75% at 1 Mbit.
// XIAO profiles use the explicit 20 TQ / 80% timing proven against AS11.
#ifndef AC_CAN_TIMING
#define AC_CAN_TIMING AC_CAN_TIMING_STOCK
#endif

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

#ifndef AC_STORAGE_SDMMC_ENABLED
#define AC_STORAGE_SDMMC_ENABLED 0
#endif

#ifndef AC_STORAGE_SPI_SD_ENABLED
#define AC_STORAGE_SPI_SD_ENABLED 0
#endif

#ifndef AC_STORAGE_MOUNT_POINT
#define AC_STORAGE_MOUNT_POINT "/sdcard"
#endif

#ifndef AC_STORAGE_MAX_OPEN_FILES
#define AC_STORAGE_MAX_OPEN_FILES 4
#endif

#ifndef AC_PROVISION_CONFIG_PATH
#define AC_PROVISION_CONFIG_PATH "/config.txt"
#endif

#ifndef AC_PROVISION_OK_PATH
#define AC_PROVISION_OK_PATH "/config.ok"
#endif

static constexpr size_t AC_PROVISION_LINE_MAX = 256;

#ifndef AC_SDMMC_WIDTH
#define AC_SDMMC_WIDTH 4
#endif

#ifndef AC_SDMMC_FREQ_KHZ
#define AC_SDMMC_FREQ_KHZ 20000
#endif

#ifndef AC_SDMMC_CLK_GPIO
#define AC_SDMMC_CLK_GPIO -1
#endif

#ifndef AC_SDMMC_CMD_GPIO
#define AC_SDMMC_CMD_GPIO -1
#endif

#ifndef AC_SDMMC_D0_GPIO
#define AC_SDMMC_D0_GPIO -1
#endif

#ifndef AC_SDMMC_D1_GPIO
#define AC_SDMMC_D1_GPIO -1
#endif

#ifndef AC_SDMMC_D2_GPIO
#define AC_SDMMC_D2_GPIO -1
#endif

#ifndef AC_SDMMC_D3_GPIO
#define AC_SDMMC_D3_GPIO -1
#endif

#ifndef AC_SPI_SD_CS_GPIO
#define AC_SPI_SD_CS_GPIO -1
#endif

#ifndef AC_SPI_SD_SCK_GPIO
#define AC_SPI_SD_SCK_GPIO -1
#endif

#ifndef AC_SPI_SD_MISO_GPIO
#define AC_SPI_SD_MISO_GPIO -1
#endif

#ifndef AC_SPI_SD_MOSI_GPIO
#define AC_SPI_SD_MOSI_GPIO -1
#endif

#ifndef AC_SPI_SD_FREQ_HZ
#define AC_SPI_SD_FREQ_HZ 10000000
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

#ifndef AC_DEFAULT_RESMED_TIME_SYNC_ENABLED
#define AC_DEFAULT_RESMED_TIME_SYNC_ENABLED 0
#endif

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
static constexpr size_t AC_WEB_LIVE_FRAME_BUDGET = 4;
static constexpr size_t AC_WEB_LIVE_BATCH_SAMPLES_MAX = 96;
static constexpr size_t AC_WEB_CONSOLE_LOG_MAX = 8192;

static constexpr size_t AC_WEB_STATUS_JSON_RESERVE = 4096;
static constexpr size_t AC_WEB_STREAM_JSON_RESERVE = 1536;
static constexpr size_t AC_WEB_CONFIG_JSON_RESERVE = 1024;
static constexpr size_t AC_WEB_WIFI_JSON_RESERVE = 1024;
static constexpr size_t AC_WEB_OTA_JSON_RESERVE = 768;
static constexpr size_t AC_WEB_RESMED_OTA_JSON_RESERVE = 1024;
static constexpr size_t AC_WEB_SETTINGS_JSON_RESERVE = 4096;
static constexpr size_t AC_WEB_SETTINGS_CATALOG_JSON_RESERVE = 8192;

#ifndef AC_MEMORY_HEAP_TRACE_ENABLED
#define AC_MEMORY_HEAP_TRACE_ENABLED 0
#endif

#ifndef AC_MEMORY_HEAP_TRACE_RECORDS
#define AC_MEMORY_HEAP_TRACE_RECORDS 128
#endif

static constexpr size_t AC_LOG_LINE_MAX = 192;
static constexpr size_t AC_SYSLOG_QUEUE_DEPTH = 32;
static constexpr size_t AC_SYSLOG_SEND_BUDGET = 4;
static constexpr uint16_t AC_SYSLOG_PORT = 514;

static constexpr uint32_t AC_WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_WIFI_PMF_RETRY_TIMEOUT_MS = 15000;
static constexpr size_t AC_WIFI_SCAN_CANDIDATES_MAX = 16;
static constexpr uint32_t AC_WIFI_ROAM_CHECK_INTERVAL_MS = 60000;
static constexpr int32_t AC_WIFI_ROAM_RSSI_THRESHOLD_DBM = -73;
static constexpr uint8_t AC_WIFI_ROAM_CONSECUTIVE_LOW = 3;
static constexpr int32_t AC_WIFI_ROAM_HYSTERESIS_DB = 8;
static constexpr uint32_t AC_WIFI_ROAM_STREAM_QUIET_MS = 30000;
static constexpr size_t AC_WIFI_BSSID_TEXT_MAX = 18;
static constexpr uint32_t AC_WIFI_MANUAL_SCAN_RESULT_TTL_MS = 30000;

static constexpr uint32_t AC_OXIMETRY_SAMPLE_STALE_MS = 1200;
static constexpr uint32_t AC_OXIMETRY_SOURCE_TIMEOUT_MS = 30000;
static constexpr uint32_t AC_OXIMETRY_NOTIFY_INTERVAL_MS = 1000;
static constexpr uint32_t AC_OXIMETRY_PAIRING_WINDOW_MS = 120000;
static constexpr size_t AC_OXIMETRY_BLE_NAME_MAX = 22;
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

static constexpr uint32_t AC_STORAGE_STATUS_POLL_MS = 30000;
static constexpr size_t AC_STORAGE_WRITE_QUEUE_INTERNAL = 4;
static constexpr size_t AC_STORAGE_WRITE_QUEUE_PSRAM = 16;
static constexpr size_t AC_STORAGE_WRITE_CHUNK_BYTES = 512;
static constexpr size_t AC_STORAGE_WRITE_PATH_MAX = 80;
static constexpr size_t AC_STORAGE_WRITE_BUDGET_ITEMS = 2;
static constexpr size_t AC_STORAGE_WRITE_BUDGET_BYTES = 1024;
static constexpr size_t AC_SINK_STREAM_FRAME_BUDGET = 4;
static constexpr uint32_t AC_SINK_ATTACH_RETRY_MS = 2000;

// ResMed OTA UpgradeDataBlock at 500 raw bytes expands to about 162 CAN
// datagram frames after ASCII-hex JSON wrapping.
static constexpr size_t AC_CAN_TX_QUEUE_DEPTH = 224;
static constexpr size_t AC_CAN_TX_DRAIN_BUDGET = 32;
static constexpr size_t AC_CAN_RX_QUEUE_LEN = 128;
static constexpr size_t AC_CAN_RX_DRAIN_BUDGET = 128;
static constexpr size_t AC_DG_MAX_PAYLOAD_BYTES = 8192;
static constexpr size_t AC_DG_INITIAL_RESERVE_BYTES = 1024;
static constexpr uint32_t AC_DG_IDLE_TIMEOUT_MS = 2000;
static constexpr size_t AC_RPC_EVENT_QUEUE_DEPTH = 48;
static constexpr size_t AC_RESMED_OTA_EVENT_QUEUE_DEPTH = 8;
static constexpr size_t AC_RPC_REQUEST_QUEUE_DEPTH = 8;
static constexpr size_t AC_STREAM_CONSUMERS_MAX = 4;
static constexpr size_t AC_STREAM_CONSUMER_QUEUE_DEPTH = 2;

static constexpr size_t AC_STREAM_FRAME_POOL_INTERNAL = 3;
static constexpr size_t AC_STREAM_FRAME_POOL_PSRAM = 8;
static constexpr size_t AC_STREAM_FRAME_RAW_MAX = 6144;
static constexpr size_t AC_STREAM_FRAME_SIGNAL_MAX = 24;
static constexpr size_t AC_STREAM_FRAME_SIGNAL_NAME_MAX = 48;
static constexpr size_t AC_STREAM_FRAME_START_TIME_MAX = 40;
static constexpr size_t AC_STREAM_FRAME_VALUES_MAX = 256;

static constexpr uint32_t AC_RPC_DEFAULT_TIMEOUT_MS = 5000;
static constexpr uint32_t AC_RPC_STREAM_TIMEOUT_MS = 8000;
static constexpr uint32_t AC_RESMED_OTA_BLOCK_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_RESMED_OTA_VERIFY_TIMEOUT_MS = 120000;
static constexpr uint32_t AC_RESMED_OTA_IDLE_TIMEOUT_MS = 300000;
static constexpr size_t AC_RESMED_OTA_MAX_BLOCK_BYTES = 500;
static constexpr size_t AC_RESMED_OTA_MAX_FILE_BYTES = 4UL * 1024UL * 1024UL;
static constexpr const char *AC_RESMED_OTA_STORAGE_DIR = "/aircannect";
static constexpr const char *AC_RESMED_OTA_STAGED_PATH =
    "/aircannect/resmed-ota.abc";
static constexpr const char *AC_RESMED_OTA_STAGED_TMP_PATH =
    "/aircannect/resmed-ota.abc.tmp";
static constexpr uint64_t AC_RESMED_OTA_STORAGE_MARGIN_BYTES =
    1024ULL * 1024ULL;
static constexpr const char *AC_RESMED_OTA_CONFIRM = "APPLY_RESMED_OTA";
static constexpr uint32_t AC_RPC_MIN_TX_INTERVAL_MS = 20;
static constexpr uint8_t AC_RPC_BACKGROUND_TIMEOUTS_BEFORE_BACKOFF = 2;
static constexpr uint32_t AC_RPC_BACKGROUND_BACKOFF_MS = 120000;
static constexpr uint32_t AC_STREAM_RESYNC_INTERVAL_MS = 2000;

static constexpr uint32_t AC_AS11_INITIAL_STATUS_POLL_DELAY_MS = 3000;
static constexpr uint32_t AC_AS11_STATUS_POLL_INTERVAL_MS = 30000;
static constexpr uint32_t AC_AS11_THERAPY_STATUS_POLL_DELAY_MS = 1000;
static constexpr uint32_t AC_AS11_THERAPY_STATUS_POLL_INTERVAL_MS = 3000;
static constexpr uint32_t AC_AS11_MOTOR_RUNTIME_POLL_INTERVAL_MS = 900000;
static constexpr uint32_t AC_AS11_TIMEZONE_POLL_INTERVAL_MS = 300000;
static constexpr uint32_t AC_AS11_CLOCK_POLL_INTERVAL_MS = 60000;
static constexpr uint32_t AC_AS11_THERAPY_CONFIRM_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_AS11_EVENT_SUBSCRIBE_DELAY_MS = 1000;
static constexpr uint32_t AC_AS11_EVENT_SUBSCRIBE_RETRY_MS = 30000;
