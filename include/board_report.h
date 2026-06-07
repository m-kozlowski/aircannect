#pragma once

// Report subsystem config: background worker, prefetch policy, and
// result-assembly / plot tuning.

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
static constexpr uint32_t AC_BG_WORKER_IDLE_TICK_MS = 10000;
// Defer prefetch briefly after foreground (e.g. web) activity.
static constexpr uint32_t AC_BG_WORKER_ACTIVITY_GRACE_MS = 3000;
// Fallback rescan: the prefetch job rescans coverage promptly when the night
// index changes (e.g. after therapy stops), so this periodic poll is just a
// safety net. FAIL_COOLDOWN is how long a night whose fetch failed is skipped
// so newest-first backfill is not blocked by one bad night.
static constexpr uint32_t AC_REPORT_PREFETCH_RESCAN_MS = 300000;
static constexpr uint32_t AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS = 600000;
// Circuit breaker: after this many consecutive prefetch failures (e.g. the CPAP
// is offline so every spool times out) the worker backs off for a long while
// instead of churning the bus; a single success resets it.
static constexpr uint32_t AC_REPORT_PREFETCH_FAIL_BURST = 3;
static constexpr uint32_t AC_REPORT_PREFETCH_OFFLINE_BACKOFF_MS = 600000;

// Report result assembly and plot build.
// Plot decimation: target buckets across a night's session span.
static constexpr size_t AC_REPORT_PLOT_BUCKETS = 1200;
// Initial reserve for the incrementally-built plot binary.
static constexpr size_t AC_REPORT_PLOT_INITIAL_RESERVE = 256 * 1024;
// Range (zoom) plot: a boundary-crossing drag fetches up to 2h at 6.25Hz
static constexpr size_t AC_REPORT_RANGE_PLOT_MAX_BYTES = 1536 * 1024;
static constexpr size_t AC_REPORT_RANGE_MAX_POINTS = 48000;  // per stream (~2.1h @ 6.25Hz)
// Gap between consecutive series chunks that marks a session boundary. The
// coalescing writer flushes here and derive_result_session_ranges() splits
// here; they must agree, or a coalesced chunk would span (and hide) a gap.
static constexpr int64_t AC_REPORT_SESSION_GAP_MS = 3LL * 60LL * 1000LL;
// Latest-night tail refresh re-fetches this far back to overlap cached data so
// the seam has no gap.
static constexpr int64_t AC_REPORT_LATEST_TAIL_OVERLAP_MS = 60000;
// Coverage tolerance: spool boundaries rarely line up to the millisecond with
// the Summary session span, so gaps at stitch joints / the final boundary
// within this many ms count as covered (served from SD, not re-spooled).
// Seconds only - a genuine interior gap (minutes) is NOT masked.
static constexpr int64_t AC_REPORT_COVERAGE_TOLERANCE_MS = 5000;

