#pragma once

// Background worker and report prefetch policy.

#include <stddef.h>
#include <stdint.h>

// Background storage worker: a low-priority FreeRTOS task that prefetches
// missing report data and builds plots while the device is idle. Pinned to
// core 0 so its CPU-heavy work runs parallel to the main loop on core 1; the
// SD-bus mutex serializes card access. Priority below AsyncTCP (3) and the
// loop, so the web UI and CAN servicing always win.
static constexpr uint32_t AC_BG_WORKER_TASK_STACK = 4096;
static constexpr uint8_t AC_BG_WORKER_TASK_PRIO = 1;
static constexpr uint8_t AC_BG_WORKER_TASK_CORE = 0;
// Re-check cadence when foreground is active (gate closed).
static constexpr uint32_t AC_BG_WORKER_BUSY_RECHECK_MS = 250;
// Short yield between bounded work units so the gate is re-checked often.
static constexpr uint32_t AC_BG_WORKER_WORK_TICK_MS = 20;
// Sleep when idle but no job has work to do.
static constexpr uint32_t AC_BG_WORKER_IDLE_TICK_MS = 2000;
// Defer prefetch briefly after foreground (e.g. web) activity.
static constexpr uint32_t AC_BG_WORKER_ACTIVITY_GRACE_MS = 3000;
// Prefetch policy: how long the worker waits before re-scanning once every
// night is cached, and how long a night whose fetch failed is skipped so
// newest-first backfill is not blocked by one bad night.
static constexpr uint32_t AC_REPORT_PREFETCH_RESCAN_MS = 60000;
static constexpr uint32_t AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS = 600000;
// Circuit breaker: after this many consecutive prefetch failures (e.g. the CPAP
// is offline so every spool times out) the worker backs off for a long while
// instead of churning the bus; a single success resets it.
static constexpr uint32_t AC_REPORT_PREFETCH_FAIL_BURST = 3;
static constexpr uint32_t AC_REPORT_PREFETCH_OFFLINE_BACKOFF_MS = 300000;

