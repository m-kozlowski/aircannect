#pragma once

#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <new>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "large_text_buffer.h"
#include "report_parser.h"
#include "report_proto.h"
#include "report_spool_types.h"
#include "rpc_arbiter.h"
#include "spool_client.h"

namespace aircannect {

static constexpr size_t AC_REPORT_SUMMARY_RECORD_MAX = 256;
static constexpr size_t AC_REPORT_NIGHT_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_CACHE_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_RESULT_CHUNK_MAX = 512;
static constexpr size_t AC_REPORT_RESULT_STREAM_MAX = 16;
static constexpr size_t AC_REPORT_RESULT_SLOT_MAX = 4;
static constexpr size_t AC_REPORT_BUILD_QUEUE_MAX = 4;

enum class ReportSummaryState : uint8_t {
    Idle,
    Fetching,
    Ready,
    Error,
};

struct ReportSummaryStatus {
    ReportSummaryState state = ReportSummaryState::Idle;
    uint32_t revision = 0;
    uint32_t records_total = 0;
    uint32_t nights_with_therapy = 0;
    uint32_t elapsed_ms = 0;
    std::string active_spool;
    std::string error;
    SpoolClientStatus spool;
};

struct ReportSummaryNight {
    size_t summary_index = 0;
    size_t therapy_index = 0;
    ReportSummaryRecord record;
};

using ReportSummaryNightCallback =
    bool (*)(void *context, const ReportSummaryNight &night);

struct ReportNightSourceCoverage {
    ReportSourceId source = ReportSourceId::Summary;
    bool required = false;
    bool complete = false;
};

struct ReportNightCoverageStatus {
    bool found = false;
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    uint32_t duration_min = 0;
    uint32_t missing_required = 0;
    size_t source_count = 0;
    ReportNightSourceCoverage sources[AC_REPORT_NIGHT_SOURCE_MAX];
};

struct ReportCacheFetchStatus {
    bool active = false;
    uint32_t revision = 0;
    uint64_t night_start_ms = 0;
    uint64_t night_end_ms = 0;
    uint32_t source_index = 0;
    uint32_t source_count = 0;
    uint32_t chunks_written = 0;
    ReportSourceId active_source = ReportSourceId::Summary;
    std::string error;
    SpoolClientStatus spool;
};

struct ReportCacheSourcePlan {
    ReportSourceId source = ReportSourceId::Summary;
    int64_t from_ms = 0;
};

struct ReportCacheClearResult {
    uint32_t store_reset = 0;
    uint32_t summary_deleted = 0;
    uint32_t nights_cleared = 0;
    uint32_t chunks_deleted = 0;
    uint32_t coverage_deleted = 0;
    uint32_t plots_deleted = 0;
};

enum class ReportResultState : uint8_t {
    Idle,
    Preparing,
    Ready,
    Incomplete,
    Partial,  // displayable best-effort; required coverage is incomplete
    Error,
};

enum class ReportPlotBuildPhase : uint8_t {
    Idle,
    Events,
    Series,
};

struct ReportResultStatus {
    ReportResultState state = ReportResultState::Idle;
    size_t therapy_index = 0;
    uint64_t night_start_ms = 0;
    uint64_t night_end_ms = 0;
    uint32_t duration_min = 0;
    uint32_t missing_required = 0;
    uint32_t missing_streams = 0;
    uint32_t stream_count = 0;
    uint32_t chunk_count = 0;
    uint32_t record_count = 0;
    uint32_t payload_bytes = 0;
    uint32_t materialized_slots = 0;
    uint32_t materialized_plot_slots = 0;
    bool event_metrics_valid = false;
    bool events_available = false;  // event source covered: counts are real, not unknown
    float ahi = 0.0f;
    float oa_index = 0.0f;
    float ca_index = 0.0f;
    float ua_index = 0.0f;
    float hypopnea_index = 0.0f;
    float arousal_index = 0.0f;
    uint32_t oa_count = 0;
    uint32_t ca_count = 0;
    uint32_t ua_count = 0;
    uint32_t hypopnea_count = 0;
    uint32_t arousal_count = 0;
    std::string error;
};

class ReportManager {
public:
    ~ReportManager();

