#pragma once

// CAN/TWAI link and ResMed AS11 RPC / stream / OTA protocol constants.

#include <stddef.h>
#include <stdint.h>

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

#ifndef AC_DEFAULT_RESMED_TIME_SYNC_ENABLED
#define AC_DEFAULT_RESMED_TIME_SYNC_ENABLED 0
#endif

// ResMed OTA UpgradeDataBlock at 500 raw bytes expands to about 162 CAN
// datagram frames after ASCII-hex JSON wrapping.
static constexpr size_t AC_CAN_TX_QUEUE_DEPTH = 224;
static constexpr size_t AC_CAN_TX_DRAIN_BUDGET = 32;
// Report spool replies can arrive as one large base64/JSON notification burst.
// Keep enough TWAI RX slack that a short main-loop pass or callback cannot
// overflow the TWAI queue before the next drain checkpoint.
static constexpr size_t AC_CAN_RX_QUEUE_LEN = 512;
static constexpr size_t AC_CAN_RX_DRAIN_BASE_BUDGET = 128;
static constexpr size_t AC_CAN_RX_DRAIN_PRESSURE_BUDGET = 512;
static constexpr uint32_t AC_CAN_RX_DRAIN_PRESSURE_MAX_MS = 2;
static constexpr uint32_t AC_CAN_BUS_RECOVERY_TIMEOUT_MS = 1500;
static constexpr uint32_t AC_CAN_RECOVERY_RETRY_MS = 250;
static constexpr uint32_t AC_CAN_RECOVERY_LOG_INTERVAL_MS = 5000;
static constexpr uint8_t AC_CAN_CONTROLLER_RESTART_MAX_ATTEMPTS = 3;
static constexpr uint8_t AC_CAN_BUS_RECOVERY_MAX_ATTEMPTS = 3;
static constexpr size_t AC_CAN_RX_BACKPRESSURE_WATERMARK =
    AC_CAN_RX_QUEUE_LEN / 4;
static constexpr size_t AC_RPC_PAYLOAD_QUEUE_DEPTH = 8;
static constexpr size_t AC_RPC_PAYLOAD_DRAIN_BUDGET = 2;
static constexpr size_t AC_RPC_PAYLOAD_BACKPRESSURE_WATERMARK =
    AC_RPC_PAYLOAD_QUEUE_DEPTH / 2;
static constexpr size_t AC_DG_MAX_PAYLOAD_BYTES = 8192;
static constexpr size_t AC_DG_INITIAL_RESERVE_BYTES = 256;
static constexpr uint32_t AC_DG_IDLE_TIMEOUT_MS = 2000;
static constexpr size_t AC_RPC_EVENT_QUEUE_DEPTH = 48;
static constexpr size_t AC_REPORT_EVENT_QUEUE_DEPTH = 32;
static constexpr size_t AC_REPORT_EVENT_BACKPRESSURE_WATERMARK =
    AC_REPORT_EVENT_QUEUE_DEPTH / 2;
static constexpr size_t AC_RESMED_OTA_EVENT_QUEUE_DEPTH = 8;
static constexpr size_t AC_RPC_REQUEST_QUEUE_DEPTH = 8;
static constexpr size_t AC_STREAM_CONSUMERS_MAX = 4;
static constexpr size_t AC_STREAM_CONSUMER_QUEUE_DEPTH = 2;

static constexpr size_t AC_STREAM_FRAME_POOL_INTERNAL = 3;
// EDF keeps a short PSRAM-backed startup buffer while SD open jobs finish.
static constexpr size_t AC_STREAM_FRAME_POOL_PSRAM = 48;
static constexpr size_t AC_STREAM_FRAME_RAW_MAX = 6144;
static constexpr size_t AC_STREAM_FRAME_SIGNAL_MAX = 24;
static constexpr size_t AC_STREAM_FRAME_SIGNAL_NAME_MAX = 48;
static constexpr size_t AC_STREAM_FRAME_START_TIME_MAX = 40;
static constexpr size_t AC_STREAM_FRAME_VALUES_MAX = 256;

static constexpr uint32_t AC_RPC_DEFAULT_TIMEOUT_MS = 5000;
static constexpr uint32_t AC_RPC_STREAM_TIMEOUT_MS = 8000;
static constexpr uint32_t AC_RPC_RAW_PASSTHROUGH_TIMEOUT_MS = 120000;
static constexpr uint32_t AC_RESMED_OTA_BLOCK_TIMEOUT_MS = 15000;
static constexpr uint32_t AC_RESMED_OTA_VERIFY_TIMEOUT_MS = 120000;
static constexpr uint32_t AC_RESMED_OTA_IDLE_TIMEOUT_MS = 300000;
static constexpr size_t AC_RESMED_OTA_MAX_BLOCK_BYTES = 500;
static constexpr size_t AC_RESMED_OTA_MAX_FILE_BYTES = 4UL * 1024UL * 1024UL;
static constexpr const char *AC_RESMED_OTA_STAGED_PATH =
    "/aircannect/resmed-ota.abc";
static constexpr uint64_t AC_RESMED_OTA_STORAGE_MARGIN_BYTES =
    1024ULL * 1024ULL;
static constexpr const char *AC_RESMED_OTA_CONFIRM = "APPLY_RESMED_OTA";
static constexpr uint32_t AC_RPC_MIN_TX_INTERVAL_MS = 20;
static constexpr uint32_t AC_RPC_SET_DATETIME_APPLY_LEAD_MS = 10;
static constexpr uint32_t AC_RPC_SET_DATETIME_TARGET_MARGIN_MS = 40;
static constexpr uint8_t AC_RPC_BACKGROUND_TIMEOUTS_BEFORE_BACKOFF = 2;
static constexpr uint32_t AC_RPC_BACKGROUND_BACKOFF_MS = 120000;
static constexpr uint32_t AC_RPC_BACKGROUND_RX_PRESSURE_BACKOFF_MS = 500;
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
static constexpr uint32_t AC_AS11_EVENT_QUIESCE_RETRY_MS = 500;
static constexpr uint32_t AC_ESP_OTA_QUIESCE_TIMEOUT_MS = 15000;
