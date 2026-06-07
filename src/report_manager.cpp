#include "report_manager.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "debug_log.h"
#include "json_util.h"
#include "memory_manager.h"
#include "report_records.h"
#include "report_store.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

const char *const REPORT_SUMMARY_FROM = "2000-01-01T00:00:00.000Z";
constexpr const char *REPORT_PLOT_CACHE_DIR = "/aircannect/report/v2/plots/v10";
constexpr size_t REPORT_PLOT_PATH_MAX = 128;

struct ChunkWriteContext {
    ReportManager *manager = nullptr;
    ReportSourceId source = ReportSourceId::Summary;
    uint32_t chunks = 0;
};

struct ResultChunkContext {
    ReportManager *manager = nullptr;
    ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
    ReportSourceId source = ReportSourceId::Summary;
    ReportSignalId signal = ReportSignalId::Flow;
    const char *name = nullptr;
    bool required = false;
    size_t stream_index = 0;
};

struct LatestChunkEndContext {
    bool found = false;
    int64_t latest_end_ms = 0;
};

struct SummaryRecordBufferContext {
    ReportSummaryRecord *records = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    uint32_t nights_with_therapy = 0;
};

struct ReportSessionRange {
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

void append_u64(LargeTextBuffer &out, uint64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu",
             static_cast<unsigned long long>(value));
    out += buf;
}

void append_long(LargeTextBuffer &out, long value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%ld", value);
    out += buf;
}

// Plot binary wire format (little-endian), served at /api/report/plot:
//   header: magic u32 'ACPB', version u16, flags u16, base_ms i64
//   events: count u32, then count * { t_delta i32, duration i32, code i32, flags i32 }
//   series: to EOF, repeated { name_len u16, name bytes, point_count u32,
//           point_count * { t_delta i32, value_milli i32 } }
// Timestamps are i32 deltas from base_ms; values stay in milli-units and the
// client divides by 1000.
constexpr uint32_t PLOT_BIN_MAGIC = 0x42504341u;  // "ACPB"
constexpr uint16_t PLOT_BIN_VERSION = 1;

bool bin_put_u16(ReportSpoolBuffer &b, uint16_t v) {
    const uint8_t x[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return b.append(x, sizeof(x));
}
bool bin_put_u32(ReportSpoolBuffer &b, uint32_t v) {
    const uint8_t x[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                          static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    return b.append(x, sizeof(x));
}
bool bin_put_i32(ReportSpoolBuffer &b, int32_t v) {
    return bin_put_u32(b, static_cast<uint32_t>(v));
}
bool bin_put_i64(ReportSpoolBuffer &b, int64_t v) {
    const uint64_t u = static_cast<uint64_t>(v);
    return bin_put_u32(b, static_cast<uint32_t>(u)) &&
           bin_put_u32(b, static_cast<uint32_t>(u >> 32));
}

void append_optional_float(LargeTextBuffer &json,
                           const char *key,
                           bool present,
                           float value) {
    if (present) json_add_float(json, key, value);
}

bool format_utc_ms_iso(uint64_t ms, std::string &out) {
    const time_t seconds = static_cast<time_t>(ms / 1000);
    struct tm tmv;
    if (!gmtime_r(&seconds, &tmv)) return false;
    char buf[32];
    if (!strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv)) {
        return false;
    }
    out = buf;
    out += ".000Z";
    return true;
}

size_t collect_session_ranges(const ReportSummaryRecord &night,
                              ReportSessionRange *ranges,
                              size_t max_ranges) {
    if (!ranges || !max_ranges || !night.valid || !night.duration_min) {
        return 0;
    }

    size_t count = 0;
    for (uint32_t i = 0; i < night.session_interval_count &&
                         count < max_ranges; ++i) {
        const ReportSummarySession &session = night.sessions[i];
        if (!session.start_ms || !session.duration_min) continue;
        ReportSessionRange &range = ranges[count];
        range.start_ms = static_cast<int64_t>(session.start_ms);
        range.end_ms = range.start_ms +
                       static_cast<int64_t>(session.duration_min) * 60000LL;
        if (range.end_ms > range.start_ms) count++;
    }

    if (count == 0 && night.end_ms > night.start_ms) {
        ranges[0].start_ms = static_cast<int64_t>(night.start_ms);
        ranges[0].end_ms = static_cast<int64_t>(night.end_ms);
        count = 1;
    }

    std::sort(ranges, ranges + count,
              [](const ReportSessionRange &a,
                 const ReportSessionRange &b) {
                  return a.start_ms < b.start_ms;
              });
    return count;
}

// A night's end_ms is a 24h day bucket, far past the actual therapy data. The
// meaningful coverage span is the session data range [first session start, last
// session end]; use it for all coverage claims and completeness checks so a
// night is "complete" when its sessions are covered, not the empty hours after.
bool night_data_span(const ReportSummaryRecord &night,
                     int64_t &span_start,
                     int64_t &span_end) {
    ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t count =
        collect_session_ranges(night, ranges, AC_REPORT_SUMMARY_SESSION_MAX);
    if (count == 0) return false;
    span_start = ranges[0].start_ms;
    span_end = ranges[0].end_ms;
    for (size_t i = 1; i < count; ++i) {
        if (ranges[i].start_ms < span_start) span_start = ranges[i].start_ms;
        if (ranges[i].end_ms > span_end) span_end = ranges[i].end_ms;
    }
    return span_end > span_start;
}

static bool event_coverage_dropped(const ReportSummaryRecord &night,
                                   const ReportSourceDef &source);

bool source_complete_for_night(const ReportSummaryRecord &night,
                               const ReportSourceDef &source) {
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;
    if (!ReportStore::coverage_complete(
            source.spool_type, span_start, span_end, source.parser_schema)) {
        return false;
    }
    if (event_coverage_dropped(night, source)) return false;
    return true;
}

// Series sources stream continuous samples; event sources are sparse (a covered
// range may legitimately contain zero events). Used to decide whether a night
// must show delivered samples before its coverage is claimed.
bool source_is_sampled(const ReportSourceDef &source) {
    return (source.purposes &
            (REPORT_SOURCE_TREND_SERIES | REPORT_SOURCE_HIGH_RES_SERIES)) != 0;
}

bool store_summary_record_to_buffer(void *context,
                                    const ReportSummaryRecord &record) {
    SummaryRecordBufferContext *ctx =
        static_cast<SummaryRecordBufferContext *>(context);
    if (!ctx || !ctx->records || ctx->count >= ctx->capacity) {
        return false;
    }
    ctx->records[ctx->count++] = record;
    if (record.duration_min > 0) ctx->nights_with_therapy++;
    return true;
}

// A built result is "ready" only when required coverage is complete; otherwise it
// is displayable best-effort but "partial" (the doc reserves complete for full
// required coverage, so a degraded night must not be presented as a valid one).
ReportResultState settled_result_state(uint32_t missing_required) {
    return missing_required == 0 ? ReportResultState::Ready
                                 : ReportResultState::Partial;
}

bool source_missing_start_for_night(const ReportSummaryRecord &night,
                                    const ReportSourceDef &source,
                                    int64_t &out_start_ms) {
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;

    int64_t missing_ms = span_start;
    if (!ReportStore::coverage_first_missing(
            source.spool_type,
            span_start,
            span_end,
            source.parser_schema,
            missing_ms)) {
        out_start_ms = span_start;
        return true;
    }
    if (missing_ms >= span_end) {
        if (event_coverage_dropped(night, source)) {
            out_start_ms = span_start;
            return true;
        }
        return false;
    }
    out_start_ms = missing_ms;
    return true;
}

bool remember_latest_chunk_end(void *context,
                               const ReportStoreChunkInfo &chunk) {
    LatestChunkEndContext *ctx =
        static_cast<LatestChunkEndContext *>(context);
    if (!ctx) return false;
    if (!ctx->found || chunk.key.end_ms > ctx->latest_end_ms) {
        ctx->found = true;
        ctx->latest_end_ms = chunk.key.end_ms;
    }
    return true;
}

bool latest_chunk_end_for_name(ReportStoreChunkKind kind,
                               const char *source,
                               const char *name,
                               int64_t night_start_ms,
                               int64_t start_ms,
                               int64_t end_ms,
                               int64_t &out_end_ms) {
    LatestChunkEndContext ctx;
    if (!ReportStore::for_each_chunk(kind,
                                     source,
                                     name,
                                     night_start_ms,
                                     start_ms,
                                     end_ms,
                                     remember_latest_chunk_end,
                                     &ctx)) {
        return false;
    }
    if (!ctx.found) return false;
    out_end_ms = ctx.latest_end_ms;
    return true;
}

bool source_latest_cached_end_for_night(const ReportSourceDef &source,
                                        const ReportSummaryRecord &night,
                                        int64_t &out_end_ms) {
    const int64_t start_ms = static_cast<int64_t>(night.start_ms);
    const int64_t end_ms = static_cast<int64_t>(night.end_ms);
    const char *spool_type = source.spool_type;
    if (!spool_type || !spool_type[0]) return false;

    if (source.id == ReportSourceId::UsageEvents ||
        source.id == ReportSourceId::RespiratoryEvents) {
        return latest_chunk_end_for_name(ReportStoreChunkKind::Events,
                                         spool_type,
                                         spool_type,
                                         start_ms,
                                         start_ms,
                                         end_ms,
                                         out_end_ms);
    }

    bool matched = false;
    int64_t earliest_latest_end = 0;
    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        const ReportSignalDef &signal = signals[i];
        if (signal.preferred_source != source.id &&
            signal.fallback_source != source.id) {
            continue;
        }
        int64_t signal_end = 0;
        if (!latest_chunk_end_for_name(ReportStoreChunkKind::Series,
                                       spool_type,
                                       signal.store_name,
                                       start_ms,
                                       start_ms,
                                       end_ms,
                                       signal_end)) {
            return false;
        }
        if (!matched || signal_end < earliest_latest_end) {
            earliest_latest_end = signal_end;
        }
        matched = true;
    }
    if (!matched) return false;
    out_end_ms = earliest_latest_end;
    return true;
}

// Events outlive the 6.25Hz high-res, so event-coverage-complete but no cached
// event chunks while the high-res is still cached means the fetch was dropped,
// not a real zero -> report it as a gap to re-fetch.
static bool event_coverage_dropped(const ReportSummaryRecord &night,
                                   const ReportSourceDef &source) {
    if (source.id != ReportSourceId::RespiratoryEvents &&
        source.id != ReportSourceId::UsageEvents) {
        return false;
    }
    int64_t end_ms = 0;
    if (source_latest_cached_end_for_night(source, night, end_ms)) {
        return false;  // event chunks present, nothing was dropped
    }
    const ReportSourceDef *high_res =
        report_source_def(ReportSourceId::RespiratoryFlow6p25Hz);
    if (!high_res) return false;
    return source_latest_cached_end_for_night(*high_res, night, end_ms);
}

bool source_required_for_report_result(ReportSourceId source) {
    switch (source) {
        case ReportSourceId::RespiratoryEvents:
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            return true;
        default:
            return false;
    }
}

bool report_ranges_overlap(int64_t a_start,
                           int64_t a_end,
                           int64_t b_start,
                           int64_t b_end) {
    return a_start < b_end && b_start < a_end;
}

bool report_time_in_ranges(int64_t value,
                           const ReportSessionRange *ranges,
                           size_t range_count) {
    if (!ranges || value <= 0) return false;
    for (size_t i = 0; i < range_count; ++i) {
        if (value >= ranges[i].start_ms && value <= ranges[i].end_ms) {
            return true;
        }
    }
    return false;
}

bool same_report_event(const ReportEventRecord &a,
                       const ReportEventRecord &b) {
    return a.start_ms == b.start_ms &&
           a.duration_ms == b.duration_ms &&
           a.code == b.code &&
           a.flags == b.flags;
}

bool report_event_seen(const ReportSpoolBuffer &seen,
                       const ReportEventRecord &event) {
    const size_t count = seen.size() / report_event_record_wire_size();
    for (size_t i = 0; i < count; ++i) {
        ReportEventRecord current;
        if (!report_read_event_record(seen.data(),
                                      seen.size(),
                                      i,
                                      current)) {
            continue;
        }
        if (same_report_event(current, event)) return true;
    }
    return false;
}

bool remember_report_event(ReportSpoolBuffer &seen,
                           const ReportEventRecord &event) {
    if (report_event_seen(seen, event)) return true;
    return report_append_event_record(seen, event);
}

}  // namespace

ReportManager::~ReportManager() {
    Memory::free(records_);
    records_ = nullptr;
    Memory::free(summary_scratch_);
    summary_scratch_ = nullptr;
    Memory::free(night_epochs_);
    night_epochs_ = nullptr;
    night_epoch_count_ = 0;
    if (result_slots_) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            result_slots_[i].~MaterializedResult();
        }
        Memory::free(result_slots_);
        result_slots_ = nullptr;
    }
    Memory::free(result_chunks_);
    result_chunks_ = nullptr;
    result_chunk_capacity_ = 0;
    if (summary_lock_) {
        vSemaphoreDelete(summary_lock_);
        summary_lock_ = nullptr;
    }
    if (summary_scratch_lock_) {
        vSemaphoreDelete(summary_scratch_lock_);
        summary_scratch_lock_ = nullptr;
    }
    if (result_slots_lock_) {
        vSemaphoreDelete(result_slots_lock_);
        result_slots_lock_ = nullptr;
    }
    if (build_queue_lock_) {
        vSemaphoreDelete(build_queue_lock_);
        build_queue_lock_ = nullptr;
    }
    if (prefetch_lock_) {
        vSemaphoreDelete(prefetch_lock_);
        prefetch_lock_ = nullptr;
    }
}

void ReportManager::begin() {
    if (!summary_lock_) summary_lock_ = xSemaphoreCreateMutex();
    if (!summary_scratch_lock_) summary_scratch_lock_ = xSemaphoreCreateMutex();
    if (!prefetch_lock_) prefetch_lock_ = xSemaphoreCreateMutex();
    if (!result_slots_lock_) result_slots_lock_ = xSemaphoreCreateMutex();
    if (!build_queue_lock_) build_queue_lock_ = xSemaphoreCreateMutex();
    if (!night_epochs_) {
        night_epochs_ = static_cast<NightEpoch *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX, sizeof(NightEpoch)));
        if (!night_epochs_) {
            Log::logf(CAT_RPC, LOG_WARN,
                      "[REPORT] night epoch PSRAM allocation failed\n");
        }
    }
    if (!result_slots_) {
        result_slots_ = static_cast<MaterializedResult *>(Memory::alloc_large(
            AC_REPORT_RESULT_SLOT_MAX * sizeof(MaterializedResult)));
        if (result_slots_) {
            for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
                new (&result_slots_[i]) MaterializedResult();
            }
        } else {
            Log::logf(CAT_RPC, LOG_WARN,
                      "[REPORT] materialized result PSRAM allocation failed\n");
        }
    }
    clear_summary_records();
    summary_status_ = {};
    if (!load_summary_from_store()) {
        publish_summary_json_snapshot();
    }
}

bool ReportManager::take_summary_lock(TickType_t timeout) const {
    return !summary_lock_ || xSemaphoreTake(summary_lock_, timeout) == pdTRUE;
}

void ReportManager::give_summary_lock() const {
    if (summary_lock_) xSemaphoreGive(summary_lock_);
}

bool ReportManager::take_summary_scratch(TickType_t timeout,
                                         ReportSummaryRecord *&out) {
    out = nullptr;
    if (!summary_scratch_lock_ ||
        xSemaphoreTake(summary_scratch_lock_, timeout) != pdTRUE) {
        return false;
    }
    if (!summary_scratch_) {
        summary_scratch_ = static_cast<ReportSummaryRecord *>(
            Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                                 sizeof(ReportSummaryRecord)));
    }
    if (!summary_scratch_) {
        xSemaphoreGive(summary_scratch_lock_);
        return false;
    }
    out = summary_scratch_;
    return true;
}

void ReportManager::give_summary_scratch() {
    if (summary_scratch_lock_) xSemaphoreGive(summary_scratch_lock_);
}

bool ReportManager::request_summary_refresh(bool force) {
    if (summary_fetch_active_ && !force) return true;
    if (cache_fetch_active_) return false;

    SpoolClientRequest request;
    request.spool_type = "Summary";
    request.from_dt = REPORT_SUMMARY_FROM;
    request.max_size = 65536;
    request.fragment_max = 2808;
    request.max_rounds = 16;
    request.stream_rounds = false;

    if (!spool_.begin(request)) {
        fail_summary("summary_start_failed");
        return false;
    }

    summary_fetch_active_ = true;
    summary_started_ms_ = millis();
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Fetching;
        summary_status_.active_spool = "Summary";
        summary_status_.error.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }
    publish_summary_json_snapshot();
    Log::logf(CAT_RPC, LOG_INFO, "[REPORT] Summary refresh queued\n");
    return true;
}