    void begin();
    void poll(RpcArbiter &arbiter);
    bool handle_event(const RpcEvent &event);

    bool request_summary_refresh(bool force = false);
    void build_summary_json(LargeTextBuffer &json) const;
    bool for_each_summary_night(ReportSummaryNightCallback callback,
                                void *context) const;
    bool summary_night_by_therapy_index(size_t therapy_index,
                                        ReportSummaryRecord &out) const;
    bool latest_summary_night(ReportSummaryRecord &out) const;
    ReportSummaryStatus summary_status() const;

    // Atomic mirror of summary_status_.revision, published each poll() so the
    // background prefetch job can watch for new nights without a cross-task read.
    uint32_t summary_revision() const { return summary_revision_pub_.load(); }

    bool night_coverage(uint64_t night_start_ms,
                        ReportNightCoverageStatus &out) const;
    bool night_coverage_by_therapy_index(
        size_t therapy_index,
        ReportNightCoverageStatus &out) const;
    bool latest_night_coverage(ReportNightCoverageStatus &out) const;

    bool request_night_cache(uint64_t night_start_ms, bool force = false);
    bool request_night_cache_by_therapy_index(size_t therapy_index,
                                              bool force = false);
    bool request_latest_night_cache(bool force = false);
    bool cancel_cache_fetch();

    bool clear_cache_all(ReportCacheClearResult &out);
    bool clear_cache_night(uint64_t night_start_ms, ReportCacheClearResult &out);
    bool clear_oldest_cache_nights(size_t max_nights,
                                   ReportCacheClearResult &out);
    bool prune_cache_to_latest_nights(size_t keep_latest,
                                      ReportCacheClearResult &out);

    bool busy() const {
        return summary_fetch_active_ || cache_fetch_active_ ||
               plot_build_active_;
    }
    bool background_work_active() const;

    bool next_night_needing_cache(uint64_t &night_start_ms_out) const;
    const ReportCacheFetchStatus &cache_fetch_status() const {
        return cache_status_;
    }

    enum class PrefetchPhase : uint8_t {
        Idle,      // nothing requested
        Selecting, // worker asked; main loop chooses the next night
        Pending,   // worker asked; main loop starts it when not busy
        Fetching,  // main loop is spooling a night
        Done,      // last fetch fully covered its night
        Failed,    // last fetch ended incomplete / was preempted
        Drained,   // nothing left to prefetch
    };
    struct PrefetchSnapshot {
        PrefetchPhase phase = PrefetchPhase::Idle;
        uint64_t night_ms = 0;
        uint64_t last_night_ms = 0;
        uint64_t last_failed_night_ms = 0;
        uint32_t completed = 0;
        uint32_t failed = 0;
        char last_source[48] = {};
        char last_error[48] = {};
    };

    bool prefetch_request_candidate();
    bool prefetch_request_night(uint64_t night_start_ms);
    void prefetch_mark_drained();
    void prefetch_preempt();
    PrefetchSnapshot prefetch_snapshot() const;
    bool foreground_busy() const;
    bool service_cache_writer();

    bool prepare_result_by_therapy_index(size_t therapy_index,
                                         bool refresh_cache = false);
    void build_result_chunks_json(LargeTextBuffer &json,
                                  size_t offset,
                                  size_t limit) const;

    // Stateless per-night result read: serves a materialized LRU entry by night
    // index with an ETag, without touching the single build-scratch slot.
    enum class ResultRead : uint8_t {
        NotFound,
        NotModified,
        Ready,
        Building,
        QueueFull,
        Unavailable,
        Busy,
    };

    ResultRead read_result(size_t therapy_index, const char *if_none_match,
                           char *etag_out, size_t etag_out_size,
                           LargeTextBuffer &json_out);

