#pragma once

// Report and export task config, prefetch policy, and result/plot tuning.

#include <stddef.h>
#include <stdint.h>

#include "report_range_tile.h"

// New report pipeline owner. It shares core 0 with storage at the same low
// priority and yields after every bounded decode step, so storage's EDF lane
// and the main loop remain responsive.
static constexpr uint32_t AC_REPORT_TASK_STACK = 8192;
static constexpr uint8_t AC_REPORT_TASK_PRIO = 1;
static constexpr uint8_t AC_REPORT_TASK_CORE = 0;
static constexpr uint32_t AC_REPORT_TASK_WORK_TICK_MS = 1;
static constexpr uint32_t AC_REPORT_TASK_WAIT_TICK_MS = 5;
static constexpr uint32_t AC_REPORT_TASK_IDLE_TICK_MS = 1000;
static constexpr size_t AC_REPORT_TASK_COMMAND_CAPACITY = 8;
static constexpr size_t AC_REPORT_TASK_BUILD_CAPACITY = 8;

// SMB and SleepHQ share one low-priority task so their network and storage
// phases cannot overlap. Runtime activity snapshots preempt both engines.
static constexpr uint32_t AC_EXPORT_TASK_STACK = 8192;
static constexpr uint8_t AC_EXPORT_TASK_PRIO = 1;
static constexpr uint8_t AC_EXPORT_TASK_CORE = 0;
static constexpr uint32_t AC_EXPORT_TASK_BUSY_RECHECK_MS = 250;
static constexpr uint32_t AC_EXPORT_TASK_WORK_TICK_MS = 20;
static constexpr uint32_t AC_EXPORT_TASK_IDLE_TICK_MS = 10000;
static constexpr uint32_t AC_EXPORT_ACTIVITY_GRACE_MS = 3000;

// After therapy stops, AS11 may still be draining live stream/report traffic and
// AirCANnect is closing EDF/SMB work. Let the bus settle before refreshing the
// report summary index that triggers cache backfill.
static constexpr uint32_t AC_REPORT_POST_THERAPY_SUMMARY_DELAY_MS = 30000;
// Post-therapy SMB sync normally waits for the report refresh/backfill to go
// idle so STR/journal/report-derived files have settled first. Do not let a
// wedged report backfill suppress auto-sync forever; after this, queue sync
// anyway while still respecting export-task activity policy.
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
// Idle full-overview plot prebuild rescans after draining the current night
// list. Summary/catalog revision changes reset this immediately.
static constexpr uint32_t AC_REPORT_PLOT_PREBUILD_RESCAN_MS = 300000;
// Default manual quota policy: keep roughly the last 90 therapy nights of
// cached report payloads. Summary records remain intact.
static constexpr size_t AC_REPORT_CACHE_QUOTA_NIGHTS = 90;
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

// Report result assembly and plot build.
// Full-night overview plot: target min/max envelope buckets across a night's
// session span. Range/zoom plots use their own high-detail budget below.
static constexpr size_t AC_REPORT_PLOT_BUCKETS = 2560;
// Range plot caps. Overview deliberately does not use these per-rate caps:
// using one common envelope bucket size keeps the full-night payload bounded
// without losing short peaks.
static constexpr int64_t AC_REPORT_HIGH_RATE_PLOT_BUCKET_MAX_MS = 2000;
static constexpr int64_t AC_REPORT_LOW_RATE_OVERVIEW_BUCKET_MAX_MS = 20000;
static constexpr uint32_t AC_REPORT_LOW_RATE_NATIVE_PLOT_MIN_INTERVAL_MS = 500;
static constexpr uint32_t AC_REPORT_LOW_RATE_NATIVE_PLOT_MAX_INTERVAL_MS =
    60000;
// Range plots may preserve slow native-cadence signals when the requested
// viewport is already detailed enough.
static constexpr uint32_t AC_REPORT_RANGE_PLOT_PRESERVE_INTERVAL_MIN_MS = 500;
static constexpr size_t AC_REPORT_PLOT_MAX_BYTES = 2 * 1024 * 1024;
// Initial reserve for the incrementally-built plot binary.
static constexpr size_t AC_REPORT_PLOT_INITIAL_RESERVE = 256 * 1024;
// Plot builders run only while realtime CAN/therapy work is idle. Bound by time
// instead of bytes so large EDF-derived chunks cannot monopolize a poll pass.
static constexpr uint32_t AC_REPORT_PLOT_POLL_BUDGET_MS = 10;
static constexpr size_t AC_REPORT_PLOT_POLL_CHUNK_CAP = 32;
// Range (zoom) plot: keep detail bounded to roughly display resolution.
static constexpr size_t AC_REPORT_RANGE_PLOT_BUCKETS = 1800;
static constexpr size_t AC_REPORT_RANGE_PLOT_MAX_BYTES = 2 * 1024 * 1024;
static constexpr size_t AC_REPORT_RANGE_MAX_POINTS = 32768;
static constexpr uint32_t AC_REPORT_RANGE_PLOT_POLL_BUDGET_MS = 10;
static constexpr size_t AC_REPORT_RANGE_PLOT_POLL_CHUNK_CAP = 32;
static constexpr int64_t AC_REPORT_RANGE_TILE_MS =
    aircannect::REPORT_RANGE_TILE_MS;
// Range plots are the high-detail zoom path. Requests wider than this should
// use the full-night plot instead of forcing a near-full rebuild through the
// foreground range endpoint.
static constexpr int64_t AC_REPORT_RANGE_PLOT_MAX_WINDOW_MS =
    3LL * 60LL * 60LL * 1000LL;
// Latest-night tail refresh re-fetches this far back to overlap cached data so
// the seam has no gap.
static constexpr int64_t AC_REPORT_LATEST_TAIL_OVERLAP_MS = 60000;
// Coverage tolerance: spool boundaries rarely line up to the millisecond with
// the Summary session span, so gaps at stitch joints / the final boundary
// within this many ms count as covered (served from SD, not re-spooled).
// Seconds only - a genuine interior gap (minutes) is NOT masked.
static constexpr int64_t AC_REPORT_COVERAGE_TOLERANCE_MS = 5000;
// Summary/session ranges are minute-granular, while EVE annotations carry exact
// end time + duration. Keep scored events just outside a rounded boundary; this
// does not relax numeric-series coverage.
static constexpr int64_t AC_REPORT_EVENT_EDGE_TOLERANCE_MS = 60000;