void ReportManager::poll(RpcArbiter &arbiter) {
    service_build_queue();
    service_range_plot();
    service_prefetch();
    // Publish the summary revision for the background prefetch job (cross-task).
    if (take_summary_lock(0)) {
        summary_revision_pub_.store(summary_status_.revision);
        give_summary_lock();
    }
    if (!summary_fetch_active_ && !cache_fetch_active_ &&
        !plot_build_active_ &&
        static_cast<int32_t>(millis() - next_trash_cleanup_ms_) >= 0) {
        next_trash_cleanup_ms_ = millis() + 250;
        uint32_t removed = 0;
        ReportStore::cleanup_trash_step(4, removed);
    }

    if (cache_fetch_active_) {
        poll_cache_fetch(arbiter);
    }
    if (plot_build_active_) {
        poll_result_plot_build();
        return;
    }
    if (!summary_fetch_active_) return;

    spool_.poll(arbiter);
    bool publish_progress = false;
    const uint32_t now_ms = millis();
    if (take_summary_lock(0)) {
        summary_status_.spool = spool_.status();
        summary_status_.elapsed_ms = summary_started_ms_
            ? now_ms - summary_started_ms_
            : 0;
        give_summary_lock();
        if (static_cast<int32_t>(now_ms - next_summary_progress_snapshot_ms_) >=
            0) {
            next_summary_progress_snapshot_ms_ = now_ms + 500;
            publish_progress = true;
        }
    }
    if (publish_progress) publish_summary_json_snapshot();

    if (spool_.complete()) {
        finish_summary_fetch();
    } else if (spool_.failed()) {
        fail_summary(spool_.status().error.c_str());
    }
}

bool ReportManager::handle_event(const RpcEvent &event) {
    if (cache_fetch_active_ && spool_.handle_event(event)) return true;
    if (summary_fetch_active_ && spool_.handle_event(event)) return true;
    return false;
}

bool ReportManager::write_parsed_chunk(void *context,
                                       const ReportParsedChunk &chunk) {
    ChunkWriteContext *ctx = static_cast<ChunkWriteContext *>(context);
    if (!ctx || !ctx->manager || !chunk.name || !chunk.name[0] ||
        !chunk.payload || chunk.payload_schema == 0 ||
        chunk.record_count == 0 ||
        chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        return false;
    }
    if (!report_source_spool_type(chunk.source)) return false;
    return ctx->manager->buffer_parsed_chunk(chunk);
}

// Map a timestamp to its summary-night bucket start (the partition key). Bucket
// boundaries sit around local noon (no therapy), so a chunk straddling one is
// filed whole by its start timestamp.
int64_t ReportManager::night_start_for_timestamp(int64_t timestamp_ms) const {
    if (!take_summary_lock(portMAX_DELAY)) return timestamp_ms;
    if (!records_ || record_count_ == 0) {
        give_summary_lock();
        return timestamp_ms;
    }
    int64_t nearest_start = 0;
    bool have_nearest = false;
    for (size_t i = 0; i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        const ReportSummaryRecord &r = records_[i];
        if (!r.valid || !r.duration_min) continue;
        const int64_t s = static_cast<int64_t>(r.start_ms);
        const int64_t e = static_cast<int64_t>(r.end_ms);
        if (timestamp_ms >= s && timestamp_ms < e) {
            give_summary_lock();
            return s;
        }
        if (s <= timestamp_ms && (!have_nearest || s > nearest_start)) {
            nearest_start = s;
            have_nearest = true;
        }
    }
    const int64_t result = have_nearest ? nearest_start : timestamp_ms;
    give_summary_lock();
    return result;
}

// Coalesce parsed chunks per (kind,name); flush on night change, session gap,
// or size cap. The source's final buffer is flushed at source completion.
bool ReportManager::buffer_parsed_chunk(const ReportParsedChunk &chunk) {
    const int64_t night = night_start_for_timestamp(chunk.start_ms);

    size_t slot = AC_REPORT_COALESCE_SLOTS;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (cache_coalesce_[i].active &&
            cache_coalesce_[i].kind == chunk.kind &&
            cache_coalesce_[i].name == chunk.name) {
            slot = i;
            break;
        }
    }

    if (slot != AC_REPORT_COALESCE_SLOTS) {
        CacheCoalesceBuffer &buf = cache_coalesce_[slot];
        // Break the chunk at a session gap, else its [start,end] spans the gap,
        // derive_result_session_ranges() merges the sessions, and the chart
        // bridges it. Series only - events are sparse and have no sessions.
        const bool session_gap =
            chunk.kind == ReportStoreChunkKind::Series &&
            chunk.start_ms - buf.last_ms > AC_REPORT_SESSION_GAP_MS;
        if (buf.night_start_ms != night || session_gap ||
            buf.payload.size() + chunk.payload_len >
                AC_REPORT_COALESCE_TARGET_BYTES) {
            if (!flush_cache_coalesce_buffer(slot)) return false;
            slot = AC_REPORT_COALESCE_SLOTS;
        }
    }

    if (slot == AC_REPORT_COALESCE_SLOTS) {
        for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
            if (!cache_coalesce_[i].active) {
                slot = i;
                break;
            }
        }
        if (slot == AC_REPORT_COALESCE_SLOTS) {
            if (!flush_cache_coalesce_buffer(0)) return false;
            slot = 0;
        }
        CacheCoalesceBuffer &buf = cache_coalesce_[slot];
        buf.active = true;
        buf.kind = chunk.kind;
        buf.source = chunk.source;
        buf.name = chunk.name;
        buf.night_start_ms = night;
        buf.first_ms = chunk.start_ms;
        buf.last_ms = chunk.end_ms;
        buf.record_count = 0;
        buf.payload_schema = chunk.payload_schema;
        buf.payload.clear();
        buf.payload.set_max_size(AC_REPORT_COALESCE_TARGET_BYTES + 65536);
    }

    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    if (!buf.payload.append(chunk.payload, chunk.payload_len)) {
        fail_cache_fetch("cache_coalesce_alloc");
        return false;
    }
    if (chunk.start_ms < buf.first_ms) buf.first_ms = chunk.start_ms;
    if (chunk.end_ms > buf.last_ms) buf.last_ms = chunk.end_ms;
    buf.record_count += chunk.record_count;
    note_cache_chunk_coverage(chunk);
    return true;
}

bool ReportManager::flush_cache_coalesce_buffer(size_t slot) {
    if (slot >= AC_REPORT_COALESCE_SLOTS) return true;
    CacheCoalesceBuffer &buf = cache_coalesce_[slot];
    if (!buf.active) return true;

    bool ok = true;
    if (buf.record_count > 0 && buf.payload.size() > 0 &&
        buf.last_ms > buf.first_ms) {
        ReportStoreChunkKey key;
        key.kind = buf.kind;
        key.source = report_source_spool_type(buf.source);
        key.name = buf.name;
        key.start_ms = buf.first_ms;
        key.end_ms = buf.last_ms;
        key.night_start_ms = buf.night_start_ms;
        if (!key.source || !key.source[0]) {
            ok = false;
        } else {
            ReportStoreChunkMeta meta;
            meta.payload_schema = buf.payload_schema;
            meta.record_count = buf.record_count;
            ok = ReportStore::write_chunk(key, meta, buf.payload.data(),
                                          buf.payload.size());
            if (ok) {
                cache_status_.chunks_written++;
                ++cache_data_epoch_;
                bump_night_epoch(buf.night_start_ms);
            }
        }
    }

    buf.active = false;
    buf.payload.clear();
    buf.record_count = 0;
    buf.first_ms = 0;
    buf.last_ms = 0;
    buf.night_start_ms = 0;
    buf.name = nullptr;
    return ok;
}

bool ReportManager::flush_all_cache_coalesce_buffers() {
    bool ok = true;
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        if (!flush_cache_coalesce_buffer(i)) ok = false;
    }
    return ok;
}

void ReportManager::discard_cache_coalesce_buffers() {
    for (size_t i = 0; i < AC_REPORT_COALESCE_SLOTS; ++i) {
        CacheCoalesceBuffer &buf = cache_coalesce_[i];
        buf.active = false;
        buf.payload.clear();
        buf.record_count = 0;
        buf.first_ms = 0;
        buf.last_ms = 0;
        buf.night_start_ms = 0;
        buf.name = nullptr;
    }
}

bool ReportManager::ensure_summary_records() {
    if (records_) return true;
    records_ = static_cast<ReportSummaryRecord *>(
        Memory::calloc_large(AC_REPORT_SUMMARY_RECORD_MAX,
                             sizeof(ReportSummaryRecord)));
    if (!records_) {
        fail_summary("summary_alloc_failed");
        return false;
    }
    return true;
}

bool ReportManager::parse_summary_result(ReportSpoolResult &result) {
    if (!ensure_summary_records()) return false;
    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) {
        fail_summary("summary_staging_alloc_failed");
        return false;
    }
    memset(staging, 0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;
    char error[64] = {};
    if (!report_parse_summary_spool(result,
                                    store_summary_record_to_buffer,
                                    &context,
                                    error,
                                    sizeof(error))) {
        give_summary_scratch();
        fail_summary(error[0] ? error : "summary_parse_failed");
        return false;
    }
    size_t write_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;
    finalize_summary_records();
    write_count = record_count_;
    if (write_count) {
        memcpy(staging, records_, write_count * sizeof(ReportSummaryRecord));
    }
    give_summary_lock();
    if (write_count &&
        !ReportStore::write_summary_records(staging, write_count)) {
        Log::logf(CAT_RPC, LOG_WARN,
                  "[REPORT] Summary store write failed records=%lu\n",
                  static_cast<unsigned long>(write_count));
    }
    give_summary_scratch();
    invalidate_materialized(0, true);
    return true;
}

bool ReportManager::load_summary_from_store() {
    if (!ensure_summary_records()) return false;
    ReportSummaryRecord *staging = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, staging)) return false;
    memset(staging, 0,
           AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    SummaryRecordBufferContext context;
    context.records = staging;
    context.capacity = AC_REPORT_SUMMARY_RECORD_MAX;
    if (!ReportStore::read_summary_records(store_summary_record_to_buffer,
                                           &context)) {
        give_summary_scratch();
        return false;
    }
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    clear_summary_records();
    if (context.count) {
        memcpy(records_, staging,
               context.count * sizeof(ReportSummaryRecord));
    }
    record_count_ = context.count;
    nights_with_therapy_ = context.nights_with_therapy;
    finalize_summary_records();
    summary_status_.state = ReportSummaryState::Ready;
    summary_status_.revision++;
    summary_status_.error.clear();
    summary_status_.active_spool.clear();
    const uint32_t records_total = summary_status_.records_total;
    const uint32_t nights_with_therapy = summary_status_.nights_with_therapy;
    give_summary_lock();
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] Summary loaded from store records=%lu "
              "therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));
    give_summary_scratch();
    publish_summary_json_snapshot();
    invalidate_materialized(0, true);
    return true;
}

void ReportManager::finalize_summary_records() {
    if (records_ && record_count_ > 1) {
        std::sort(records_, records_ + record_count_,
                  [](const ReportSummaryRecord &a,
                     const ReportSummaryRecord &b) {
                      return a.start_ms < b.start_ms;
                  });
    }
    summary_status_.records_total = static_cast<uint32_t>(record_count_);
    summary_status_.nights_with_therapy = nights_with_therapy_;
}

void ReportManager::finish_summary_fetch() {
    ReportSpoolResult result;
    spool_.move_result_to(result);
    summary_fetch_active_ = false;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.active_spool.clear();
        summary_status_.spool = spool_.status();
        give_summary_lock();
    }
    if (!parse_summary_result(result)) return;
    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Ready;
        summary_status_.revision++;
        summary_status_.error.clear();
        records_total = summary_status_.records_total;
        nights_with_therapy = summary_status_.nights_with_therapy;
        give_summary_lock();
    }
    publish_summary_json_snapshot();
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] Summary ready records=%lu therapy_nights=%lu\n",
              static_cast<unsigned long>(records_total),
              static_cast<unsigned long>(nights_with_therapy));
    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        const bool refresh_cache = pending_result_refresh_cache_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index,
                                                 refresh_cache);
    }
}

void ReportManager::fail_summary(const char *message) {
    summary_fetch_active_ = false;
    std::string error;
    if (take_summary_lock(portMAX_DELAY)) {
        summary_status_.state = ReportSummaryState::Error;
        summary_status_.revision++;
        summary_status_.active_spool.clear();
        summary_status_.error = message ? message : "summary_error";
        summary_status_.spool = spool_.status();
        error = summary_status_.error;
        give_summary_lock();
    } else {
        error = message ? message : "summary_error";
    }
    Log::logf(CAT_RPC, LOG_WARN, "[REPORT] Summary failed: %s\n",
              error.c_str());
    publish_summary_json_snapshot();
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(error.c_str());
    }
}

void ReportManager::clear_summary_records() {
    if (records_) {
        memset(records_, 0,
               AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord));
    }
    record_count_ = 0;
    nights_with_therapy_ = 0;
}

static const char *summary_state_name_for(ReportSummaryState state) {
    switch (state) {
        case ReportSummaryState::Idle: return "idle";
        case ReportSummaryState::Fetching: return "fetching";
        case ReportSummaryState::Ready: return "ready";
        case ReportSummaryState::Error: return "error";
    }
    return "unknown";
}

const char *ReportManager::summary_state_name() const {
    return summary_state_name_for(summary_status().state);
}

ReportSummaryStatus ReportManager::summary_status() const {
    ReportSummaryStatus snapshot;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        snapshot.state = ReportSummaryState::Error;
        snapshot.error = "summary_busy";
        return snapshot;
    }
    snapshot = summary_status_;
    give_summary_lock();
    return snapshot;
}

void append_summary_json_from_records(LargeTextBuffer &json,
                                      const ReportSummaryStatus &status_snapshot,
                                      uint32_t data_epoch_snapshot,
                                      const ReportSummaryRecord *snapshot,
                                      size_t record_count_snapshot) {
    json.clear();
    if (record_count_snapshot > AC_REPORT_SUMMARY_RECORD_MAX) {
        record_count_snapshot = AC_REPORT_SUMMARY_RECORD_MAX;
    }
    if (!snapshot) record_count_snapshot = 0;

    json += "{";
    json_add_string(json,
                    "state",
                    summary_state_name_for(status_snapshot.state),
                    false);
    json_add_int(json, "revision",
                 static_cast<long>(status_snapshot.revision));
    json_add_int(json, "data_epoch",
                 static_cast<long>(data_epoch_snapshot));
    json_add_string(json, "error", status_snapshot.error.c_str());
    json_add_int(json, "records_total",
                 static_cast<long>(status_snapshot.records_total));
    json_add_int(json, "nights_with_therapy",
                 static_cast<long>(status_snapshot.nights_with_therapy));
    json_add_int(json, "elapsed_ms",
                 static_cast<long>(status_snapshot.elapsed_ms));
    json_add_string(json, "active_spool",
                    status_snapshot.active_spool.c_str());
    json += ",\"spool\":{\"state\":\"";
    json += spool_client_state_name(status_snapshot.spool.state);
    json += "\",\"round\":";
    append_long(json, static_cast<long>(status_snapshot.spool.current_round));
    json += ",\"fragments\":";
    append_long(json, static_cast<long>(status_snapshot.spool.fragments));
    json += ",\"bytes\":";
    append_long(json, static_cast<long>(status_snapshot.spool.bytes));
    json += "},\"nights\":[";
    bool first = true;
    size_t therapy_seen = 0;
    for (size_t i = 0; i < record_count_snapshot; ++i) {
        const ReportSummaryRecord &record = snapshot[i];
        if (!record.valid || record.duration_min == 0) continue;
        if (!first) json += ',';
        first = false;
        const size_t therapy_index =
            status_snapshot.nights_with_therapy > therapy_seen
                ? status_snapshot.nights_with_therapy - therapy_seen - 1
                : 0;
        therapy_seen++;
        json += "{";
        json_add_int(json, "index", static_cast<long>(i), false);
        json_add_int(json, "therapy_index",
                     static_cast<long>(therapy_index));
        json_add_uint64(json, "start", record.start_ms);
        json_add_uint64(json, "end", record.end_ms);
        json_add_int(json, "duration_min",
                     static_cast<long>(record.duration_min));
        if (record.has_tz_offset_min) {
            json_add_int(json, "tz_offset_min",
                         static_cast<long>(record.tz_offset_min));
        }
        append_optional_float(json, "ahi", record.has_ahi, record.ahi);
        append_optional_float(json, "apnea_index",
                              record.has_apnea_index,
                              record.apnea_index);
        append_optional_float(json, "hypopnea_index",
                              record.has_hypopnea_index,
                              record.hypopnea_index);
        append_optional_float(json, "oa_index",
                              record.has_oa_index,
                              record.oa_index);
        append_optional_float(json, "ca_index",
                              record.has_ca_index,
                              record.ca_index);
        append_optional_float(json, "ua_index",
                              record.has_ua_index,
                              record.ua_index);
        append_optional_float(json, "rera_index",
                              record.has_rera_index,
                              record.rera_index);
        if (record.has_session_count) {
            json_add_int(json, "session_count",
                         static_cast<long>(record.session_count));
        }
        json += ",\"sessions\":[";
        for (uint32_t session_index = 0;
             session_index < record.session_interval_count;
             ++session_index) {
            const ReportSummarySession &session =
                record.sessions[session_index];
            if (session_index) json += ',';
            json += "{";
            json_add_uint64(json, "start", session.start_ms, false);
            json_add_int(json, "duration_min",
                         static_cast<long>(session.duration_min));
            json += "}";
        }
        json += "]";
        json += ",\"id\":\"";
        append_u64(json, record.start_ms);
        json += "\"}";
    }
    json += "]}";
}