    // Stateless per-night plot read: serves the PSRAM blob for (night, version);
    // a miss queues a build and returns Building.
    enum class PlotRead : uint8_t {
        NotFound,
        Ready,
        Building,
        QueueFull,
        Unavailable,
        Busy,
    };

    PlotRead read_plot(size_t therapy_index, const char *version,
                       std::shared_ptr<ReportSpoolBuffer> &out);
    PlotRead read_plot_range(size_t therapy_index, int64_t from_ms,
                             int64_t to_ms,
                             std::shared_ptr<ReportSpoolBuffer> &out);

    const ReportSpoolBuffer &result_plot_bin() const {
        return result_plot_bin_;
    }
    const ReportResultStatus &result_status() const {
        return result_status_;
    }

    // Per-night content version "<start>-<dur>-<sessions>-<epoch>" for the HTTP
    // ETag; computed from the in-memory summary record + the per-night epoch (no SD).
    bool night_etag(size_t therapy_index, char *out, size_t out_size) const;

private:
    struct PlotRange {
        int64_t start_ms = 0;
        int64_t end_ms = 0;
    };

    struct ReportResultChunk {
        ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
        ReportSourceId source = ReportSourceId::Summary;
        ReportSignalId signal = ReportSignalId::Flow;
        const char *name = nullptr;
        int64_t start_ms = 0;
        int64_t end_ms = 0;
        uint32_t payload_schema = 0;
        uint32_t record_count = 0;
        uint32_t payload_len = 0;
    };

    struct ReportResultStream {
        ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
        ReportSourceId source = ReportSourceId::Summary;
        ReportSignalId signal = ReportSignalId::Flow;
        const char *name = nullptr;
        bool required = false;
        bool complete = false;
        uint32_t chunk_count = 0;
        uint32_t record_count = 0;
        uint32_t payload_bytes = 0;
    };

    struct PrefetchSkip {
        uint64_t night_ms = 0;
        uint32_t until_ms = 0;
    };

    struct CacheCoalesceBuffer {
        bool active = false;
        ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
        ReportSourceId source = ReportSourceId::Summary;
        const char *name = nullptr;
        int64_t night_start_ms = 0;
        int64_t first_ms = 0;
        int64_t last_ms = 0;
        uint32_t record_count = 0;
        uint32_t payload_schema = 0;
        ReportSpoolBuffer payload;
    };
    struct PlotBuildBucket {
        bool have = false;
        int range_index = -1;
        int64_t start_t = 0;
        int64_t end_t = 0;
        int64_t min_t = 0;
        int64_t max_t = 0;
        int32_t start_value = 0;
        int32_t end_value = 0;
        int32_t min_value = 0;
        int32_t max_value = 0;

        void clear() {
            have = false;
            range_index = -1;
            start_t = 0;
            end_t = 0;
            min_t = 0;
            max_t = 0;
            start_value = 0;
            end_value = 0;
            min_value = 0;
            max_value = 0;
        }
    };

    static constexpr size_t PREFETCH_SKIP_MAX = 8;
    static constexpr size_t AC_REPORT_COALESCE_SLOTS = 8;
    static constexpr size_t AC_REPORT_COALESCE_TARGET_BYTES = 64 * 1024;
    static constexpr size_t AC_REPORT_CACHE_WRITE_QUEUE_MAX = 8;
    static constexpr size_t AC_REPORT_CACHE_WRITE_BACKPRESSURE_WATERMARK =
        AC_REPORT_CACHE_WRITE_QUEUE_MAX / 2;

    enum class CacheWriteEnqueueResult : uint8_t {
        Queued,
        Blocked,
        Failed,
    };
    enum class CacheFlushResult : uint8_t {
        Flushed,
        Blocked,
        Failed,
    };

    static bool write_parsed_chunk(void *context,
                                   const ReportParsedChunk &chunk);
    static bool collect_result_chunk(void *context,
                                     const ReportStoreChunkInfo &chunk);

