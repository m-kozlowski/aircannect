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
static constexpr uint32_t AC_BG_WORKER_TASK_STACK = 8192;
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
// After therapy stops, AS11 may still be draining live stream/report traffic and
// AirCANnect is closing EDF/SMB work. Let the bus settle before refreshing the
// report summary index that triggers cache backfill.
static constexpr uint32_t AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS = 30000;
// Post-therapy SMB sync normally waits for the report refresh/backfill to go
// idle so STR/journal/report-derived files have settled first. Do not let a
// wedged report backfill suppress auto-sync forever; after this, queue sync
// anyway while still respecting the background worker's stream/therapy/OTA gate.
static constexpr uint32_t AC_REPORT_POST_THERAPY_SYNC_MAX_WAIT_MS = 180000;
// SleepHQ runs after report settle and SMB sync. If that serialized post-
// therapy window never opens, drop this immediate trigger and let idle backfill
// recover later instead of keeping the post-therapy lane armed forever.
static constexpr uint32_t AC_SLEEPHQ_POST_THERAPY_SYNC_MAX_WAIT_MS = 180000;
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
// Default manual quota policy: keep roughly the last 90 therapy nights of
// cached report payloads. Summary records remain intact.
static constexpr size_t AC_REPORT_CACHE_QUOTA_NIGHTS = 90;
static constexpr size_t AC_REPORT_SOURCE_EVENT_DRAIN_BUDGET = 1;
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
// Minimum spacing between PullSpoolFragments requests. The response is a CAN
// burst; this keeps report backfill from immediately requesting the next burst
// before the arbiter has observed queue pressure from the previous one.
static constexpr uint32_t AC_REPORT_SPOOL_PULL_PACE_MS = 100;
static constexpr size_t AC_REPORT_SUMMARY_SPOOL_ROUND_BYTES = 8192;
static constexpr size_t AC_REPORT_CACHE_SPOOL_ROUND_BYTES = 8192;

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