void ReportManager::publish_summary_json_snapshot() {
    ReportSummaryRecord *records_snapshot = nullptr;
    const bool have_scratch =
        take_summary_scratch(portMAX_DELAY, records_snapshot);
    ReportSummaryStatus status_snapshot;
    uint32_t data_epoch_snapshot = 0;
    size_t record_count_snapshot = 0;

    if (take_summary_lock(portMAX_DELAY)) {
        status_snapshot = summary_status_;
        data_epoch_snapshot = cache_data_epoch_;
        record_count_snapshot = record_count_;
        if (record_count_snapshot > AC_REPORT_SUMMARY_RECORD_MAX) {
            record_count_snapshot = AC_REPORT_SUMMARY_RECORD_MAX;
        }
        if (records_snapshot && records_ && record_count_snapshot) {
            memcpy(records_snapshot,
                   records_,
                   record_count_snapshot * sizeof(ReportSummaryRecord));
        }
        give_summary_lock();
    } else {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_busy";
    }

    if (!have_scratch && record_count_snapshot) {
        status_snapshot.state = ReportSummaryState::Error;
        status_snapshot.error = "summary_snapshot_alloc";
        record_count_snapshot = 0;
    }

    summary_json_build_.clear();
    append_summary_json_from_records(summary_json_build_,
                                     status_snapshot,
                                     data_epoch_snapshot,
                                     records_snapshot,
                                     record_count_snapshot);
    if (have_scratch) give_summary_scratch();

    if (summary_json_build_.overflowed()) {
        Log::logf(CAT_RPC, LOG_WARN,
                  "[REPORT] Summary JSON snapshot allocation failed\n");
        summary_json_build_ =
            "{\"state\":\"error\",\"error\":\"summary_snapshot_alloc\","
            "\"nights\":[]}";
    }
    summary_json_snapshot_.swap(summary_json_build_);
}

void ReportManager::build_summary_json(LargeTextBuffer &json) const {
    if (!summary_json_snapshot_.length()) {
        json = "{\"state\":\"idle\",\"error\":\"summary_snapshot_missing\","
               "\"nights\":[]}";
        return;
    }
    json.clear();
    json.append(summary_json_snapshot_.c_str(), summary_json_snapshot_.length());
}

void ReportManager::bump_night_epoch(uint64_t night_start_ms) {
    if (!night_start_ms || !night_epochs_) return;
    if (!take_summary_lock(portMAX_DELAY)) return;
    for (size_t i = 0; i < night_epoch_count_; ++i) {
        if (night_epochs_[i].night_start_ms == night_start_ms) {
            night_epochs_[i].epoch++;
            give_summary_lock();
            return;
        }
    }
    if (night_epoch_count_ < AC_REPORT_SUMMARY_RECORD_MAX) {
        night_epochs_[night_epoch_count_].night_start_ms = night_start_ms;
        night_epochs_[night_epoch_count_].epoch = 1;
        ++night_epoch_count_;
    }
    give_summary_lock();
}

uint32_t ReportManager::night_epoch_for_unlocked(uint64_t night_start_ms) const {
    if (!night_epochs_) return 0;
    for (size_t i = 0; i < night_epoch_count_; ++i) {
        if (night_epochs_[i].night_start_ms == night_start_ms) {
            return night_epochs_[i].epoch;
        }
    }
    return 0;
}

void ReportManager::format_night_etag(const ReportSummaryRecord &rec, char *out,
                                      size_t out_size) const {
    if (!take_summary_lock(portMAX_DELAY)) {
        if (out && out_size) out[0] = '\0';
        return;
    }
    format_night_etag_unlocked(rec, out, out_size);
    give_summary_lock();
}

void ReportManager::format_night_etag_unlocked(const ReportSummaryRecord &rec,
                                               char *out,
                                               size_t out_size) const {
    if (!out || !out_size) return;
    snprintf(out, out_size, "%llu-%lu-%lu-%lu",
             static_cast<unsigned long long>(rec.start_ms),
             static_cast<unsigned long>(rec.duration_min),
             static_cast<unsigned long>(rec.session_interval_count),
             static_cast<unsigned long>(night_epoch_for_unlocked(rec.start_ms)));
}

bool ReportManager::night_etag(size_t therapy_index, char *out,
                               size_t out_size) const {
    ReportSummaryRecord rec;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    if (!summary_night_by_therapy_index_unlocked(therapy_index, rec)) {
        give_summary_lock();
        return false;
    }
    format_night_etag_unlocked(rec, out, out_size);
    give_summary_lock();
    return true;
}

static const char *result_state_name_for(ReportResultState state) {
    switch (state) {
        case ReportResultState::Idle: return "idle";
        case ReportResultState::Preparing: return "preparing";
        case ReportResultState::Ready: return "ready";
        case ReportResultState::Incomplete: return "incomplete";
        case ReportResultState::Partial: return "partial";
        case ReportResultState::Error: return "error";
    }
    return "unknown";
}

void ReportManager::build_result_json_from(
    const ReportResultStatus &result_status_,
    const ReportSummaryRecord &result_night_,
    const ReportResultStream *result_streams_,
    size_t result_stream_count_,
    const ReportCacheFetchStatus &cache_status_,
    LargeTextBuffer &json) const {
    json.clear();
    json += "{";
    json_add_string(json, "state", result_state_name_for(result_status_.state),
                    false);
    json_add_string(json, "error", result_status_.error.c_str());
    json_add_int(json, "therapy_index",
                 static_cast<long>(result_status_.therapy_index));
    json_add_uint64(json, "start", result_status_.night_start_ms);
    json_add_uint64(json, "end", result_status_.night_end_ms);
    json_add_int(json, "duration_min",
                 static_cast<long>(result_status_.duration_min));
    json_add_int(json, "missing_required",
                 static_cast<long>(result_status_.missing_required));
    json_add_int(json, "missing_streams",
                 static_cast<long>(result_status_.missing_streams));
    json_add_int(json, "streams",
                 static_cast<long>(result_status_.stream_count));
    json_add_int(json, "chunks",
                 static_cast<long>(result_status_.chunk_count));
    json_add_int(json, "records",
                 static_cast<long>(result_status_.record_count));
    json_add_int(json, "bytes",
                 static_cast<long>(result_status_.payload_bytes));
    json += ",\"cache\":{";
    json_add_bool(json, "active", cache_status_.active, false);
    json_add_int(json, "revision",
                 static_cast<long>(cache_status_.revision));
    json_add_string(json,
                    "source",
                    report_source_spool_type(cache_status_.active_source));
    json_add_int(json,
                 "source_index",
                 static_cast<long>(cache_status_.source_index));
    json_add_int(json,
                 "source_count",
                 static_cast<long>(cache_status_.source_count));
    json_add_int(json,
                 "chunks_written",
                 static_cast<long>(cache_status_.chunks_written));
    json_add_string(json, "error", cache_status_.error.c_str());
    json_add_string(json,
                    "spool_state",
                    spool_client_state_name(cache_status_.spool.state));
    json_add_int(json,
                 "spool_round",
                 static_cast<long>(cache_status_.spool.current_round));
    json_add_int(json,
                 "spool_fragments",
                 static_cast<long>(cache_status_.spool.fragments));
    json_add_int(json,
                 "spool_bytes",
                 static_cast<long>(cache_status_.spool.bytes));
    json_add_int(json,
                 "spool_elapsed_ms",
                 static_cast<long>(cache_status_.spool.elapsed_ms));
    json += "}";
    append_optional_float(json,
                          "ahi",
                          result_status_.event_metrics_valid,
                          result_status_.ahi);
    append_optional_float(json,
                          "oa_index",
                          result_status_.event_metrics_valid,
                          result_status_.oa_index);
    append_optional_float(json,
                          "ca_index",
                          result_status_.event_metrics_valid,
                          result_status_.ca_index);
    append_optional_float(json,
                          "ua_index",
                          result_status_.event_metrics_valid,
                          result_status_.ua_index);
    append_optional_float(json,
                          "hypopnea_index",
                          result_status_.event_metrics_valid,
                          result_status_.hypopnea_index);
    append_optional_float(json,
                          "arousal_index",
                          result_status_.event_metrics_valid,
                          result_status_.arousal_index);
    json_add_int(json, "oa_count",
                 static_cast<long>(result_status_.oa_count));
    json_add_int(json, "ca_count",
                 static_cast<long>(result_status_.ca_count));
    json_add_int(json, "ua_count",
                 static_cast<long>(result_status_.ua_count));
    json_add_int(json, "hypopnea_count",
                 static_cast<long>(result_status_.hypopnea_count));
    json_add_int(json, "arousal_count",
                 static_cast<long>(result_status_.arousal_count));
    json_add_bool(json, "events_available", result_status_.events_available);
    json += ",\"sessions\":[";
    for (uint32_t i = 0; i < result_night_.session_interval_count; ++i) {
        const ReportSummarySession &session = result_night_.sessions[i];
        if (i) json += ',';
        json += "{";
        json_add_uint64(json, "start", session.start_ms, false);
        json_add_int(json, "duration_min",
                     static_cast<long>(session.duration_min));
        json += "}";
    }
    json += "],\"stream_details\":[";
    for (size_t i = 0; i < result_stream_count_; ++i) {
        const ReportResultStream &stream = result_streams_[i];
        if (i) json += ',';
        json += "{";
        json_add_string(json,
                        "kind",
                        ReportStore::kind_name(stream.kind),
                        false);
        json_add_string(json,
                        "source",
                        report_source_spool_type(stream.source));
        json_add_string(json, "name", stream.name ? stream.name : "");
        json_add_bool(json, "required", stream.required);
        json_add_bool(json, "complete", stream.complete);
        json_add_int(json, "chunks",
                     static_cast<long>(stream.chunk_count));
        json_add_int(json, "records",
                     static_cast<long>(stream.record_count));
        json_add_int(json, "bytes",
                     static_cast<long>(stream.payload_bytes));
        // low_res: this series fell back to a lower-resolution source than the
        // signal prefers (high-res aged out -> 1-minute trend); the UI badges it.
        bool low_res = false;
        if (stream.kind == ReportStoreChunkKind::Series) {
            size_t signal_def_count = 0;
            const ReportSignalDef *signal_defs =
                report_signal_defs(signal_def_count);
            for (size_t s = 0; s < signal_def_count; ++s) {
                if (signal_defs[s].id == stream.signal) {
                    low_res = stream.source != signal_defs[s].preferred_source;
                    break;
                }
            }
        }
        json_add_bool(json, "low_res", low_res);
        json += "}";
    }
    json += "]}";
}

void ReportManager::publish_result_to_slot() {
    if (!result_slots_lock_ || !result_slots_) return;
    // The ETag is the materialized-result version. It must match the plot blob
    // published beside it, so reads never recompute it from a different model
    // such as the raw Summary record.
    char etag[48];
    format_night_etag(result_night_, etag, sizeof(etag));
    if (!etag[0]) return;

    // Snapshot the plot bytes outside the slot lock (the copy is the heavy
    // part); a shared_ptr lets a GET stream it even after the blob is evicted.
    std::shared_ptr<ReportSpoolBuffer> plot;
    if (result_plot_bin_.size() > 0) {
        plot = std::make_shared<ReportSpoolBuffer>();
        plot->set_max_size(result_plot_bin_.size());
        if (!plot->reserve_capacity(result_plot_bin_.size()) ||
            !plot->append(result_plot_bin_.data(), result_plot_bin_.size())) {
            plot.reset();
        }
    }
    if (result_plot_bin_.size() > 0 && !plot) {
        Log::logf(CAT_RPC,
                  LOG_WARN,
                  "[REPORT] Result publish skipped: plot snapshot failed "
                  "index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        return;
    }
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            result_slots_[i].night_start_ms == result_night_.start_ms) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
            if (!result_slots_[i].valid) {
                pick = i;
                break;
            }
            if (result_slots_[i].last_used < result_slots_[pick].last_used) {
                pick = i;
            }
        }
    }
    MaterializedResult &slot = result_slots_[pick];
    slot.valid = true;
    slot.night_start_ms = result_night_.start_ms;
    snprintf(slot.etag, sizeof(slot.etag), "%s", etag);
    slot.status = result_status_;
    slot.night = result_night_;
    slot.stream_count = result_stream_count_;
    for (size_t i = 0; i < AC_REPORT_RESULT_STREAM_MAX; ++i) {
        slot.streams[i] = (i < result_stream_count_) ? result_streams_[i]
                                                     : ReportResultStream{};
    }
    slot.plot = plot;
    slot.last_used = ++result_slot_tick_;
    xSemaphoreGive(result_slots_lock_);
}

void ReportManager::invalidate_materialized_locked(uint64_t night_start_ms,
                                                   bool all) {
    if (!result_slots_) return;
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            (all || result_slots_[i].night_start_ms == night_start_ms)) {
            result_slots_[i] = MaterializedResult{};
        }
    }
}

void ReportManager::invalidate_materialized(uint64_t night_start_ms, bool all) {
    if (!result_slots_lock_) return;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    invalidate_materialized_locked(night_start_ms, all);
    xSemaphoreGive(result_slots_lock_);
}

ReportManager::ResultRead ReportManager::read_result(
    size_t therapy_index, const char *if_none_match, char *etag_out,
    size_t etag_out_size, LargeTextBuffer &json_out) {
    if (!result_slots_ || !result_slots_lock_) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::Unavailable;
    }
    ReportSummaryRecord rec;
    char etag[48];
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::Busy;
    }
    if (!summary_night_by_therapy_index_unlocked(therapy_index, rec)) {
        give_summary_lock();
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ResultRead::NotFound;
    }
    give_summary_lock();

    ReportResultStatus status;
    ReportSummaryRecord night;
    ReportResultStream streams[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t stream_count = 0;
    bool found = false;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid &&
            result_slots_[i].night_start_ms == rec.start_ms) {
            result_slots_[i].last_used = ++result_slot_tick_;
            snprintf(etag, sizeof(etag), "%s", result_slots_[i].etag);
            status = result_slots_[i].status;
            night = result_slots_[i].night;
            stream_count = result_slots_[i].stream_count;
            if (stream_count > AC_REPORT_RESULT_STREAM_MAX) {
                stream_count = AC_REPORT_RESULT_STREAM_MAX;
            }
            for (size_t k = 0; k < stream_count; ++k) {
                streams[k] = result_slots_[i].streams[k];
            }
            found = true;
            break;
        }
    }
    xSemaphoreGive(result_slots_lock_);
    if (found) {
        if (etag_out && etag_out_size) {
            snprintf(etag_out, etag_out_size, "%s", etag);
        }
        if (if_none_match && if_none_match[0] &&
            strcmp(if_none_match, etag) == 0) {
            return ResultRead::NotModified;
        }
        const ReportCacheFetchStatus inactive{};
        build_result_json_from(status, night, streams, stream_count, inactive,
                               json_out);
        return ResultRead::Ready;
    }
    if (etag_out && etag_out_size) etag_out[0] = '\0';

    switch (enqueue_build(rec.start_ms, therapy_index, false)) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return ResultRead::Building;
        case BuildQueueResult::Full:
            return ResultRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return ResultRead::Unavailable;
    }
}

ReportManager::PlotRead ReportManager::read_plot(
    size_t therapy_index, const char *version,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!result_slots_ || !result_slots_lock_) {
        return PlotRead::Unavailable;
    }
    ReportSummaryRecord rec;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        return PlotRead::Busy;
    }
    if (!summary_night_by_therapy_index_unlocked(therapy_index, rec)) {
        give_summary_lock();
        return PlotRead::NotFound;
    }
    give_summary_lock();
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    for (size_t i = 0; i < AC_REPORT_RESULT_SLOT_MAX; ++i) {
        if (result_slots_[i].valid && result_slots_[i].plot &&
            result_slots_[i].night_start_ms == rec.start_ms &&
            (!version || !version[0] ||
             strcmp(result_slots_[i].etag, version) == 0)) {
            result_slots_[i].last_used = ++result_slot_tick_;
            out = result_slots_[i].plot;
            xSemaphoreGive(result_slots_lock_);
            return PlotRead::Ready;
        }
    }
    xSemaphoreGive(result_slots_lock_);
    switch (enqueue_build(rec.start_ms, therapy_index, false)) {
        case BuildQueueResult::Queued:
        case BuildQueueResult::AlreadyQueued:
            return PlotRead::Building;
        case BuildQueueResult::Full:
            return PlotRead::QueueFull;
        case BuildQueueResult::Unavailable:
        default:
            return PlotRead::Unavailable;
    }
}