    bool ensure_summary_records();
    bool parse_summary_result(ReportSpoolResult &result);
    bool load_summary_from_store();
    void finalize_summary_records();
    void finish_summary_fetch();
    void fail_summary(const char *message);
    void clear_summary_records();
    const char *summary_state_name() const;

    bool cache_source_supported(ReportSourceId source) const;
    bool build_cache_plan(const ReportSummaryRecord &night, bool force, bool latest_tail_refresh);
    bool start_next_cache_source();
    bool store_cache_round(ReportSpoolResult &result);
    bool write_cache_source_coverage(ReportSourceId source, int64_t from_ms);
    void reset_cache_source_coverage_marks();
    void note_cache_chunk_coverage(const ReportParsedChunk &chunk);
    bool drain_source_events(RpcArbiter &arbiter);
    void poll_cache_fetch(RpcArbiter &arbiter);
    void log_spool_can_pressure(const RpcArbiter &arbiter);
    bool fail_cache_fetch_if_write_failed();
    bool drain_cache_spool_rounds();
    void finish_cache_spool_if_terminal();
    void finish_cache_fetch();
    void fail_cache_fetch(const char *message);
    bool source_chunk_extent(const ReportSummaryRecord &night,
                             ReportSourceId source,
                             const char *name,
                             int64_t &min_start,
                             int64_t &max_end) const;
    bool buffer_parsed_chunk(const ReportParsedChunk &chunk);
    CacheFlushResult flush_cache_coalesce_buffer(size_t slot);
    CacheFlushResult flush_all_cache_coalesce_buffers();
    void discard_cache_coalesce_buffers();
    CacheWriteEnqueueResult enqueue_cache_write(
        CacheCoalesceBuffer &buf,
        const ReportStoreChunkKey &key,
        const ReportStoreChunkMeta &meta);
    void reset_cache_write_fetch_state_locked();
    void begin_cache_write_fetch();
    void abort_cache_write_fetch();
    bool cache_write_backpressure_active() const;
    bool cache_writes_pending_for_active_fetch() const;
    bool cache_write_failed_for_active_fetch(std::string &error) const;
    bool finalize_cache_source_if_ready();
    void note_cache_chunk_committed(uint64_t night_start_ms);
    bool clear_cache_range(int64_t start_ms,
                           int64_t end_ms,
                           ReportCacheClearResult &out);
    int64_t night_start_for_timestamp(int64_t timestamp_ms) const;

    void service_prefetch(bool realtime_active);
    void prefetch_yield_to_foreground();
    void set_prefetch_phase(PrefetchPhase phase, uint64_t night_ms,
                            bool inc_completed, bool inc_failed);
    void prefetch_note_failure(uint64_t night_ms);
    bool prefetch_in_cooldown(uint64_t night_ms, uint32_t now_ms) const;
    void clear_sparse_event_empty_markers(uint64_t night_start_ms);
    void note_sparse_event_confirmed_empty(const ReportSummaryRecord &night,
                                           const ReportSourceDef &source);
    bool sparse_event_confirmed_empty(const ReportSummaryRecord &night,
                                      const ReportSourceDef &source) const;
    bool sparse_event_refresh_due(const ReportSummaryRecord &night,
                                  const ReportSourceDef &source) const;
    bool sparse_event_refresh_due_for_night(
        const ReportSummaryRecord &night) const;

