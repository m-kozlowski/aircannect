#pragma once

// Report and export task config, prefetch policy, and result/plot tuning.

#include <stddef.h>
#include <stdint.h>

// New report pipeline owner. It shares core 0 with storage at the same low
// priority and yields after every bounded decode step, so storage's EDF lane
// and the main loop remain responsive.
static constexpr uint32_t AC_REPORT_TASK_STACK = 12288;
static constexpr uint8_t AC_REPORT_TASK_PRIO = 1;
static constexpr uint8_t AC_REPORT_TASK_CORE = 0;
static constexpr uint32_t AC_REPORT_TASK_WORK_TICK_MS = 1;
static constexpr uint32_t AC_REPORT_TASK_WAIT_TICK_MS = 5;
static constexpr uint32_t AC_REPORT_TASK_IDLE_TICK_MS = 1000;
static constexpr size_t AC_REPORT_TASK_COMMAND_CAPACITY = 8;
static constexpr size_t AC_REPORT_TASK_BUILD_CAPACITY = 8;
static constexpr size_t AC_REPORT_FILE_LOAD_COPY_BYTES = 16 * 1024;
static constexpr size_t AC_REPORT_FOREGROUND_RECORD_BUDGET = 32;

// SMB and SleepHQ share one background task so their network and storage
// phases cannot overlap. It runs at idle priority: export code crosses TLS,
// JSON, and filesystem libraries whose internal loops are not under our
// scheduler control, so no export work may prevent IDLE0 from running.
static constexpr uint32_t AC_EXPORT_TASK_STACK = 8192;
static constexpr uint8_t AC_EXPORT_TASK_PRIO = 0;
static constexpr uint8_t AC_EXPORT_TASK_CORE = 0;
static constexpr uint32_t AC_EXPORT_TASK_BUSY_RECHECK_MS = 250;
static constexpr uint32_t AC_EXPORT_TASK_WORK_TICK_MS = 20;
static constexpr uint32_t AC_EXPORT_TASK_IDLE_TICK_MS = 10000;
static constexpr uint32_t AC_EXPORT_ACTIVITY_GRACE_MS = 3000;
static constexpr uint32_t AC_EXPORT_FULL_RECONCILE_IDLE_GRACE_MS =
    5UL * 60UL * 1000UL;
static constexpr uint64_t AC_EXPORT_FULL_RECONCILE_INTERVAL_SECONDS =
    24ULL * 60ULL * 60ULL;

// Automatic report prebuild and export startup checks stay out of the first
// boot window while CAN/RPC discovery, Wi-Fi, and time synchronization settle.
// Durable report bootstrap and explicit user requests are not delayed.
static constexpr uint32_t AC_RUNTIME_STARTUP_IDLE_GRACE_MS = 30000;

// After therapy stops, AS11 may still be draining live stream/report traffic and
// AirCANnect is closing EDF/SMB work. Let the bus settle before refreshing the
// report summary index that triggers cache backfill.
static constexpr uint32_t AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS = 30000;
// Post-therapy exports wait for report state to settle before reading the
// newly finalized files. A stalled report refresh must not suppress either
// endpoint indefinitely.
static constexpr uint32_t AC_EXPORT_POST_THERAPY_SETTLE_MAX_WAIT_MS = 180000;
// Report spool backfill is intentionally guarded at three layers:
// fragment size keeps AS11 happy, max-notifications bounds each CAN burst, and
// pull pacing gives the arbiter time to observe RX pressure before next pull.
// AS11 rejects too-small PullSpoolFragments maxFragmentSize values. Keep the
// observed protocol default here; burst pacing belongs between spool rounds.
static constexpr size_t AC_REPORT_SPOOL_FRAGMENT_MAX_BYTES = 2808;
// Limit each PullSpoolFragments response burst. A single AS11 SpoolFragment is
// already large after base64/JSON wrapping; request boundaries give the arbiter
// a chance to observe CAN/RPC backpressure before asking for more.
static constexpr size_t AC_REPORT_SPOOL_MAX_NOTIFICATIONS_PER_PULL = 1;
// Spool notifications are copied into report-owned PSRAM before decoding so
// RPC/CAN dispatch remains bounded. The protocol currently requests one
// notification per pull; extra slots tolerate delayed report service.
static constexpr size_t AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH = 4;
static constexpr size_t AC_REPORT_SPOOL_NOTIFICATION_BACKPRESSURE_WATERMARK =
    AC_REPORT_SPOOL_NOTIFICATION_QUEUE_DEPTH / 2;
// Minimum spacing between PullSpoolFragments requests. The response is a CAN
// burst; this keeps report backfill from immediately requesting the next burst
// before the arbiter has observed queue pressure from the previous one.
static constexpr uint32_t AC_REPORT_SPOOL_PULL_PACE_MS = 100;
static constexpr size_t AC_REPORT_SUMMARY_SPOOL_ROUND_BYTES = 8192;
static constexpr size_t AC_REPORT_CACHE_SPOOL_ROUND_BYTES = 8192;
static constexpr size_t AC_REPORT_AVAILABILITY_PROBE_BYTES = 4096;