void ReportManager::build_result_chunks_json(LargeTextBuffer &json,
                                             size_t offset,
                                             size_t limit) const {
    if (limit > 128) limit = 128;
    const size_t total = result_status_.chunk_count;
    if (offset > total) offset = total;
    const size_t end = std::min(total, offset + limit);

    json.clear();
    json += "{";
    json_add_string(json, "state", result_state_name(), false);
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "limit", static_cast<long>(limit));
    json_add_int(json, "total", static_cast<long>(total));
    json_add_bool(json, "more", end < total);
    json += ",\"chunks\":[";
    for (size_t i = offset; i < end; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (i != offset) json += ',';
        json += "{";
        json_add_int(json, "id", static_cast<long>(i), false);
        json_add_string(json,
                        "kind",
                        ReportStore::kind_name(chunk.kind));
        json_add_string(json,
                        "source",
                        report_source_spool_type(chunk.source));
        json_add_string(json, "name", chunk.name ? chunk.name : "");
        json_add_uint64(json,
                        "start",
                        static_cast<uint64_t>(chunk.start_ms));
        json_add_uint64(json,
                        "end",
                        static_cast<uint64_t>(chunk.end_ms));
        json_add_int(json, "schema",
                     static_cast<long>(chunk.payload_schema));
        json_add_int(json, "records",
                     static_cast<long>(chunk.record_count));
        json_add_int(json, "bytes",
                     static_cast<long>(chunk.payload_len));
        json += "}";
    }
    json += "]}";
}

bool ReportManager::night_coverage(uint64_t night_start_ms,
                                   ReportNightCoverageStatus &out) const {
    out = {};
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    ReportSummaryRecord night;
    const bool found = summary_night_by_start_unlocked(night_start_ms, night);
    give_summary_lock();
    if (!found) return false;

    out.found = true;
    out.start_ms = night.start_ms;
    out.end_ms = night.end_ms;
    out.duration_min = night.duration_min;

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);
    for (size_t i = 0; i < source_count &&
                       out.source_count < AC_REPORT_NIGHT_SOURCE_MAX; ++i) {
        const ReportSourceDef &source = sources[i];
        if (source.id == ReportSourceId::Summary) continue;
        if (!cache_source_supported(source.id)) continue;
        ReportNightSourceCoverage &entry = out.sources[out.source_count++];
        entry.source = source.id;
        entry.required = source_required_for_report_result(source.id);
        entry.complete = source_complete_for_night(night, source);
        if (entry.required && !entry.complete) out.missing_required++;
    }
    return true;
}

bool ReportManager::next_night_needing_cache(
    uint64_t &night_start_ms_out) const {
    const uint32_t now = millis();
    // Oldest-first: the spool is open-ended (fromDateTime -> now), so fetching
    // the OLDEST night with a gap streams every source from there forward and
    // backfills all newer nights in a single sweep (deduped on write)
    for (size_t i = 0; i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
        if (!records_ || i >= record_count_) {
            give_summary_lock();
            return false;
        }
        const ReportSummaryRecord record = records_[i];
        give_summary_lock();
        if (!record.valid || !record.duration_min) continue;
        if (prefetch_in_cooldown(record.start_ms, now)) continue;
        ReportNightCoverageStatus coverage;
        if (!night_coverage(record.start_ms, coverage)) continue;
        if (coverage.missing_required > 0) {
            night_start_ms_out = record.start_ms;
            return true;
        }
    }
    return false;
}

bool ReportManager::for_each_summary_night(
    ReportSummaryNightCallback callback,
    void *context) const {
    if (!callback) return false;

    ReportSummaryRecord *snapshot =
        static_cast<ReportSummaryRecord *>(Memory::alloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX * sizeof(ReportSummaryRecord)));
    if (!snapshot) return false;

    bool any = false;
    size_t therapy_index = 0;
    size_t count = 0;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) {
        Memory::free(snapshot);
        return false;
    }
    count = records_ ? record_count_ : 0;
    if (count > AC_REPORT_SUMMARY_RECORD_MAX) {
        count = AC_REPORT_SUMMARY_RECORD_MAX;
    }
    if (records_ && count) {
        memcpy(snapshot, records_, count * sizeof(ReportSummaryRecord));
    }
    give_summary_lock();
    for (size_t i = count; i > 0; --i) {
        const size_t summary_index = i - 1;
        const ReportSummaryRecord record = snapshot[summary_index];
        if (!record.valid || record.duration_min == 0) continue;

        ReportSummaryNight night;
        night.summary_index = summary_index;
        night.therapy_index = therapy_index++;
        night.record = record;
        any = true;
        if (!callback(context, night)) break;
    }
    Memory::free(snapshot);
    return any;
}

bool ReportManager::summary_night_by_therapy_index(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    const bool ok = summary_night_by_therapy_index_unlocked(therapy_index, out);
    give_summary_lock();
    return ok;
}

bool ReportManager::summary_night_by_therapy_index_unlocked(
    size_t therapy_index,
    ReportSummaryRecord &out) const {
    if (!records_) return false;

    size_t current = 0;
    for (size_t i = record_count_; i > 0; --i) {
        const ReportSummaryRecord &record = records_[i - 1];
        if (!record.valid || record.duration_min == 0) continue;
        if (current == therapy_index) {
            out = record;
            return true;
        }
        current++;
    }
    return false;
}

bool ReportManager::summary_night_by_start_unlocked(
    uint64_t night_start_ms,
    ReportSummaryRecord &out) const {
    if (!records_) return false;
    for (size_t i = 0; i < record_count_; ++i) {
        const ReportSummaryRecord &record = records_[i];
        if (record.valid && record.start_ms == night_start_ms) {
            out = record;
            return true;
        }
    }
    return false;
}

bool ReportManager::latest_summary_night(ReportSummaryRecord &out) const {
    return summary_night_by_therapy_index(0, out);
}

bool ReportManager::night_coverage_by_therapy_index(
    size_t therapy_index,
    ReportNightCoverageStatus &out) const {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return night_coverage(night.start_ms, out);
}

bool ReportManager::latest_night_coverage(
    ReportNightCoverageStatus &out) const {
    return night_coverage_by_therapy_index(0, out);
}

bool ReportManager::request_night_cache(uint64_t night_start_ms, bool force) {
    if (summary_fetch_active_ || cache_fetch_active_) return false;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    ReportSummaryRecord night;
    const bool found = summary_night_by_start_unlocked(night_start_ms, night);
    give_summary_lock();
    if (!found || night.duration_min == 0) return false;
    if (!build_cache_plan(night, force, false)) return false;
    if (cache_source_count_ == 0) {
        finish_cache_fetch();
        return true;
    }
    return start_next_cache_source();
}

bool ReportManager::request_night_cache_by_therapy_index(
    size_t therapy_index,
    bool force) {
    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) return false;
    return request_night_cache(night.start_ms, force);
}

bool ReportManager::request_latest_night_cache(bool force) {
    return request_night_cache_by_therapy_index(0, force);
}

bool ReportManager::prefetch_in_cooldown(uint64_t night_ms,
                                         uint32_t now_ms) const {
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms &&
            prefetch_skip_[i].until_ms != 0 &&
            static_cast<int32_t>(now_ms - prefetch_skip_[i].until_ms) < 0) {
            return true;
        }
    }
    return false;
}

void ReportManager::prefetch_note_failure(uint64_t night_ms) {
    uint32_t until = millis() + AC_REPORT_PREFETCH_FAIL_COOLDOWN_MS;
    if (until == 0) until = 1;
    size_t pick = 0;
    for (size_t i = 0; i < PREFETCH_SKIP_MAX; ++i) {
        if (prefetch_skip_[i].night_ms == night_ms ||
            prefetch_skip_[i].night_ms == 0) {
            pick = i;
            break;
        }
        if (prefetch_skip_[i].until_ms < prefetch_skip_[pick].until_ms) pick = i;
    }
    prefetch_skip_[pick].night_ms = night_ms;
    prefetch_skip_[pick].until_ms = until;
}

void ReportManager::set_prefetch_phase(PrefetchPhase phase,
                                       uint64_t night_ms,
                                       bool inc_completed,
                                       bool inc_failed) {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    prefetch_phase_ = phase;
    prefetch_active_night_ = night_ms;
    if (inc_completed) prefetch_completed_++;
    if (inc_failed) prefetch_failed_++;
    xSemaphoreGive(prefetch_lock_);
}

bool ReportManager::prefetch_request_next() {
    if (!prefetch_lock_) return false;
    bool accepted = false;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    if (prefetch_phase_ != PrefetchPhase::Pending &&
        prefetch_phase_ != PrefetchPhase::Fetching) {
        prefetch_phase_ = PrefetchPhase::Pending;
        accepted = true;
    }
    xSemaphoreGive(prefetch_lock_);
    return accepted;
}

void ReportManager::prefetch_cancel() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    prefetch_cancel_req_ = true;
    xSemaphoreGive(prefetch_lock_);
}

ReportManager::PrefetchSnapshot ReportManager::prefetch_snapshot() const {
    PrefetchSnapshot snap;
    if (!prefetch_lock_) return snap;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    snap.phase = prefetch_phase_;
    snap.night_ms = prefetch_active_night_;
    snap.completed = prefetch_completed_;
    snap.failed = prefetch_failed_;
    xSemaphoreGive(prefetch_lock_);
    return snap;
}

bool ReportManager::foreground_busy() const {
    if (summary_fetch_active_ || plot_build_active_) return true;
    if (build_queue_lock_) {
        xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
        const bool queued = build_queue_count_ > 0;
        xSemaphoreGive(build_queue_lock_);
        if (queued) return true;
    }
    if (!cache_fetch_active_) return false;
    // A cache fetch is in flight: it's foreground unless it's the prefetch's own.
    if (!prefetch_lock_) return true;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool prefetch_owned = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);
    return !prefetch_owned;
}

void ReportManager::prefetch_yield_to_foreground() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const bool owns = (prefetch_phase_ == PrefetchPhase::Fetching);
    xSemaphoreGive(prefetch_lock_);
    if (!owns) return;
    if (cache_fetch_active_) {
        spool_.reset();
        cache_fetch_active_ = false;
        cache_status_.active = false;
        cache_status_.revision++;
        cache_status_.error = "preempted_by_user";
    }
    set_prefetch_phase(PrefetchPhase::Failed, 0, false, true);
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] prefetch yielded to foreground prepare\n");
}

ReportManager::BuildQueueResult ReportManager::enqueue_build(
    uint64_t night_start_ms,
    size_t therapy_index,
    bool refresh) {
    if (!build_queue_lock_ || !night_start_ms) {
        return BuildQueueResult::Unavailable;
    }
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    for (size_t k = 0; k < build_queue_count_; ++k) {
        size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        if (build_queue_[idx].night_start_ms == night_start_ms) {
            if (refresh) build_queue_[idx].refresh = true;
            xSemaphoreGive(build_queue_lock_);
            return BuildQueueResult::AlreadyQueued;
        }
    }
    if (build_queue_count_ < AC_REPORT_BUILD_QUEUE_MAX) {
        size_t tail =
            (build_queue_head_ + build_queue_count_) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_[tail].night_start_ms = night_start_ms;
        build_queue_[tail].therapy_index = therapy_index;
        build_queue_[tail].refresh = refresh;
        build_queue_count_++;
        xSemaphoreGive(build_queue_lock_);
        return BuildQueueResult::Queued;
    }
    xSemaphoreGive(build_queue_lock_);
    return BuildQueueResult::Full;
}

void ReportManager::clear_build_queue(uint64_t night_start_ms, bool all) {
    if (!build_queue_lock_) return;
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    ResultBuildJob kept[AC_REPORT_BUILD_QUEUE_MAX];
    size_t kept_count = 0;
    for (size_t k = 0; k < build_queue_count_; ++k) {
        const size_t idx = (build_queue_head_ + k) % AC_REPORT_BUILD_QUEUE_MAX;
        const ResultBuildJob &job = build_queue_[idx];
        if (!all && job.night_start_ms != night_start_ms) {
            kept[kept_count++] = job;
        }
    }
    for (size_t i = 0; i < AC_REPORT_BUILD_QUEUE_MAX; ++i) {
        build_queue_[i] = i < kept_count ? kept[i] : ResultBuildJob{};
    }
    build_queue_head_ = 0;
    build_queue_count_ = kept_count;
    xSemaphoreGive(build_queue_lock_);
}

void ReportManager::service_build_queue() {
    if (!build_queue_lock_) return;
    if (summary_fetch_active_ || plot_build_active_) return;
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    const bool have = build_queue_count_ > 0;
    ResultBuildJob job = have ? build_queue_[build_queue_head_] : ResultBuildJob{};
    xSemaphoreGive(build_queue_lock_);
    if (!have) return;
    // A foreground build preempts an idle prefetch fetch.
    if (cache_fetch_active_) {
        prefetch_yield_to_foreground();
        if (cache_fetch_active_) return;  // a non-prefetch fetch is in flight; wait
    }
    xSemaphoreTake(build_queue_lock_, portMAX_DELAY);
    if (build_queue_count_ > 0 &&
        build_queue_[build_queue_head_].night_start_ms == job.night_start_ms) {
        build_queue_head_ = (build_queue_head_ + 1) % AC_REPORT_BUILD_QUEUE_MAX;
        build_queue_count_--;
    }
    xSemaphoreGive(build_queue_lock_);
    prepare_result_by_therapy_index_internal(job.therapy_index, job.refresh);
}

void ReportManager::service_prefetch() {
    if (!prefetch_lock_) return;
    xSemaphoreTake(prefetch_lock_, portMAX_DELAY);
    const PrefetchPhase phase = prefetch_phase_;
    const bool cancel = prefetch_cancel_req_;
    prefetch_cancel_req_ = false;
    const uint64_t active = prefetch_active_night_;
    xSemaphoreGive(prefetch_lock_);

    if (cancel && (phase == PrefetchPhase::Fetching ||
                   phase == PrefetchPhase::Pending)) {
        if (cache_fetch_active_) {
            spool_.reset();
            cache_fetch_active_ = false;
            cache_status_.active = false;
            cache_status_.revision++;
            cache_status_.error = "prefetch_cancelled";
        }
        set_prefetch_phase(PrefetchPhase::Failed, 0, false, true);
        return;
    }

    if (phase == PrefetchPhase::Fetching && !cache_fetch_active_) {
        // The fetch concluded inside poll_cache_fetch; success = night covered.
        ReportNightCoverageStatus coverage;
        const bool covered =
            night_coverage(active, coverage) && coverage.missing_required == 0;
        if (!covered) prefetch_note_failure(active);
        set_prefetch_phase(covered ? PrefetchPhase::Done : PrefetchPhase::Failed,
                           0, covered, !covered);
        return;
    }

    if (phase == PrefetchPhase::Pending && !busy()) {
        uint64_t night = 0;
        if (next_night_needing_cache(night) &&
            request_night_cache(night, false)) {
            set_prefetch_phase(PrefetchPhase::Fetching, night, false, false);
        } else {
            set_prefetch_phase(PrefetchPhase::Drained, 0, false, false);
        }
    }
}

bool ReportManager::clear_plot_cache_for_night(const ReportSummaryRecord &night,
                                               uint32_t &deleted) {
    deleted = 0;
    char path[REPORT_PLOT_PATH_MAX];
    if (!result_plot_cache_path_for_night(night, path, sizeof(path))) {
        return false;
    }
    const bool existed = Storage::exists(path);
    if (!Storage::remove(path)) return false;
    if (existed) deleted = 1;
    return true;
}

bool ReportManager::clear_cache_range(int64_t start_ms,
                                      int64_t end_ms,
                                      ReportCacheClearResult &out) {
    if (start_ms < 0 || end_ms <= start_ms) return false;

    ReportSummaryRecord *night_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, night_batch)) return false;

    size_t night_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        return false;
    }
    for (size_t n = 0; records_ && n < record_count_ &&
                       n < AC_REPORT_SUMMARY_RECORD_MAX; ++n) {
        const ReportSummaryRecord &r = records_[n];
        if (!r.valid) continue;
        const int64_t ns = static_cast<int64_t>(r.start_ms);
        const int64_t ne = static_cast<int64_t>(r.end_ms);
        if (ne <= ns || ns >= end_ms || ne <= start_ms) continue;
        if (night_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;
        night_batch[night_count++] = r;
    }
    give_summary_lock();

    bool ok = true;
    uint32_t deleted = 0;

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);
    for (size_t i = 0; i < source_count; ++i) {
        const ReportSourceDef &source = sources[i];
        if (source.id == ReportSourceId::Summary) continue;
        if (!source.spool_type || !source.spool_type[0]) continue;

        deleted = 0;
        if (!ReportStore::clear_coverage(source.spool_type,
                                         start_ms,
                                         end_ms,
                                         deleted)) {
            ok = false;
        }
        out.coverage_deleted += deleted;

        // Chunks live in per-night dirs; clear each night overlapping the range.
        for (size_t n = 0; n < night_count; ++n) {
            const ReportSummaryRecord &r = night_batch[n];
            const int64_t ns = static_cast<int64_t>(r.start_ms);
            const int64_t ne = static_cast<int64_t>(r.end_ms);
            deleted = 0;
            if (!ReportStore::clear_chunks(ReportStoreChunkKind::Events,
                                           source.spool_type,
                                           source.spool_type,
                                           ns,
                                           start_ms,
                                           end_ms,
                                           deleted)) {
                ok = false;
            }
            out.chunks_deleted += deleted;
        }
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t i = 0; i < signal_count; ++i) {
        const ReportSignalDef &signal = signals[i];
        const ReportSourceId source_ids[] = {
            signal.preferred_source,
            signal.fallback_source,
        };
        for (ReportSourceId source_id : source_ids) {
            const ReportSourceDef *source = report_source_def(source_id);
            if (!source || source->id == ReportSourceId::Summary ||
                !source->spool_type || !source->spool_type[0] ||
                !signal.store_name || !signal.store_name[0]) {
                continue;
            }
            for (size_t n = 0; n < night_count; ++n) {
                const ReportSummaryRecord &r = night_batch[n];
                const int64_t ns = static_cast<int64_t>(r.start_ms);
                const int64_t ne = static_cast<int64_t>(r.end_ms);
                deleted = 0;
                if (!ReportStore::clear_chunks(ReportStoreChunkKind::Series,
                                               source->spool_type,
                                               signal.store_name,
                                               ns,
                                               start_ms,
                                               end_ms,
                                               deleted)) {
                    ok = false;
                }
                out.chunks_deleted += deleted;
            }
        }
    }
    give_summary_scratch();
    return ok;
}