    bool ensure_result_chunks();
    void clear_result_prepare();
    void fail_result_prepare(const char *message);
    bool add_result_stream(ReportStoreChunkKind kind,
                           ReportSourceId source,
                           ReportSignalId signal,
                           const char *name,
                           bool required,
                           bool complete,
                           size_t &stream_index);
    bool add_result_chunks_for_range(ReportStoreChunkKind kind,
                                     ReportSourceId source,
                                     ReportSignalId signal,
                                     const char *name,
                                     int64_t night_start_ms,
                                     int64_t start_ms,
                                     int64_t end_ms,
                                     bool required);
    bool prepare_result_by_therapy_index_internal(size_t therapy_index,
                                                  bool refresh_cache);
    bool defer_result_prepare_for_summary(size_t therapy_index,
                                          bool refresh_cache);
    bool load_result_night(size_t therapy_index,
                           ReportSummaryRecord &night);
    bool publish_existing_result_if_current(size_t therapy_index,
                                            const ReportSummaryRecord &night,
                                            bool refresh_cache);
    void begin_result_prepare_for_night(size_t therapy_index,
                                        const ReportSummaryRecord &night);
    bool refresh_result_cache_if_needed(const ReportSummaryRecord &night,
                                        const ReportNightCoverageStatus &coverage,
                                        size_t therapy_index,
                                        bool refresh_cache,
                                        bool &deferred);
    bool add_result_chunks_for_night(const ReportSummaryRecord &night,
                                     int64_t range_start_ms,
                                     int64_t range_end_ms);
    bool count_result_events_from_chunks();
    void apply_result_event_indices_from_counts();
    bool finalize_result_prepare(size_t therapy_index);
    size_t derive_result_session_ranges(PlotRange *ranges, size_t max_ranges) const;
    void apply_result_session_ranges(const PlotRange *ranges, size_t range_count);
    const char *result_state_name() const;

    void reset_plot_build();
    void build_empty_plot_bin(ReportSpoolBuffer &out) const;
    bool start_result_plot_build();
    void poll_result_plot_build();
    bool plot_time_in_ranges(int64_t timestamp_ms) const;
    int plot_range_index(int64_t timestamp_ms) const;
    bool process_plot_event_chunk(const ReportResultChunk &chunk);
    bool open_plot_series(const ReportResultStream &stream);
    bool process_plot_series_chunk(const ReportResultChunk &chunk);
    void flush_plot_bucket();
    bool finish_result_plot_build();
    bool build_range_plot(int64_t from_ms, int64_t to_ms, ReportSpoolBuffer &out);

    bool result_plot_cache_path_for_night(const ReportSummaryRecord &night,
                                          char *path,
                                          size_t path_size) const;
    bool result_plot_cache_path(char *path, size_t path_size) const;
    bool clear_plot_cache_for_night(const ReportSummaryRecord &night,
                                    uint32_t &deleted) const;
    bool load_result_plot_cache();
    bool save_result_plot_cache() const;

    ReportSummaryRecord *records_ = nullptr;
    size_t record_count_ = 0;
    uint32_t nights_with_therapy_ = 0;
    ReportSummaryRecord *summary_scratch_ = nullptr;
    ReportSummaryStatus summary_status_;
    mutable SemaphoreHandle_t summary_lock_ = nullptr;
    SemaphoreHandle_t summary_scratch_lock_ = nullptr;
    LargeTextBuffer summary_json_snapshot_;
    LargeTextBuffer summary_json_build_;
    uint32_t next_summary_progress_snapshot_ms_ = 0;
    bool take_summary_lock(TickType_t timeout) const;
    void give_summary_lock() const;
    bool take_summary_scratch(TickType_t timeout,
                              ReportSummaryRecord *&out);
    void give_summary_scratch();
    void publish_summary_json_snapshot();
    bool summary_night_by_therapy_index_unlocked(size_t therapy_index,
                                                 ReportSummaryRecord &out) const;
    bool summary_night_by_start_unlocked(uint64_t night_start_ms,
                                         ReportSummaryRecord &out) const;
    std::atomic<uint32_t> summary_revision_pub_{0};  // see summary_revision()
    SpoolClient spool_;
    uint32_t observed_spool_rx_queue_full_alerts_ = 0;
    bool summary_fetch_active_ = false;
    uint32_t summary_started_ms_ = 0;