bool ReportManager::clear_cache_all(ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_) {
        return false;
    }

    uint32_t store_reset = 0;
    if (!ReportStore::reset_cache_store(store_reset)) {
        return false;
    }
    out.store_reset = store_reset;

    clear_build_queue(0, true);
    invalidate_materialized(0, true);

    if (!take_summary_lock(portMAX_DELAY)) return false;
    clear_summary_records();
    night_epoch_count_ = 0;
    summary_status_.state = ReportSummaryState::Idle;
    summary_status_.revision++;
    summary_status_.records_total = 0;
    summary_status_.nights_with_therapy = 0;
    summary_status_.elapsed_ms = 0;
    summary_status_.active_spool.clear();
    summary_status_.error.clear();
    give_summary_lock();
    publish_summary_json_snapshot();

    clear_result_prepare();
    return true;
}

bool ReportManager::clear_cache_night(uint64_t night_start_ms,
                                      ReportCacheClearResult &out) {
    out = {};
    if (summary_fetch_active_ || cache_fetch_active_ || plot_build_active_) {
        return false;
    }

    ReportSummaryRecord night;
    if (!take_summary_lock(pdMS_TO_TICKS(20))) return false;
    const bool found = summary_night_by_start_unlocked(night_start_ms, night);
    give_summary_lock();
    if (!found || night.end_ms <= night.start_ms) return false;
    clear_build_queue(night.start_ms, false);
    invalidate_materialized(night.start_ms, false);

    const bool ok = clear_cache_range(static_cast<int64_t>(night.start_ms),
                                      static_cast<int64_t>(night.end_ms),
                                      out);
    uint32_t plot_deleted = 0;
    if (clear_plot_cache_for_night(night, plot_deleted)) {
        out.plots_deleted += plot_deleted;
    }
    if (result_status_.night_start_ms == night.start_ms) {
        clear_result_prepare();
    }
    if (take_summary_lock(portMAX_DELAY)) {
        for (size_t i = 0; i < night_epoch_count_; ++i) {
            if (night_epochs_[i].night_start_ms == night.start_ms) {
                night_epochs_[i] = night_epochs_[night_epoch_count_ - 1];
                --night_epoch_count_;
                break;
            }
        }
        give_summary_lock();
    }
    return ok;
}

bool ReportManager::cancel_cache_fetch() {
    if (!cache_fetch_active_) return false;
    spool_.reset();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = "cancelled";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_RPC,
              LOG_INFO,
              "[REPORT] Cache fetch cancelled night=%llu source=%s\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              report_source_spool_type(cache_status_.active_source));
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare("cache_cancelled");
    }
    return true;
}

bool ReportManager::ensure_result_chunks() {
    if (result_chunks_) return true;
    result_chunks_ = static_cast<ReportResultChunk *>(
        Memory::calloc_large(AC_REPORT_RESULT_CHUNK_MAX,
                             sizeof(ReportResultChunk)));
    if (!result_chunks_) {
        fail_result_prepare("result_manifest_alloc_failed");
        return false;
    }
    result_chunk_capacity_ = AC_REPORT_RESULT_CHUNK_MAX;
    return true;
}

void ReportManager::clear_result_prepare() {
    reset_plot_build();
    if (result_chunks_ && result_chunk_capacity_) {
        memset(result_chunks_, 0,
               result_chunk_capacity_ * sizeof(ReportResultChunk));
    }
    memset(result_streams_, 0, sizeof(result_streams_));
    result_stream_count_ = 0;
    result_night_ = {};
    result_plot_bin_.clear();
    result_status_ = {};
}

void ReportManager::fail_result_prepare(const char *message) {
    reset_plot_build();
    result_status_.state = ReportResultState::Error;
    result_status_.error = message ? message : "result_prepare_failed";
    Log::logf(CAT_RPC, LOG_WARN, "[REPORT] Result prepare failed: %s\n",
              result_status_.error.c_str());
}

const char *ReportManager::result_state_name() const {
    return result_state_name_for(result_status_.state);
}

bool ReportManager::add_result_stream(ReportStoreChunkKind kind,
                                      ReportSourceId source,
                                      ReportSignalId signal,
                                      const char *name,
                                      bool required,
                                      bool complete,
                                      size_t &stream_index) {
    if (!name || !name[0]) return false;
    for (size_t i = 0; i < result_stream_count_; ++i) {
        ReportResultStream &stream = result_streams_[i];
        if (stream.kind == kind && stream.source == source &&
            stream.name && strcmp(stream.name, name) == 0) {
            stream_index = i;
            if (required) stream.required = true;
            if (!complete && stream.complete) {
                stream.complete = false;
                if (stream.required) result_status_.missing_streams++;
            }
            return true;
        }
    }

    if (result_stream_count_ >= AC_REPORT_RESULT_STREAM_MAX) {
        fail_result_prepare("result_streams_full");
        return false;
    }
    stream_index = result_stream_count_;
    ReportResultStream &stream = result_streams_[result_stream_count_++];
    stream.kind = kind;
    stream.source = source;
    stream.signal = signal;
    stream.name = name;
    stream.required = required;
    stream.complete = complete;
    result_status_.stream_count =
        static_cast<uint32_t>(result_stream_count_);
    if (required && !complete) result_status_.missing_streams++;
    return true;
}

bool ReportManager::collect_result_chunk(void *context,
                                         const ReportStoreChunkInfo &info) {
    ResultChunkContext *ctx = static_cast<ResultChunkContext *>(context);
    if (!ctx || !ctx->manager || !ctx->name || !ctx->name[0]) return false;
    ReportManager *manager = ctx->manager;
    if (!manager->result_chunks_ ||
        manager->result_status_.chunk_count >=
            manager->result_chunk_capacity_) {
        manager->fail_result_prepare("result_chunks_full");
        return false;
    }

    for (uint32_t i = 0; i < manager->result_status_.chunk_count; ++i) {
        const ReportResultChunk &existing = manager->result_chunks_[i];
        if (existing.kind == info.key.kind &&
            existing.source == ctx->source &&
            existing.name && info.key.name &&
            strcmp(existing.name, info.key.name) == 0 &&
            existing.start_ms == info.key.start_ms &&
            existing.end_ms == info.key.end_ms) {
            return true;
        }
    }

    ReportResultChunk &chunk =
        manager->result_chunks_[manager->result_status_.chunk_count++];
    chunk.kind = ctx->kind;
    chunk.source = ctx->source;
    chunk.signal = ctx->signal;
    chunk.name = ctx->name;
    chunk.start_ms = info.key.start_ms;
    chunk.end_ms = info.key.end_ms;
    chunk.payload_schema = info.meta.payload_schema;
    chunk.record_count = info.meta.record_count;
    chunk.payload_len = info.payload_len;

    if (ctx->stream_index < manager->result_stream_count_) {
        ReportResultStream &stream =
            manager->result_streams_[ctx->stream_index];
        stream.chunk_count++;
        stream.record_count += info.meta.record_count;
        stream.payload_bytes += info.payload_len;
    }
    manager->result_status_.record_count += info.meta.record_count;
    manager->result_status_.payload_bytes += info.payload_len;
    return true;
}

namespace {
struct ReportChunkExtentCtx {
    int64_t min_start = 0;
    int64_t max_end = 0;
    bool found = false;
};
bool report_chunk_extent_cb(void *context, const ReportStoreChunkInfo &info) {
    auto *ctx = static_cast<ReportChunkExtentCtx *>(context);
    if (!ctx->found || info.key.start_ms < ctx->min_start) {
        ctx->min_start = info.key.start_ms;
    }
    if (!ctx->found || info.key.end_ms > ctx->max_end) {
        ctx->max_end = info.key.end_ms;
    }
    ctx->found = true;
    return true;  // keep iterating: every chunk is needed to bound the extent
}
}  // namespace

// Earliest start / latest end of a source's cached chunks for a night, within
// the night's session span. Returns false when the source has none cached.
bool ReportManager::source_chunk_extent(const ReportSummaryRecord &night,
                                        ReportSourceId source,
                                        const char *name,
                                        int64_t &min_start,
                                        int64_t &max_end) const {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0] || !name || !name[0]) {
        return false;
    }
    int64_t span_start = 0;
    int64_t span_end = 0;
    if (!night_data_span(night, span_start, span_end)) return false;
    ReportChunkExtentCtx ctx;
    ReportStore::for_each_chunk(ReportStoreChunkKind::Series,
                                def->spool_type,
                                name,
                                static_cast<int64_t>(night.start_ms),
                                span_start,
                                span_end,
                                report_chunk_extent_cb,
                                &ctx);
    if (!ctx.found) return false;
    min_start = ctx.min_start;
    max_end = ctx.max_end;
    return true;
}

bool ReportManager::add_result_chunks_for_range(ReportStoreChunkKind kind,
                                                ReportSourceId source,
                                                ReportSignalId signal,
                                                const char *name,
                                                int64_t night_start_ms,
                                                int64_t start_ms,
                                                int64_t end_ms,
                                                bool required) {
    if (!ensure_result_chunks()) return false;
    const ReportSourceDef *source_def = report_source_def(source);
    if (!source_def || !source_def->spool_type || !source_def->spool_type[0]) {
        fail_result_prepare("bad_result_source");
        return false;
    }
    const bool sparse_events = kind == ReportStoreChunkKind::Events;
    // Completeness is coverage-driven for every source, events included: a swept
    // span is covered full-width (so zero events is real) and an unswept span is
    // not (events unavailable, not zero). sparse_events still guards the aged-out
    // series check below, where a covered event stream may hold zero chunks.
    const bool complete =
        ReportStore::coverage_complete(source_def->spool_type,
                                       start_ms,
                                       end_ms,
                                       source_def->parser_schema);
    size_t stream_index = 0;
    if (!add_result_stream(kind,
                           source,
                           signal,
                           name,
                           required,
                           complete,
                           stream_index)) {
        return false;
    }
    if (!complete) return true;

    ResultChunkContext context;
    context.manager = this;
    context.kind = kind;
    context.source = source;
    context.signal = signal;
    context.name = name;
    context.required = required;
    context.stream_index = stream_index;
    if (!ReportStore::for_each_chunk(kind,
                                     source_def->spool_type,
                                     name,
                                     night_start_ms,
                                     start_ms,
                                     end_ms,
                                     collect_result_chunk,
                                     &context)) {
        if (result_status_.state != ReportResultState::Error) {
            fail_result_prepare("result_chunk_list_failed");
        }
        return false;
    }
    if (!sparse_events &&
        required &&
        result_streams_[stream_index].chunk_count == 0 &&
        result_streams_[stream_index].complete) {
        result_streams_[stream_index].complete = false;
        result_status_.missing_streams++;
    }
    return true;
}

bool ReportManager::result_plot_cache_path_for_night(
    const ReportSummaryRecord &night,
    char *path,
    size_t path_size) const {
    if (!path || !path_size || night.start_ms == 0) return false;
    // record_count is a data-version tag: when a night's chunks change the count moves,
    // so the key changes and the stale plot binary is never loaded
    const int written = snprintf(
        path,
        path_size,
        "%s/%llu-%lu-%lu-%lu.bin",
        REPORT_PLOT_CACHE_DIR,
        static_cast<unsigned long long>(night.start_ms),
        static_cast<unsigned long>(night.duration_min),
        static_cast<unsigned long>(night.session_interval_count),
        static_cast<unsigned long>(result_status_.record_count));
    return written > 0 && static_cast<size_t>(written) < path_size;
}

bool ReportManager::result_plot_cache_path(char *path,
                                           size_t path_size) const {
    return result_plot_cache_path_for_night(result_night_, path, path_size);
}

bool ReportManager::load_result_plot_cache() {
    Storage::Guard g;
    result_plot_bin_.clear();
    if (!Storage::mounted()) return false;
    char path[REPORT_PLOT_PATH_MAX];
    if (!result_plot_cache_path(path, sizeof(path)) ||
        !Storage::exists(path)) {
        return false;
    }
    File file = Storage::open(path, "r");
    if (!file) return false;
    const size_t size = static_cast<size_t>(file.size());
    result_plot_bin_.set_max_size(size);
    if (size < 8 || !result_plot_bin_.reserve_capacity(size)) {
        file.close();
        result_plot_bin_.clear();
        return false;
    }
    uint8_t buffer[512];
    while (file.available()) {
        const int n = file.read(buffer, sizeof(buffer));
        if (n < 0 ||
            !result_plot_bin_.append(buffer, static_cast<size_t>(n))) {
            file.close();
            result_plot_bin_.clear();
            return false;
        }
        if (n == 0) break;
    }
    file.close();
    const uint8_t *d = result_plot_bin_.data();
    const uint32_t magic = d ? (static_cast<uint32_t>(d[0]) |
                                (static_cast<uint32_t>(d[1]) << 8) |
                                (static_cast<uint32_t>(d[2]) << 16) |
                                (static_cast<uint32_t>(d[3]) << 24))
                             : 0u;
    // Reject on a PLOT_BIN_VERSION mismatch too (the cache dir name guards
    // format changes by hand; this guards a version bump that forgot the dir).
    const uint16_t version = d ? (static_cast<uint16_t>(d[4]) |
                                  (static_cast<uint16_t>(d[5]) << 8))
                               : 0u;
    if (result_plot_bin_.size() != size || magic != PLOT_BIN_MAGIC ||
        version != PLOT_BIN_VERSION) {
        result_plot_bin_.clear();
        return false;
    }
    return true;
}

bool ReportManager::save_result_plot_cache() const {
    Storage::Guard g;
    if (!Storage::mounted() || result_plot_bin_.size() == 0) return false;
    if (!Storage::ensure_dir("/aircannect") ||
        !Storage::ensure_dir("/aircannect/report") ||
        !Storage::ensure_dir("/aircannect/report/v2") ||
        !Storage::ensure_dir("/aircannect/report/v2/plots") ||
        !Storage::ensure_dir(REPORT_PLOT_CACHE_DIR)) {
        return false;
    }
    char path[REPORT_PLOT_PATH_MAX];
    if (!result_plot_cache_path(path, sizeof(path))) return false;
    char tmp[REPORT_PLOT_PATH_MAX + 8];
    const int written = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(tmp)) {
        return false;
    }
    Storage::remove(tmp);
    File file = Storage::open(tmp, "w");
    if (!file) return false;
    const uint8_t *data = result_plot_bin_.data();
    const size_t len = result_plot_bin_.size();
    const bool ok = file.write(data, len) == len;
    file.close();
    if (!ok) {
        Storage::remove(tmp);
        return false;
    }
    Storage::remove(path);
    if (!Storage::rename(tmp, path)) {
        Storage::remove(tmp);
        return false;
    }
    return true;
}

void ReportManager::reset_plot_build() {
    plot_build_active_ = false;
    plot_build_phase_ = ReportPlotBuildPhase::Idle;
    plot_build_bin_.clear();
    plot_tmp_.clear();
    plot_bin_ok_ = true;
    memset(plot_ranges_, 0, sizeof(plot_ranges_));
    plot_range_count_ = 0;
    plot_start_ms_ = 0;
    plot_end_ms_ = 0;
    plot_bucket_ms_ = 1;
    plot_chunk_index_ = 0;
    plot_stream_index_ = 0;
    plot_current_bucket_ = -1;
    plot_series_open_ = false;
    plot_seen_events_.clear();
    plot_bucket_.clear();
}

void ReportManager::build_empty_plot_bin(ReportSpoolBuffer &out) const {
    out.clear();
    out.set_max_size(32);
    bin_put_u32(out, PLOT_BIN_MAGIC);
    bin_put_u16(out, PLOT_BIN_VERSION);
    bin_put_u16(out, 0);   // flags
    bin_put_i64(out, 0);   // base_ms
    bin_put_u32(out, 0);   // event count; no series follow
}

bool ReportManager::plot_time_in_ranges(int64_t timestamp_ms) const {
    return plot_range_index(timestamp_ms) >= 0;
}

int ReportManager::plot_range_index(int64_t timestamp_ms) const {
    for (size_t i = 0; i < plot_range_count_; ++i) {
        if (timestamp_ms >= plot_ranges_[i].start_ms &&
            timestamp_ms < plot_ranges_[i].end_ms) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

size_t ReportManager::derive_result_session_ranges(PlotRange *ranges,
                                                   size_t max_ranges) const {
    if (!ranges || !max_ranges || !result_chunks_ ||
        result_status_.chunk_count == 0 ||
        result_night_.end_ms <= result_night_.start_ms) {
        return 0;
    }

    const int64_t SESSION_GAP_MS = AC_REPORT_SESSION_GAP_MS;
    const int64_t night_start = static_cast<int64_t>(result_night_.start_ms);
    const int64_t night_end = static_cast<int64_t>(result_night_.end_ms);
    size_t count = 0;

    for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
        const ReportResultChunk &chunk = result_chunks_[i];
        if (chunk.kind != ReportStoreChunkKind::Series ||
            chunk.signal != ReportSignalId::Flow ||
            chunk.end_ms <= chunk.start_ms) {
            continue;
        }

        const int64_t start_ms = std::max(chunk.start_ms, night_start);
        const int64_t end_ms = std::min(chunk.end_ms, night_end);
        if (end_ms <= start_ms) continue;

        if (count > 0 && start_ms <= ranges[count - 1].end_ms + SESSION_GAP_MS) {
            ranges[count - 1].end_ms =
                std::max(ranges[count - 1].end_ms, end_ms);
            continue;
        }
        if (count >= max_ranges) {
            ranges[count - 1].end_ms =
                std::max(ranges[count - 1].end_ms, end_ms);
            continue;
        }
        ranges[count].start_ms = start_ms;
        ranges[count].end_ms = end_ms;
        count++;
    }
    return count;
}

void ReportManager::apply_result_session_ranges(const PlotRange *ranges,
                                                size_t range_count) {
    if (!ranges || !range_count) return;

    result_night_.session_interval_count = 0;
    result_night_.has_session_count = true;
    result_night_.session_count = static_cast<uint32_t>(range_count);

    uint32_t total_min = 0;
    for (size_t i = 0; i < range_count &&
                       i < AC_REPORT_SUMMARY_SESSION_MAX; ++i) {
        if (ranges[i].end_ms <= ranges[i].start_ms) continue;
        const int64_t duration_ms = ranges[i].end_ms - ranges[i].start_ms;
        uint32_t duration_min =
            static_cast<uint32_t>((duration_ms + 30000LL) / 60000LL);
        if (duration_min == 0) duration_min = 1;

        ReportSummarySession &session =
            result_night_.sessions[result_night_.session_interval_count++];
        session.start_ms = static_cast<uint64_t>(ranges[i].start_ms);
        session.duration_min = duration_min;
        total_min += duration_min;
    }

    if (result_night_.session_interval_count > 0) {
        result_night_.duration_min = total_min;
        result_status_.duration_min = total_min;
    }
}

bool ReportManager::start_result_plot_build() {
    reset_plot_build();
    build_empty_plot_bin(result_plot_bin_);
    if (result_status_.state == ReportResultState::Error ||
        !result_chunks_ || result_status_.chunk_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        result_status_.state = settled_result_state(result_status_.missing_required);
        result_status_.error.clear();
        publish_result_to_slot();
        return true;
    }

    ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX];
    const size_t range_count =
        collect_session_ranges(result_night_,
                               ranges,
                               AC_REPORT_SUMMARY_SESSION_MAX);
    if (range_count == 0) {
        build_empty_plot_bin(result_plot_bin_);
        result_status_.state = settled_result_state(result_status_.missing_required);
        result_status_.error.clear();
        publish_result_to_slot();
        return true;
    }

    plot_range_count_ = range_count;
    plot_start_ms_ = ranges[0].start_ms;
    plot_end_ms_ = ranges[0].end_ms;
    for (size_t i = 0; i < range_count; ++i) {
        plot_ranges_[i].start_ms = ranges[i].start_ms;
        plot_ranges_[i].end_ms = ranges[i].end_ms;
        plot_start_ms_ = std::min(plot_start_ms_, ranges[i].start_ms);
        plot_end_ms_ = std::max(plot_end_ms_, ranges[i].end_ms);
    }
    if (plot_start_ms_ <= 0 || plot_end_ms_ <= plot_start_ms_) {
        build_empty_plot_bin(result_plot_bin_);
        result_status_.state = settled_result_state(result_status_.missing_required);
        result_status_.error.clear();
        publish_result_to_slot();
        return true;
    }
    if (load_result_plot_cache()) {
        result_status_.state = settled_result_state(result_status_.missing_required);
        result_status_.error.clear();
        Log::logf(CAT_RPC,
                  LOG_INFO,
                  "[REPORT] Result plot cache hit index=%lu bytes=%lu\n",
                  static_cast<unsigned long>(result_status_.therapy_index),
                  static_cast<unsigned long>(result_plot_bin_.size()));
        publish_result_to_slot();
        return true;
    }

    const int64_t span_ms = plot_end_ms_ - plot_start_ms_;
    plot_bucket_ms_ = std::max<int64_t>(
        1, span_ms / static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS));
    plot_build_bin_.clear();
    plot_build_bin_.set_max_size(768 * 1024);
    plot_tmp_.clear();
    plot_tmp_.set_max_size(128 * 1024);
    plot_bin_ok_ = true;
    if (!plot_build_bin_.reserve_capacity(AC_REPORT_PLOT_INITIAL_RESERVE)) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_bin_ok_ &= bin_put_u32(plot_build_bin_, PLOT_BIN_MAGIC);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, PLOT_BIN_VERSION);
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_, 0);          // flags
    plot_bin_ok_ &= bin_put_i64(plot_build_bin_, plot_start_ms_);  // base_ms
    if (!plot_bin_ok_) {
        fail_result_prepare("plot_alloc_failed");
        return false;
    }
    plot_build_active_ = true;
    plot_build_phase_ = ReportPlotBuildPhase::Events;
    result_status_.state = ReportResultState::Preparing;
    result_status_.error = "plot_preparing";
    return true;
}

bool ReportManager::process_plot_event_chunk(const ReportResultChunk &chunk) {
    const char *source = report_source_spool_type(chunk.source);
    if (!source || !source[0] || !chunk.name || !chunk.name[0]) {
        fail_result_prepare("plot_event_key_failed");
        return false;
    }
    ReportStoreChunkKey key;
    key.kind = chunk.kind;
    key.source = source;
    key.name = chunk.name;
    key.start_ms = chunk.start_ms;
    key.end_ms = chunk.end_ms;
    key.night_start_ms = static_cast<int64_t>(result_night_.start_ms);
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!ReportStore::read_chunk(key, meta, payload)) {
        fail_result_prepare("plot_event_read_failed");
        return false;
    }
    const size_t count = payload.size() / report_event_record_wire_size();
    for (size_t index = 0; index < count; ++index) {
        ReportEventRecord event;
        if (!report_read_event_record(payload.data(),
                                      payload.size(),
                                      index,
                                      event)) {
            continue;
        }
        if (!plot_time_in_ranges(event.start_ms)) continue;
        if (report_event_seen(plot_seen_events_, event)) continue;
        if (!remember_report_event(plot_seen_events_, event)) {
            fail_result_prepare("plot_event_dedupe_failed");
            return false;
        }
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.start_ms - plot_start_ms_));
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(event.duration_ms));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_, static_cast<int32_t>(event.code));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_,
                                    static_cast<int32_t>(event.flags));
    }
    return true;
}

bool ReportManager::open_plot_series(const ReportResultStream &stream) {
    const size_t name_len = stream.name ? strlen(stream.name) : 0;
    plot_bin_ok_ &= bin_put_u16(plot_build_bin_,
                                static_cast<uint16_t>(name_len));
    if (name_len) {
        plot_bin_ok_ &= plot_build_bin_.append(
            reinterpret_cast<const uint8_t *>(stream.name), name_len);
    }
    plot_tmp_.clear();
    plot_series_open_ = true;
    plot_current_bucket_ = -1;
    plot_bucket_.clear();
    return plot_bin_ok_;
}

void ReportManager::flush_plot_bucket() {
    if (!plot_bucket_.have) return;

    struct PlotPoint {
        int64_t t = 0;
        int32_t value = 0;
    };
    PlotPoint points[4] = {
        {plot_bucket_.start_t, plot_bucket_.start_value},
        {plot_bucket_.min_t, plot_bucket_.min_value},
        {plot_bucket_.max_t, plot_bucket_.max_value},
        {plot_bucket_.end_t, plot_bucket_.end_value},
    };
    std::sort(points,
              points + 4,
              [](const PlotPoint &a, const PlotPoint &b) {
                  return a.t < b.t;
              });
    bool emitted[4] = {};
    for (uint8_t i = 0; i < 4; ++i) {
        if (emitted[i]) continue;
        plot_bin_ok_ &= bin_put_i32(
            plot_tmp_, static_cast<int32_t>(points[i].t - plot_start_ms_));
        plot_bin_ok_ &= bin_put_i32(plot_tmp_, points[i].value);
        for (uint8_t j = i + 1; j < 4; ++j) {
            if (points[j].t == points[i].t) emitted[j] = true;
        }
    }

    plot_bucket_.clear();
}

bool ReportManager::process_plot_series_chunk(const ReportResultChunk &chunk) {
    const char *source = report_source_spool_type(chunk.source);
    if (!source || !source[0] || !chunk.name || !chunk.name[0]) {
        fail_result_prepare("plot_series_key_failed");
        return false;
    }
    ReportStoreChunkKey key;
    key.kind = chunk.kind;
    key.source = source;
    key.name = chunk.name;
    key.start_ms = chunk.start_ms;
    key.end_ms = chunk.end_ms;
    key.night_start_ms = static_cast<int64_t>(result_night_.start_ms);
    ReportStoreChunkMeta meta;
    ReportSpoolBuffer payload;
    if (!ReportStore::read_chunk(key, meta, payload)) {
        fail_result_prepare("plot_series_read_failed");
        return false;
    }
    const size_t sample_count =
        payload.size() / report_series_sample_wire_size();
    for (size_t sample_index = 0; sample_index < sample_count; ++sample_index) {
        ReportSeriesSample sample;
        if (!report_read_series_sample(payload.data(),
                                       payload.size(),
                                       sample_index,
                                       sample)) {
            continue;
        }
        if (!plot_time_in_ranges(sample.timestamp_ms)) continue;
        int64_t sample_bucket =
            (sample.timestamp_ms - plot_start_ms_) / plot_bucket_ms_;
        if (sample_bucket < 0) sample_bucket = 0;
        if (sample_bucket >= static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS)) {
            sample_bucket = static_cast<int64_t>(AC_REPORT_PLOT_BUCKETS) - 1;
        }
        if (plot_current_bucket_ != sample_bucket) {
            flush_plot_bucket();
            plot_current_bucket_ = sample_bucket;
        }
        int32_t value_milli = sample.value_milli;
        if ((chunk.signal == ReportSignalId::Flow &&
             chunk.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
            (chunk.signal == ReportSignalId::Leak &&
             chunk.source == ReportSourceId::Leak0p5Hz)) {
            const int64_t scaled =
                static_cast<int64_t>(value_milli) * 60LL;
            value_milli = scaled > INT32_MAX
                              ? INT32_MAX
                              : (scaled < INT32_MIN
                                     ? INT32_MIN
                                     : static_cast<int32_t>(scaled));
        }
        if (!plot_bucket_.have) {
            plot_bucket_.have = true;
            plot_bucket_.range_index =
                plot_range_index(sample.timestamp_ms);
            plot_bucket_.start_t = sample.timestamp_ms;
            plot_bucket_.end_t = sample.timestamp_ms;
            plot_bucket_.min_t = sample.timestamp_ms;
            plot_bucket_.max_t = sample.timestamp_ms;
            plot_bucket_.start_value = value_milli;
            plot_bucket_.end_value = value_milli;
            plot_bucket_.min_value = value_milli;
            plot_bucket_.max_value = value_milli;
        } else {
            plot_bucket_.end_t = sample.timestamp_ms;
            plot_bucket_.end_value = value_milli;
            if (value_milli < plot_bucket_.min_value) {
                plot_bucket_.min_value = value_milli;
                plot_bucket_.min_t = sample.timestamp_ms;
            }
            if (value_milli > plot_bucket_.max_value) {
                plot_bucket_.max_value = value_milli;
                plot_bucket_.max_t = sample.timestamp_ms;
            }
        }
    }
    return true;
}

bool ReportManager::finish_result_plot_build() {
    if (!plot_bin_ok_ || plot_build_bin_.size() == 0) {
        fail_result_prepare("plot_overflow");
        return false;
    }
    const size_t len = plot_build_bin_.size();
    result_plot_bin_.clear();
    result_plot_bin_.set_max_size(len);
    if (!result_plot_bin_.reserve_capacity(len) ||
        !result_plot_bin_.append(plot_build_bin_.data(), len)) {
        fail_result_prepare("plot_publish_failed");
        return false;
    }
    save_result_plot_cache();
    reset_plot_build();
    result_status_.state = settled_result_state(result_status_.missing_required);
    result_status_.error.clear();
    publish_result_to_slot();
    Log::logf(CAT_RPC,
              LOG_INFO,
              "[REPORT] Result plot ready index=%lu chunks=%lu bytes=%lu\n",
              static_cast<unsigned long>(result_status_.therapy_index),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_plot_bin_.size()));
    return true;
}

bool ReportManager::build_range_plot(int64_t from_ms, int64_t to_ms,
                                     ReportSpoolBuffer &out) {
    if (to_ms <= from_ms) return false;
    out.clear();
    out.set_max_size(AC_REPORT_RANGE_PLOT_MAX_BYTES);
    bool ok = out.reserve_capacity(64 * 1024);
    ok = ok && bin_put_u32(out, PLOT_BIN_MAGIC);
    ok = ok && bin_put_u16(out, PLOT_BIN_VERSION);
    ok = ok && bin_put_u16(out, 0);  // flags
    ok = ok && bin_put_i64(out, from_ms);  // base_ms
    if (!ok) return false;
    const int64_t night = static_cast<int64_t>(result_night_.start_ms);

    // Walk result_chunks_ (the prepared manifest); keep samples in [from,to].
    // scratch is local (freed on return) so the range leaves no persistent buffer.
    ReportSpoolBuffer scratch;
    scratch.set_max_size(768 * 1024);
    ReportSpoolBuffer seen;
    seen.set_max_size(16 * 1024);
    seen.reserve_capacity(2 * 1024);
    uint32_t ev_count = 0;
    for (size_t ci = 0; ci < result_status_.chunk_count && ok; ++ci) {
        const ReportResultChunk &chunk = result_chunks_[ci];
        if (chunk.kind != ReportStoreChunkKind::Events) continue;
        if (chunk.end_ms <= from_ms || chunk.start_ms >= to_ms) continue;
        const char *src = report_source_spool_type(chunk.source);
        if (!src || !chunk.name) continue;
        ReportStoreChunkKey key;
        key.kind = chunk.kind;
        key.source = src;
        key.name = chunk.name;
        key.start_ms = chunk.start_ms;
        key.end_ms = chunk.end_ms;
        key.night_start_ms = night;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
        if (!ReportStore::read_chunk(key, meta, payload)) continue;
        const size_t wire = report_event_record_wire_size();
        const size_t n = wire ? payload.size() / wire : 0;
        for (size_t j = 0; j < n; ++j) {
            ReportEventRecord e;
            if (!report_read_event_record(payload.data(), payload.size(), j,
                                          e)) {
                continue;
            }
            if (e.start_ms < from_ms || e.start_ms >= to_ms) continue;
            if (report_event_seen(seen, e)) continue;
            if (!remember_report_event(seen, e)) { ok = false; break; }
            ok = ok && bin_put_i32(scratch,
                                   static_cast<int32_t>(e.start_ms - from_ms));
            ok = ok && bin_put_i32(scratch,
                                   static_cast<int32_t>(e.duration_ms));
            ok = ok && bin_put_i32(scratch, static_cast<int32_t>(e.code));
            ok = ok && bin_put_i32(scratch, static_cast<int32_t>(e.flags));
            if (!ok) break;
            ++ev_count;
        }
    }
    ok = ok && bin_put_u32(out, ev_count);
    if (ok && scratch.size()) {
        ok = out.append(scratch.data(), scratch.size());
    }
    if (!ok) return false;

    // Series: one stream at a time - name, point count, raw in-range points.
    for (size_t i = 0; i < result_stream_count_; ++i) {
        const ReportResultStream &st = result_streams_[i];
        if (st.kind != ReportStoreChunkKind::Series || !st.name || !st.name[0]) {
            continue;
        }
        const size_t name_len = strlen(st.name);
        ok = ok && bin_put_u16(out, static_cast<uint16_t>(name_len));
        ok = ok && out.append(reinterpret_cast<const uint8_t *>(st.name),
                              name_len);
        if (!ok) return false;
        const int32_t scale =
            (st.source == ReportSourceId::RespiratoryFlow6p25Hz ||
             st.source == ReportSourceId::Leak0p5Hz) ? 60 : 1;
        scratch.clear();
        uint32_t points = 0;
        for (size_t ci = 0;
             ci < result_status_.chunk_count &&
             points < AC_REPORT_RANGE_MAX_POINTS && ok; ++ci) {
            const ReportResultChunk &chunk = result_chunks_[ci];
            if (chunk.kind != ReportStoreChunkKind::Series ||
                chunk.source != st.source ||
                strcmp(chunk.name ? chunk.name : "", st.name) != 0) {
                continue;
            }
            if (chunk.end_ms <= from_ms || chunk.start_ms >= to_ms) continue;
            const char *src = report_source_spool_type(chunk.source);
            if (!src) continue;
            ReportStoreChunkKey key;
            key.kind = chunk.kind;
            key.source = src;
            key.name = chunk.name;
            key.start_ms = chunk.start_ms;
            key.end_ms = chunk.end_ms;
            key.night_start_ms = night;
            ReportStoreChunkMeta meta;
            ReportSpoolBuffer payload;
            if (!ReportStore::read_chunk(key, meta, payload)) continue;
            const size_t wire = report_series_sample_wire_size();
            const size_t n = wire ? payload.size() / wire : 0;
            for (size_t j = 0; j < n && points < AC_REPORT_RANGE_MAX_POINTS;
                 ++j) {
                ReportSeriesSample s;
                if (!report_read_series_sample(payload.data(), payload.size(),
                                               j, s)) {
                    continue;
                }
                if (s.timestamp_ms < from_ms || s.timestamp_ms >= to_ms) {
                    continue;
                }
                int64_t v = static_cast<int64_t>(s.value_milli) * scale;
                if (v > INT32_MAX) v = INT32_MAX;
                else if (v < INT32_MIN) v = INT32_MIN;
                ok = ok && bin_put_i32(
                    scratch, static_cast<int32_t>(s.timestamp_ms - from_ms));
                ok = ok && bin_put_i32(scratch, static_cast<int32_t>(v));
                if (!ok) break;
                ++points;
            }
        }
        ok = ok && bin_put_u32(out, points);
        if (ok && scratch.size()) {
            ok = out.append(scratch.data(), scratch.size());
        }
        if (!ok) return false;
    }
    return ok;
}