    bool cache_fetch_active_ = false;
    bool pending_result_prepare_ = false;
    bool pending_result_refresh_cache_ = false;
    size_t pending_result_therapy_index_ = 0;
    ReportSummaryRecord cache_night_;
    ReportCacheSourcePlan cache_plan_[AC_REPORT_CACHE_SOURCE_MAX] = {};
    size_t cache_source_count_ = 0;
    size_t cache_source_index_ = 0;

    // Per-summary-night max observed chunk.end_ms during the current fetch
    // (0 = night received no data this sweep). Bounds how far coverage may be
    // claimed so a partial fetch never marks a whole night complete.
    // Positional sidecar for records_: summary refresh is refused while a cache
    // fetch is active, so record indices cannot change between chunk parsing
    // and coverage writing.
    int64_t cache_source_night_extent_ms_[AC_REPORT_SUMMARY_RECORD_MAX] = {};

    // Write-side coalescing: buffer parsed chunks per (kind,name) and flush
    // larger files, so a night reads back as a few files instead of hundreds.
    CacheCoalesceBuffer cache_coalesce_[AC_REPORT_COALESCE_SLOTS];
    struct CacheWriteQueueSlot {
        bool active = false;
        uint32_t fetch_id = 0;
        ReportStoreChunkKey key;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
    };
    CacheWriteQueueSlot cache_write_queue_[AC_REPORT_CACHE_WRITE_QUEUE_MAX];
    mutable SemaphoreHandle_t cache_write_lock_ = nullptr;
    size_t cache_write_head_ = 0;
    size_t cache_write_tail_ = 0;
    size_t cache_write_count_ = 0;
    uint32_t cache_write_fetch_id_ = 0;
    uint32_t cache_write_pending_ = 0;
    uint32_t cache_write_failed_fetch_id_ = 0;
    std::string cache_write_error_;
    bool cache_source_finalizing_ = false;
    ReportCacheSourcePlan cache_finalizing_plan_;
    ReportCacheFetchStatus cache_status_;
    // Monotonic count of chunks ever written this boot (survives the per-fetch
    // cache_status_ reset). Exposed in the summary so the browser can drop a
    // client-cached plot once new data has landed for any night.
    uint32_t cache_data_epoch_ = 0;
    // Per-night data version, keyed by the stable night_start_ms so it survives
    // summary rebuilds: bumped on every chunk write to that night. Backs the
    // report ETag (night_etag) so a GET derives a night's version without SD I/O.
    struct NightEpoch {
        uint64_t night_start_ms = 0;
        uint32_t epoch = 0;
    };
    NightEpoch *night_epochs_ = nullptr;  // PSRAM, AC_REPORT_SUMMARY_RECORD_MAX
    size_t night_epoch_count_ = 0;
    uint32_t night_epoch_for_unlocked(uint64_t night_start_ms) const;
    void format_night_etag(const ReportSummaryRecord &rec, char *out,
                           size_t out_size) const;
    void format_night_etag_unlocked(const ReportSummaryRecord &rec,
                                    char *out,
                                    size_t out_size) const;
    uint32_t next_trash_cleanup_ms_ = 0;

    mutable SemaphoreHandle_t prefetch_lock_ = nullptr;
    PrefetchPhase prefetch_phase_ = PrefetchPhase::Idle;
    uint64_t prefetch_active_night_ = 0;
    uint64_t prefetch_last_night_ = 0;
    uint64_t prefetch_last_failed_night_ = 0;
    bool prefetch_preempt_req_ = false;
    uint32_t prefetch_completed_ = 0;
    uint32_t prefetch_failed_ = 0;
    char prefetch_last_source_[48] = {};
    char prefetch_last_error_[48] = {};
    PrefetchSkip prefetch_skip_[PREFETCH_SKIP_MAX] = {};
    struct SparseEventEmptyMarker {
        ReportSourceId source = ReportSourceId::Summary;
        uint64_t night_ms = 0;
        char night_key[48] = {};
    };
    SparseEventEmptyMarker sparse_event_empty_[4] = {};

    ReportResultChunk *result_chunks_ = nullptr;
    size_t result_chunk_capacity_ = 0;
    ReportResultStream result_streams_[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t result_stream_count_ = 0;

    // Served-result LRU. The result_* slot above is build SCRATCH only; GET-by-
    // index serves these immutable per-night entries, so two clients on different
    // nights never clobber each other. Published atomically at build finish.
    struct MaterializedResult {
        bool valid = false;
        uint64_t night_start_ms = 0;
        char etag[48] = {};
        uint32_t last_used = 0;
        ReportResultStatus status;
        ReportSummaryRecord night;
        ReportResultStream streams[AC_REPORT_RESULT_STREAM_MAX] = {};
        size_t stream_count = 0;
        std::shared_ptr<ReportSpoolBuffer> plot;
    };
    MaterializedResult *result_slots_ = nullptr;  // PSRAM, AC_REPORT_RESULT_SLOT_MAX
    SemaphoreHandle_t result_slots_lock_ = nullptr;
    // Zoom range plot: request set by the web thread, built on the main loop,
    // result held for serving. All guarded by result_slots_lock_
    bool range_req_active_ = false;
    size_t range_req_index_ = 0;
    int64_t range_req_from_ = 0;
    int64_t range_req_to_ = 0;
    std::shared_ptr<ReportSpoolBuffer> range_plot_bytes_;
    size_t range_plot_index_ = 0;
    int64_t range_plot_from_ = 0;
    int64_t range_plot_to_ = 0;
    void service_range_plot(bool realtime_active);
    uint32_t result_slot_tick_ = 0;
    void publish_result_to_slot();
    void update_materialized_status_locked();
    void invalidate_materialized_locked(uint64_t night_start_ms, bool all);
    void invalidate_materialized(uint64_t night_start_ms, bool all);
    void build_result_json_from(const ReportResultStatus &status,
                                const ReportSummaryRecord &night,
                                const ReportResultStream *streams,
                                size_t stream_count,
                                const ReportCacheFetchStatus &cache,
                                LargeTextBuffer &json) const;

    // Bounded dedup build queue: a GET-miss (or POST refresh) enqueues a night to
    // build; the main loop services one at a time, replacing the single pending slot.
    struct ResultBuildJob {
        uint64_t night_start_ms = 0;
        size_t therapy_index = 0;
        bool refresh = false;
    };
    enum class BuildQueueResult : uint8_t {
        Queued,
        AlreadyQueued,
        Full,
        Unavailable,
    };
    ResultBuildJob build_queue_[AC_REPORT_BUILD_QUEUE_MAX];
    size_t build_queue_head_ = 0;
    size_t build_queue_count_ = 0;
    SemaphoreHandle_t build_queue_lock_ = nullptr;
    BuildQueueResult enqueue_build(uint64_t night_start_ms,
                                   size_t therapy_index,
                                   bool refresh);
    void clear_build_queue(uint64_t night_start_ms, bool all);
    void service_build_queue(bool realtime_active);

    ReportSummaryRecord result_night_;
    ReportResultStatus result_status_;
    ReportSpoolBuffer result_plot_bin_;
    ReportSpoolBuffer plot_build_bin_;
    ReportSpoolBuffer plot_tmp_;
    ReportSpoolBuffer plot_seen_events_;

    bool plot_bin_ok_ = true;
    bool plot_build_active_ = false;
    ReportPlotBuildPhase plot_build_phase_ = ReportPlotBuildPhase::Idle;
    PlotRange plot_ranges_[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t plot_range_count_ = 0;
    int64_t plot_start_ms_ = 0;
    int64_t plot_end_ms_ = 0;
    int64_t plot_bucket_ms_ = 1;
    uint32_t plot_chunk_index_ = 0;
    size_t plot_stream_index_ = 0;
    int64_t plot_current_bucket_ = -1;
    bool plot_series_open_ = false;
    PlotBuildBucket plot_bucket_;
};

}  // namespace aircannect