// Series-point count from the actual blob bytes (0 if malformed). Gates caching
// and serving so a zero-series/truncated blob is never stored or sent.
static uint32_t range_blob_series_points(const ReportSpoolBuffer &b) {
    const uint8_t *d = reinterpret_cast<const uint8_t *>(b.data());
    const size_t n = b.size();
    if (!d || n < 20) return 0;
    uint32_t ev = 0;
    memcpy(&ev, d + 16, sizeof(ev));
    size_t off = 20 + static_cast<size_t>(ev) * 16;
    if (off > n) return 0;  // events truncated -> treat as no data
    uint32_t total = 0;
    // Bail to 0 on any malformation (e.g. a stream whose points are truncated).
    while (off < n) {
        if (off + 2 > n) return 0;
        uint16_t name_len = 0;
        memcpy(&name_len, d + off, sizeof(name_len));
        off += 2;
        if (off + name_len + 4 > n) return 0;
        off += name_len;
        uint32_t pc = 0;
        memcpy(&pc, d + off, sizeof(pc));
        off += 4;
        if (off + static_cast<size_t>(pc) * 8 > n) return 0;
        off += static_cast<size_t>(pc) * 8;
        total += pc;
    }
    return total;
}

ReportManager::PlotRead ReportManager::read_plot_range(
    size_t therapy_index, int64_t from_ms, int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!result_slots_lock_) return PlotRead::Unavailable;
    if (to_ms <= from_ms) return PlotRead::NotFound;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    if (range_plot_bytes_ && range_plot_index_ == therapy_index &&
        range_plot_from_ == from_ms && range_plot_to_ == to_ms &&
        range_blob_series_points(*range_plot_bytes_) > 0) {
        out = range_plot_bytes_;
        range_plot_bytes_.reset();  // serve once; the response holds it while streaming
        xSemaphoreGive(result_slots_lock_);
        return PlotRead::Ready;
    }
    range_req_active_ = true;
    range_req_index_ = therapy_index;
    range_req_from_ = from_ms;
    range_req_to_ = to_ms;
    xSemaphoreGive(result_slots_lock_);
    return PlotRead::Building;
}

void ReportManager::service_range_plot() {
    if (!result_slots_lock_) return;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    const bool active = range_req_active_;
    const size_t index = range_req_index_;
    const int64_t from_ms = range_req_from_;
    const int64_t to_ms = range_req_to_;
    xSemaphoreGive(result_slots_lock_);
    if (!active) return;
    // Defer while a night build or summary fetch owns the shared state + SD bus.
    if (plot_build_active_ || summary_fetch_active_) return;
    // Yield an idle prefetch so the range builds now; a real foreground fetch is
    // not yielded, so wait for that.
    if (cache_fetch_active_) {
        prefetch_yield_to_foreground();
        if (cache_fetch_active_) return;
    }
    const bool ready = result_status_.state == ReportResultState::Ready ||
                       result_status_.state == ReportResultState::Partial;
    if (!ready || result_status_.therapy_index != index) {
        // Prepare the requested night first, then retry on a later poll.
        ReportSummaryRecord rec;
        if (summary_night_by_therapy_index(index, rec)) {
            enqueue_build(rec.start_ms, index, false);
        } else {
            xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
            if (range_req_active_ && range_req_index_ == index &&
                range_req_from_ == from_ms && range_req_to_ == to_ms) {
                range_req_active_ = false;
            }
            xSemaphoreGive(result_slots_lock_);
        }
        return;
    }
    std::shared_ptr<ReportSpoolBuffer> bytes =
        std::make_shared<ReportSpoolBuffer>();
    const bool ok = bytes && build_range_plot(from_ms, to_ms, *bytes);
    const bool has_points = ok && range_blob_series_points(*bytes) > 0;
    xSemaphoreTake(result_slots_lock_, portMAX_DELAY);
    if (has_points) {
        range_plot_bytes_ = bytes;
        range_plot_index_ = index;
        range_plot_from_ = from_ms;
        range_plot_to_ = to_ms;
    } else if (range_plot_index_ == index && range_plot_from_ == from_ms &&
               range_plot_to_ == to_ms) {
        // Zero-series build (wrong manifest): never cache, and drop any stale
        // empty for this window so a later poll rebuilds.
        range_plot_bytes_.reset();
    }
    if (range_req_active_ && range_req_index_ == index &&
        range_req_from_ == from_ms && range_req_to_ == to_ms) {
        range_req_active_ = false;
    }
    xSemaphoreGive(result_slots_lock_);
}

void ReportManager::poll_result_plot_build() {
    if (!plot_build_active_) return;
    // Bound SD work per poll by BYTES read: after coalescing a chunk is ~512 KB,
    // so a chunk-count budget would pull a whole night in one poll and block the
    // main loop for seconds. The chunk cap is a secondary guard against per-open
    // cost on many tiny (legacy) chunks. The byte check sits at the loop top, so
    // the chunk that crosses the budget still completes 
    constexpr size_t PLOT_BYTE_BUDGET = 256 * 1024;
    constexpr size_t PLOT_CHUNK_CAP = 24;
    size_t bytes = 0;
    size_t reads = 0;
    while (plot_build_active_ && bytes < PLOT_BYTE_BUDGET &&
           reads < PLOT_CHUNK_CAP) {
        if (plot_build_phase_ == ReportPlotBuildPhase::Events) {
            bool processed = false;
            while (plot_chunk_index_ < result_status_.chunk_count) {
                const ReportResultChunk &chunk =
                    result_chunks_[plot_chunk_index_++];
                if (chunk.kind != ReportStoreChunkKind::Events) continue;
                if (!process_plot_event_chunk(chunk)) return;
                processed = true;
                bytes += chunk.payload_len;
                reads++;
                break;
            }
            if (processed) continue;
            // Events phase done: write the event count + accumulated records.
            const uint32_t event_count =
                static_cast<uint32_t>(plot_tmp_.size() / 16);
            plot_bin_ok_ &= bin_put_u32(plot_build_bin_, event_count);
            if (plot_tmp_.size()) {
                plot_bin_ok_ &=
                    plot_build_bin_.append(plot_tmp_.data(), plot_tmp_.size());
            }
            plot_tmp_.clear();
            plot_build_phase_ = ReportPlotBuildPhase::Series;
            plot_chunk_index_ = 0;
            plot_stream_index_ = 0;
            continue;
        }

        if (plot_build_phase_ == ReportPlotBuildPhase::Series) {
            if (plot_stream_index_ >= result_stream_count_) {
                finish_result_plot_build();
                return;
            }

            const ReportResultStream &stream =
                result_streams_[plot_stream_index_];
            if (stream.kind != ReportStoreChunkKind::Series ||
                !stream.name || !stream.name[0] ||
                stream.chunk_count == 0) {
                plot_stream_index_++;
                plot_chunk_index_ = 0;
                continue;
            }
            if (!plot_series_open_ && !open_plot_series(stream)) {
                fail_result_prepare("plot_series_open_failed");
                return;
            }

            bool processed = false;
            while (plot_chunk_index_ < result_status_.chunk_count) {
                const ReportResultChunk &chunk =
                    result_chunks_[plot_chunk_index_++];
                if (chunk.kind != ReportStoreChunkKind::Series ||
                    chunk.source != stream.source ||
                    strcmp(chunk.name ? chunk.name : "", stream.name) != 0) {
                    continue;
                }
                if (!process_plot_series_chunk(chunk)) return;
                processed = true;
                bytes += chunk.payload_len;
                reads++;
                break;
            }
            if (processed) continue;

            flush_plot_bucket();
            // Series done: write its point count + accumulated points.
            const uint32_t point_count =
                static_cast<uint32_t>(plot_tmp_.size() / 8);
            plot_bin_ok_ &= bin_put_u32(plot_build_bin_, point_count);
            if (plot_tmp_.size()) {
                plot_bin_ok_ &=
                    plot_build_bin_.append(plot_tmp_.data(), plot_tmp_.size());
            }
            plot_tmp_.clear();
            plot_series_open_ = false;
            plot_stream_index_++;
            plot_chunk_index_ = 0;
            continue;
        }

        fail_result_prepare("plot_bad_state");
        return;
    }
}

bool ReportManager::prepare_result_by_therapy_index(size_t therapy_index,
                                                    bool refresh_cache) {
    return prepare_result_by_therapy_index_internal(therapy_index,
                                                   refresh_cache);
}

bool ReportManager::prepare_result_by_therapy_index_internal(
    size_t therapy_index,
    bool refresh_cache) {
    if (summary_fetch_active_) {
        pending_result_prepare_ = true;
        pending_result_refresh_cache_ = refresh_cache;
        pending_result_therapy_index_ = therapy_index;
        clear_result_prepare();
        result_status_.state = ReportResultState::Preparing;
        result_status_.therapy_index = therapy_index;
        result_status_.error = "summary_fetching";
        return true;
    }

    if (!ensure_result_chunks()) return false;

    ReportSummaryRecord night;
    if (!summary_night_by_therapy_index(therapy_index, night)) {
        clear_result_prepare();
        fail_result_prepare("night_not_found");
        return false;
    }

    // Idempotent re-prepare: same night already prepared with a plot -> keep it,
    // bump revision. Identity is (therapy_index, start_ms, end_ms); duration_min
    // is deliberately excluded - the plot build overwrites it with the session
    // sum, so comparing it would defeat the fast path and briefly empty
    // result_plot_bin_ (a /api/report/plot 404 window).
    if (!refresh_cache &&
        (result_status_.state == ReportResultState::Ready ||
         result_status_.state == ReportResultState::Partial) &&
        result_status_.therapy_index == therapy_index &&
        result_status_.chunk_count > 0 &&
        result_status_.night_start_ms == night.start_ms &&
        result_status_.night_end_ms == night.end_ms &&
        result_plot_bin_.size() > 0) {
        publish_result_to_slot();
        return true;
    }

    clear_result_prepare();
    result_status_.state = ReportResultState::Preparing;
    result_status_.therapy_index = therapy_index;

    result_night_ = night;
    result_status_.night_start_ms = night.start_ms;
    result_status_.night_end_ms = night.end_ms;
    result_status_.duration_min = night.duration_min;
    if (night.has_ahi) {
        result_status_.ahi = night.ahi;
        result_status_.oa_index = night.has_oa_index ? night.oa_index : 0.0f;
        result_status_.ca_index = night.has_ca_index ? night.ca_index : 0.0f;
        result_status_.ua_index = night.has_ua_index ? night.ua_index : 0.0f;
        result_status_.hypopnea_index =
            night.has_hypopnea_index ? night.hypopnea_index : 0.0f;
        result_status_.arousal_index =
            night.has_rera_index ? night.rera_index : 0.0f;
        result_status_.event_metrics_valid = true;
    }

    ReportNightCoverageStatus coverage;
    if (!night_coverage(night.start_ms, coverage)) {
        fail_result_prepare("coverage_unavailable");
        return false;
    }
    result_status_.missing_required = coverage.missing_required;
    // Event counts come from cached event chunks; if the event source is not
    // covered for this night, zero counts are unknown (not real) -> flag
    // unavailable so the UI shows that rather than reporting zero events.
    const ReportSourceDef *events_def =
        report_source_def(ReportSourceId::RespiratoryEvents);
    result_status_.events_available =
        events_def && source_complete_for_night(night, *events_def);

    // Use the session data span, not night.end_ms (a 24h day bucket far past the
    // therapy data) coverage is only written/checked over the session span,
    // so the result chunk range and its coverage check must match.
    ReportSessionRange night_range;
    if (!night_data_span(night, night_range.start_ms, night_range.end_ms)) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "no_sessions";
        return true;
    }

    const bool latest_tail_refresh = refresh_cache && therapy_index == 0;
    const bool cache_refresh_requested = refresh_cache;
    // Reports are read-only: opening a night never triggers a spool (that would
    // duel the background sweep on the single-consumer CAN bus). Only an
    // explicit refresh fetches; otherwise we build from whatever is cached.
    const bool cache_needed = cache_refresh_requested;
    if (cache_needed) {
        // A user-initiated prepare preempts a low-priority background prefetch
        // so it claims the fetch slot immediately instead of queuing behind it.
        prefetch_yield_to_foreground();
        if (!cache_fetch_active_) {
            if (!build_cache_plan(night,
                                  cache_refresh_requested,
                                  latest_tail_refresh)) {
                return false;
            }
            if (cache_source_count_ > 0) {
                if (!start_next_cache_source()) {
                    return false;
                }
            } else {
                cache_fetch_active_ = false;
                cache_status_.active = false;
                cache_status_.revision++;
                cache_status_.error.clear();
            }
        }

        if (coverage.missing_required) {
            pending_result_prepare_ = true;
            pending_result_refresh_cache_ = false;
            pending_result_therapy_index_ = therapy_index;
            result_status_.state = ReportResultState::Preparing;
            result_status_.error = "cache_fetching";
            return true;
        }
    }

    if (!add_result_chunks_for_range(
            ReportStoreChunkKind::Events,
            ReportSourceId::RespiratoryEvents,
            ReportSignalId::Flow,
            report_source_spool_type(ReportSourceId::RespiratoryEvents),
            static_cast<int64_t>(night.start_ms),
            night_range.start_ms,
            night_range.end_ms,
            true)) {
        return false;
    }

    size_t signal_count = 0;
    const ReportSignalDef *signals = report_signal_defs(signal_count);
    for (size_t signal_index = 0; signal_index < signal_count; ++signal_index) {
        const ReportSignalDef &signal = signals[signal_index];
        // Prefer the high-res source only when its cached data covers the night
        // about as fully as the 1-minute fallback. On a retention-boundary night
        // the device kept only a recent TAIL of high-res; preferring it would
        // drop the early hours the 1-minute trend still has. Older nights age out
        // high-res entirely (no chunks), and that also falls back here.
        ReportSourceId source = signal.preferred_source;
        if (source != signal.fallback_source) {
            constexpr int64_t kCoverTolMs = 5 * 60 * 1000;  // absorb rate offsets
            int64_t p_min = 0, p_max = 0, f_min = 0, f_max = 0;
            const bool p_has = source_chunk_extent(
                night, source, signal.store_name, p_min, p_max);
            const bool f_has = source_chunk_extent(
                night, signal.fallback_source, signal.store_name, f_min, f_max);
            if (!p_has || (f_has && (p_min > f_min + kCoverTolMs ||
                                     p_max < f_max - kCoverTolMs))) {
                source = signal.fallback_source;
            }
        }
        if (!add_result_chunks_for_range(ReportStoreChunkKind::Series,
                                         source,
                                         signal.id,
                                         signal.store_name,
                                         static_cast<int64_t>(night.start_ms),
                                         night_range.start_ms,
                                         night_range.end_ms,
                                         true)) {
            return false;
        }
    }

    if (result_status_.state == ReportResultState::Error) return false;
    // Build a best-effort plot from whatever is cached: aged-out signals leave
    // missing_streams>0 and not-yet-swept sources leave missing_required>0, but
    // both are reported for the UI to mark - they do not block rendering. Only
    // a night with nothing cached at all (background hasn't reached it) has no
    // plot to show.
    if (result_status_.chunk_count == 0) {
        result_status_.state = ReportResultState::Incomplete;
        result_status_.error = "not_cached";
    } else {
        std::sort(result_chunks_,
                  result_chunks_ + result_status_.chunk_count,
                  [](const ReportResultChunk &a,
                     const ReportResultChunk &b) {
                      if (a.kind != b.kind) return a.kind < b.kind;
                      if (a.source != b.source) return a.source < b.source;
                      const int name_cmp = strcmp(a.name ? a.name : "",
                                                  b.name ? b.name : "");
                      if (name_cmp != 0) return name_cmp < 0;
                      return a.start_ms < b.start_ms;
                  });
        PlotRange derived_ranges[AC_REPORT_SUMMARY_SESSION_MAX];
        const size_t derived_range_count =
            derive_result_session_ranges(derived_ranges,
                                         AC_REPORT_SUMMARY_SESSION_MAX);
        if (derived_range_count > 0) {
            apply_result_session_ranges(derived_ranges, derived_range_count);
        }

        ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX];
        const size_t range_count =
            collect_session_ranges(result_night_,
                                   ranges,
                                   AC_REPORT_SUMMARY_SESSION_MAX);
        result_status_.oa_count = 0;
        result_status_.ca_count = 0;
        result_status_.ua_count = 0;
        result_status_.hypopnea_count = 0;
        result_status_.arousal_count = 0;
        ReportSpoolBuffer counted_events;
        counted_events.set_max_size(64 * 1024);
        for (uint32_t i = 0; i < result_status_.chunk_count; ++i) {
            const ReportResultChunk &chunk = result_chunks_[i];
            if (chunk.kind != ReportStoreChunkKind::Events) continue;
            const char *source = report_source_spool_type(chunk.source);
            if (!source || !source[0] || !chunk.name || !chunk.name[0]) {
                fail_result_prepare("event_chunk_key_failed");
                return false;
            }
            ReportStoreChunkKey key;
            key.kind = chunk.kind;
            key.source = source;
            key.name = chunk.name;
            key.start_ms = chunk.start_ms;
            key.end_ms = chunk.end_ms;
            key.night_start_ms = static_cast<int64_t>(result_night_.start_ms);
            ReportStoreChunkMeta meta;
            ReportSpoolBuffer payload;
            if (!ReportStore::read_chunk(key, meta, payload)) {
                fail_result_prepare("event_chunk_read_failed");
                return false;
            }
            const size_t count =
                payload.size() / report_event_record_wire_size();
            for (size_t index = 0; index < count; ++index) {
                ReportEventRecord event;
                if (!report_read_event_record(payload.data(),
                                              payload.size(),
                                              index,
                                              event)) {
                    continue;
                }
                if (!report_time_in_ranges(event.start_ms,
                                           ranges,
                                           range_count)) {
                    continue;
                }
                if (event.duration_ms < 0) continue;
                if (report_event_seen(counted_events, event)) continue;
                if (!remember_report_event(counted_events, event)) {
                    fail_result_prepare("event_dedupe_failed");
                    return false;
                }
                switch (event.code) {
                    case 2:
                        result_status_.hypopnea_count++;
                        break;
                    case 3:
                        result_status_.ca_count++;
                        break;
                    case 4:
                        result_status_.oa_count++;
                        break;
                    case 5:
                        result_status_.ua_count++;
                        break;
                    case 6:
                        result_status_.arousal_count++;
                        break;
                    default:
                        break;
                }
            }
        }
        result_status_.event_metrics_valid = false;
        if (result_status_.duration_min > 0) {
            const float hours =
                static_cast<float>(result_status_.duration_min) / 60.0f;
            if (hours > 0.0f) {
                result_status_.oa_index =
                    static_cast<float>(result_status_.oa_count) / hours;
                result_status_.ca_index =
                    static_cast<float>(result_status_.ca_count) / hours;
                result_status_.ua_index =
                    static_cast<float>(result_status_.ua_count) / hours;
                result_status_.hypopnea_index =
                    static_cast<float>(result_status_.hypopnea_count) / hours;
                result_status_.arousal_index =
                    static_cast<float>(result_status_.arousal_count) / hours;
                result_status_.ahi =
                    result_status_.oa_index +
                    result_status_.ca_index +
                    result_status_.ua_index +
                    result_status_.hypopnea_index;
                // Trust chunk-derived indices only when events are covered;
                // otherwise the counts are zero-by-absence, so omit the AHI.
                result_status_.event_metrics_valid =
                    result_status_.events_available;
            }
        }
        if (!start_result_plot_build()) return false;
        if (plot_build_active_) {
            Log::logf(CAT_RPC,
                      LOG_INFO,
                      "[REPORT] Result prepared index=%lu state=%s chunks=%lu "
                      "records=%lu bytes=%lu plot=building\n",
                      static_cast<unsigned long>(therapy_index),
                      result_state_name(),
                      static_cast<unsigned long>(result_status_.chunk_count),
                      static_cast<unsigned long>(result_status_.record_count),
                      static_cast<unsigned long>(result_status_.payload_bytes));
            return true;
        }
    }
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] Result prepared index=%lu state=%s chunks=%lu "
              "records=%lu bytes=%lu\n",
              static_cast<unsigned long>(therapy_index),
              result_state_name(),
              static_cast<unsigned long>(result_status_.chunk_count),
              static_cast<unsigned long>(result_status_.record_count),
              static_cast<unsigned long>(result_status_.payload_bytes));
    return true;
}

bool ReportManager::cache_source_supported(ReportSourceId source) const {
    switch (source) {
        case ReportSourceId::RespiratoryEvents:
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            return true;
        default:
            return false;
    }
}

bool ReportManager::build_cache_plan(const ReportSummaryRecord &night,
                                     bool force,
                                     bool latest_tail_refresh) {
    cache_night_ = night;
    cache_source_count_ = 0;
    cache_source_index_ = 0;
    cache_status_ = {};
    cache_status_.night_start_ms = night.start_ms;
    cache_status_.night_end_ms = night.end_ms;
    cache_status_.active = true;

    int64_t latest_tail_start_ms = static_cast<int64_t>(night.start_ms);
    if (latest_tail_refresh) {
        ReportSessionRange ranges[AC_REPORT_SUMMARY_SESSION_MAX];
        const size_t range_count =
            collect_session_ranges(night,
                                   ranges,
                                   AC_REPORT_SUMMARY_SESSION_MAX);
        latest_tail_start_ms = range_count
            ? ranges[range_count - 1].start_ms
            : static_cast<int64_t>(night.start_ms);
    }

    size_t source_count = 0;
    const ReportSourceDef *sources = report_source_defs(source_count);
    for (size_t i = 0; i < source_count; ++i) {
        const ReportSourceDef &source = sources[i];
        if (source.id == ReportSourceId::Summary) continue;
        if (!cache_source_supported(source.id)) continue;
        if (!force && !source_required_for_report_result(source.id)) continue;
        int64_t from_ms = 0;
        if (latest_tail_refresh) {
            from_ms = latest_tail_start_ms;
            int64_t cached_end_ms = 0;
            if (source_latest_cached_end_for_night(source,
                                                   night,
                                                   cached_end_ms) &&
                cached_end_ms > latest_tail_start_ms) {
                from_ms = cached_end_ms - AC_REPORT_LATEST_TAIL_OVERLAP_MS;
                if (from_ms < latest_tail_start_ms) {
                    from_ms = latest_tail_start_ms;
                }
            }
        } else if (force) {
            from_ms = static_cast<int64_t>(night.start_ms);
        } else {
            if (!source_missing_start_for_night(night, source, from_ms)) {
                continue;
            }
        }
        if (cache_source_count_ >= AC_REPORT_CACHE_SOURCE_MAX) {
            fail_cache_fetch("cache_plan_full");
            return false;
        }
        ReportCacheSourcePlan &plan = cache_plan_[cache_source_count_++];
        plan.source = source.id;
        plan.from_ms = from_ms;
    }
    cache_status_.source_count = static_cast<uint32_t>(cache_source_count_);
    discard_cache_coalesce_buffers();
    cache_fetch_active_ = true;
    cache_status_.active = true;
    return true;
}

bool ReportManager::start_next_cache_source() {
    if (!cache_fetch_active_) return false;
    if (cache_source_index_ >= cache_source_count_) {
        finish_cache_fetch();
        return true;
    }

    const ReportCacheSourcePlan &plan = cache_plan_[cache_source_index_];
    const ReportSourceId source = plan.source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    std::string from_dt;
    if (!format_utc_ms_iso(static_cast<uint64_t>(plan.from_ms), from_dt)) {
        fail_cache_fetch("bad_cache_from_time");
        return false;
    }

    SpoolClientRequest request;
    request.spool_type = def->spool_type;
    request.from_dt = from_dt;
    request.max_size = 65536;
    request.fragment_max = 2808;
    request.max_rounds = 128;
    request.stream_rounds = true;
    if (!spool_.begin(request)) {
        fail_cache_fetch("cache_spool_start_failed");
        return false;
    }

    reset_cache_source_coverage_marks();
    cache_status_.active_source = source;
    cache_status_.source_index = static_cast<uint32_t>(cache_source_index_);
    cache_status_.spool = spool_.status();
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] Cache source queued source=%s from=%s night=%llu\n",
              def->spool_type,
              from_dt.c_str(),
              static_cast<unsigned long long>(cache_night_.start_ms));
    return true;
}

bool ReportManager::store_cache_round(ReportSpoolResult &result) {
    const ReportSourceId source = cache_plan_[cache_source_index_].source;
    const ReportSourceDef *def = report_source_def(source);
    if (!def) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    ChunkWriteContext context;
    context.manager = this;
    context.source = source;
    char error[64] = {};
    bool parsed = false;
    switch (source) {
        case ReportSourceId::UsageEvents:
        case ReportSourceId::RespiratoryEvents:
            parsed = report_parse_event_spool(result,
                                              source,
                                              write_parsed_chunk,
                                              &context,
                                              error,
                                              sizeof(error));
            break;
        case ReportSourceId::TherapyOneMinute:
        case ReportSourceId::RespiratoryFlow6p25Hz:
        case ReportSourceId::MaskPressure6p25Hz:
        case ReportSourceId::InspiratoryPressure0p5Hz:
        case ReportSourceId::Leak0p5Hz:
            parsed = report_parse_series_spool(result,
                                               source,
                                               write_parsed_chunk,
                                               &context,
                                               error,
                                               sizeof(error));
            break;
        default:
            snprintf(error, sizeof(error), "%s", "unsupported_cache_source");
            parsed = false;
            break;
    }
    if (!parsed) {
        // An empty source spool is not a fetch failure: a session can hold zero
        // events, or a sampled source can have aged out
        if (strcmp(error, "spool_empty") == 0) return true;
        fail_cache_fetch(error[0] ? error : "cache_parse_failed");
        return false;
    }

    cache_status_.chunks_written += context.chunks;
    return true;
}

void ReportManager::reset_cache_source_coverage_marks() {
    memset(cache_source_night_extent_ms_,
           0,
           sizeof(cache_source_night_extent_ms_));
}

void ReportManager::note_cache_chunk_coverage(const ReportParsedChunk &chunk) {
    // Track every night the chunk touches, series AND events, recording how far
    // (max end_ms) real data reached so write_cache_source_coverage can bound
    // its coverage claims to the actually-delivered extent.
    if (chunk.start_ms < 0 || chunk.end_ms <= chunk.start_ms) {
        return;
    }

    if (!take_summary_lock(portMAX_DELAY)) return;
    for (size_t record_index = 0;
         records_ &&
         record_index < record_count_ &&
         record_index < AC_REPORT_SUMMARY_RECORD_MAX;
         ++record_index) {
        const ReportSummaryRecord &record = records_[record_index];
        if (!record.valid || !record.duration_min) continue;

        if (report_ranges_overlap(chunk.start_ms,
                                  chunk.end_ms,
                                  static_cast<int64_t>(record.start_ms),
                                  static_cast<int64_t>(record.end_ms))) {
            if (chunk.end_ms > cache_source_night_extent_ms_[record_index]) {
                cache_source_night_extent_ms_[record_index] = chunk.end_ms;
            }
        }
    }
    give_summary_lock();
}

bool ReportManager::write_cache_source_coverage(ReportSourceId source,
                                                int64_t from_ms) {
    const ReportSourceDef *def = report_source_def(source);
    if (!def || !def->spool_type || !def->spool_type[0]) {
        fail_cache_fetch("bad_cache_source");
        return false;
    }

    // Build coverage for every night this sweep delivered, then persist them all
    // in ONE load+coalesce+rewrite (write_coverage_batch). Writing per night
    // re-read+rewrote the whole coverage file O(nights) times on the spool path,
    // starving the CAN RX (dropped frames -> framing CRC). This runs only after
    // a source's spool completed the [from_ms, now] sweep. Per night:
    // - start: a tail refresh (from_ms past the session start) claims only from
    //   where it fetched, never back-claiming the earlier span; a full sweep
    //   (from_ms <= span_start) claims from the span start.
    // - end: the full session span. A sampled source that delivered nothing is
    //   skipped on a partial sweep but settled covered on a full sweep (the
    //   device no longer retains it -- aged out -- so it stops re-fetching).
    //   Events are sparse, so a covered span can legitimately hold zero events.
    const bool sampled = source_is_sampled(*def);
    static ReportStoreCoverageRecord *cov_batch = nullptr;
    if (!cov_batch) {
        cov_batch = static_cast<ReportStoreCoverageRecord *>(Memory::calloc_large(
            AC_REPORT_SUMMARY_RECORD_MAX, sizeof(ReportStoreCoverageRecord)));
    }
    if (!cov_batch) {
        fail_cache_fetch("coverage_alloc_failed");
        return false;
    }

    ReportSummaryRecord *summary_batch = nullptr;
    if (!take_summary_scratch(portMAX_DELAY, summary_batch)) {
        fail_cache_fetch("coverage_summary_alloc_failed");
        return false;
    }

    size_t summary_count = 0;
    if (!take_summary_lock(portMAX_DELAY)) {
        give_summary_scratch();
        fail_cache_fetch("coverage_summary_busy");
        return false;
    }
    for (size_t i = 0; records_ && i < record_count_ &&
                       i < AC_REPORT_SUMMARY_RECORD_MAX; ++i) {
        summary_batch[summary_count++] = records_[i];
    }
    give_summary_lock();

    size_t batch_count = 0;
    for (size_t i = 0; i < summary_count; ++i) {
        const ReportSummaryRecord &record = summary_batch[i];
        if (!record.valid || !record.duration_min) continue;
        int64_t span_start = 0;
        int64_t span_end = 0;
        if (!night_data_span(record, span_start, span_end)) continue;
        if (span_end <= from_ms) continue;
        const int64_t extent = cache_source_night_extent_ms_[i];
        if (sampled && extent <= span_start && from_ms > span_start) continue;
        if (batch_count >= AC_REPORT_SUMMARY_RECORD_MAX) break;

        ReportStoreCoverageRecord &coverage = cov_batch[batch_count];
        coverage = {};
        coverage.start_ms = from_ms > span_start ? from_ms : span_start;
        coverage.end_ms = span_end;
        coverage.parser_schema = def->parser_schema;
        coverage.state = ReportStoreCoverageState::Complete;
        coverage.origin = ReportStoreChunkOrigin::Spool;
        ++batch_count;
    }
    if (batch_count == 0) {
        give_summary_scratch();
        fail_cache_fetch("coverage_empty");
        return false;
    }
    give_summary_scratch();
    if (!ReportStore::write_coverage_batch(def->spool_type, cov_batch,
                                           batch_count)) {
        fail_cache_fetch("coverage_write_failed");
        return false;
    }
    if (!source_complete_for_night(cache_night_, *def)) {
        fail_cache_fetch("coverage_incomplete");
        return false;
    }
    return true;
}

void ReportManager::poll_cache_fetch(RpcArbiter &arbiter) {
    if (!cache_fetch_active_) return;

    spool_.poll(arbiter);
    cache_status_.spool = spool_.status();

    ReportSpoolResult round;
    while (spool_.take_completed_round(round)) {
        if (!store_cache_round(round)) return;
        round.clear();
        cache_status_.spool = spool_.status();
    }

    if (spool_.complete()) {
        ReportSpoolResult final_result;
        spool_.move_result_to(final_result);
        if (final_result.truncated) {
            fail_cache_fetch("source_truncated");
            return;
        }
        // Flush before claiming coverage, so coverage never marks unpersisted data.
        if (!flush_all_cache_coalesce_buffers()) {
            fail_cache_fetch("cache_flush_failed");
            return;
        }
        const ReportCacheSourcePlan completed_plan =
            cache_plan_[cache_source_index_];
        if (!write_cache_source_coverage(
                completed_plan.source,
                completed_plan.from_ms)) {
            return;
        }
        cache_source_index_++;
        start_next_cache_source();
    } else if (spool_.failed()) {
        fail_cache_fetch(spool_.status().error.c_str());
    }
}

void ReportManager::finish_cache_fetch() {
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error.clear();
    cache_status_.spool = spool_.status();
    Log::logf(CAT_RPC, LOG_INFO,
              "[REPORT] Cache fetch complete night=%llu chunks=%lu\n",
              static_cast<unsigned long long>(cache_status_.night_start_ms),
              static_cast<unsigned long>(cache_status_.chunks_written));
    if (pending_result_prepare_) {
        const size_t therapy_index = pending_result_therapy_index_;
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        prepare_result_by_therapy_index_internal(therapy_index, false);
    }
}

void ReportManager::fail_cache_fetch(const char *message) {
    discard_cache_coalesce_buffers();
    cache_fetch_active_ = false;
    cache_status_.active = false;
    cache_status_.revision++;
    cache_status_.error = message ? message : "cache_fetch_failed";
    cache_status_.spool = spool_.status();
    Log::logf(CAT_RPC, LOG_WARN, "[REPORT] Cache fetch failed: %s\n",
              cache_status_.error.c_str());
    if (pending_result_prepare_) {
        pending_result_prepare_ = false;
        pending_result_refresh_cache_ = false;
        fail_result_prepare(cache_status_.error.c_str());
    }
}

}  // namespace aircannect
