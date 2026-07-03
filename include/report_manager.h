#pragma once

#include <atomic>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <new>
#include <string>

#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "large_text_buffer.h"
#include "edf_report_catalog.h"
#include "report_data_provider.h"
#include "report_daily_metrics.h"
#include "report_materializer.h"
#include "report_night_index.h"
#include "report_parser.h"
#include "report_proto.h"
#include "report_spool_types.h"
#include "rpc_arbiter.h"
#include "spool_client.h"

namespace aircannect {

class EdfReportCatalogJob;
struct EdfReportCatalogStatus;
struct ReportSeriesSample;
struct EdfReportSessionDescriptor;
struct EdfReportRequiredRange;
struct ReportResolveScratch;

static constexpr size_t AC_REPORT_SUMMARY_RECORD_MAX = 256;
static constexpr size_t AC_REPORT_NIGHT_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_CACHE_SOURCE_MAX = 8;
static constexpr size_t AC_REPORT_RESULT_CHUNK_MAX = 512;
static constexpr size_t AC_REPORT_RESULT_STREAM_MAX = 16;
static constexpr size_t AC_REPORT_RESULT_SLOT_MAX = 4;
static constexpr size_t AC_REPORT_BUILD_QUEUE_MAX = 4;
static constexpr size_t AC_REPORT_RESULT_ETAG_MAX = 80;

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
    uint32_t result_json_deleted = 0;
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
    bool events_available = false;  // event source covered: counts are real, not unknown
    bool ahi_valid = false;
    bool oa_index_valid = false;
    bool ca_index_valid = false;
    bool ua_index_valid = false;
    bool hypopnea_index_valid = false;
    bool arousal_index_valid = false;
    bool mask_pressure_50_valid = false;
    bool leak_50_valid = false;
    ReportMetricSource ahi_source = ReportMetricSource::None;
    ReportMetricSource oa_index_source = ReportMetricSource::None;
    ReportMetricSource ca_index_source = ReportMetricSource::None;
    ReportMetricSource ua_index_source = ReportMetricSource::None;
    ReportMetricSource hypopnea_index_source = ReportMetricSource::None;
    ReportMetricSource arousal_index_source = ReportMetricSource::None;
    ReportMetricSource mask_pressure_50_source = ReportMetricSource::None;
    ReportMetricSource leak_50_source = ReportMetricSource::None;
    float ahi = 0.0f;
    float oa_index = 0.0f;
    float ca_index = 0.0f;
    float ua_index = 0.0f;
    float hypopnea_index = 0.0f;
    float arousal_index = 0.0f;
    float mask_pressure_50_cm_h2o = 0.0f;
    float leak_50_l_min = 0.0f;
    uint32_t oa_count = 0;
    uint32_t ca_count = 0;
    uint32_t ua_count = 0;
    uint32_t hypopnea_count = 0;
    uint32_t arousal_count = 0;
    std::string error;
};

class ReportManager : private ReportMaterializerSink {
public:
    ~ReportManager();

    // Lifecycle and device events
    void begin();
    void set_edf_report_catalog(EdfReportCatalogJob *catalog);
    void poll(RpcArbiter &arbiter);
    bool handle_event(const RpcEvent &event);

    // Summary and night index
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

    // Coverage and spool cache
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

    // Work state and background service
    bool busy() const {
        return summary_fetch_active_ || cache_fetch_active_ ||
               plot_build_active_ || range_build_active_;
    }
    bool background_work_active() const;

    bool next_night_needing_cache(uint64_t &night_start_ms_out) const;
    const ReportCacheFetchStatus &cache_fetch_status() const {
        return cache_status_;
    }

    // Report prefetch
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

    // Idle plot prebuild and build queue
    enum class PlotPrebuildResult : uint8_t {
        Queued,
        AlreadyQueued,
        Scanned,
        Drained,
        Waiting,
        Unavailable,
    };
    PlotPrebuildResult request_idle_plot_prebuild();
    void preempt_idle_plot_prebuild();

    struct BuildQueueSnapshot {
        bool available = false;
        bool lock_ok = false;
        size_t count = 0;
        uint64_t head_night_ms = 0;
        size_t head_therapy_index = 0;
        bool head_refresh = false;
        bool head_idle_prebuild = false;
        uint32_t head_age_ms = 0;
        uint64_t last_night_ms = 0;
        size_t last_therapy_index = 0;
        char last_outcome[16] = {};
        char last_state[16] = {};
        char last_error[48] = {};
        uint32_t enqueue_total = 0;
        uint32_t queued_total = 0;
        uint32_t already_total = 0;
        uint32_t service_total = 0;
        uint64_t last_enqueue_night_ms = 0;
        size_t last_enqueue_therapy_index = 0;
        char last_read[24] = {};
        char last_enqueue_result[24] = {};
        char last_service_block[24] = {};
    };
    BuildQueueSnapshot build_queue_snapshot() const;
    bool edf_catalog_status(EdfReportCatalogStatus &out,
                            uint32_t timeout_ms = 0) const;

    bool foreground_busy() const;
    bool service_cache_writer();

    // Result materialization
    bool prepare_result_by_therapy_index(size_t therapy_index,
                                         bool refresh_cache = false);
    bool request_result_prepare_by_therapy_index(size_t therapy_index,
                                                 bool refresh_cache = false);
    bool request_result_prepare_by_start(uint64_t night_start_ms,
                                         bool refresh_cache = false);
    void build_result_chunks_json(LargeTextBuffer &json,
                                  size_t offset,
                                  size_t limit) const;

    // Result serving
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
    ResultRead read_result_by_start(uint64_t night_start_ms,
                                    const char *if_none_match,
                                    char *etag_out,
                                    size_t etag_out_size,
                                    LargeTextBuffer &json_out);

    // Plot serving
    // Stateless per-night plot read: serves the PSRAM blob for (night, version);
    // a miss queues a build and returns Building.
    enum class PlotRead : uint8_t {
        NotFound,
        Ready,
        Error,
        Building,
        Stale,
        Empty,
        QueueFull,
        Unavailable,
        Busy,
    };

    PlotRead read_plot(size_t therapy_index, const char *version,
                       char *etag_out, size_t etag_out_size,
                       std::shared_ptr<ReportSpoolBuffer> &out);
    PlotRead read_plot_range(size_t therapy_index, const char *version,
                             char *etag_out, size_t etag_out_size,
                             int64_t from_ms, int64_t to_ms,
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
    // Materialized result and plot types
    struct PlotRange {
        int64_t start_ms = 0;
        int64_t end_ms = 0;
    };

    struct ReportResultChunk {
        ReportProviderChunkRef provider_ref;
        ReportStoreChunkKind kind = ReportStoreChunkKind::Series;
        ReportSourceId source = ReportSourceId::Summary;
        ReportSignalId signal = ReportSignalId::Flow;
        const char *name = nullptr;
        uint8_t stream_index = 0;
        uint32_t stream_mask = 0;
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
        bool has_edf_segment = false;
        bool has_spool_segment = false;
        uint32_t chunk_count = 0;
        uint32_t record_count = 0;
        uint32_t payload_bytes = 0;
    };

    enum class ResultPrepareOutcome : uint8_t {
        Prepared,
        Deferred,
        Retry,
        Failed,
    };

    struct PrefetchSkip {
        uint64_t night_ms = 0;
        uint32_t until_ms = 0;
    };

    struct SparseEventEmptyMarker {
        ReportSourceId source = ReportSourceId::Summary;
        uint64_t night_ms = 0;
        char night_key[48] = {};
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
        uint32_t series_interval_ms = 0;
        bool series_values_pending = false;
        ReportSpoolBuffer payload;
    };

    struct CacheWriteQueueSlot {
        bool active = false;
        uint32_t fetch_id = 0;
        ReportStoreChunkKey key;
        ReportStoreChunkMeta meta;
        ReportSpoolBuffer payload;
    };

    struct NightEpoch {
        uint64_t night_start_ms = 0;
        uint32_t epoch = 0;
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
    struct PlotSeriesBuildState {
        ReportSpoolBuffer points;
        PlotBuildBucket bucket;
        int64_t current_bucket = -1;
        int64_t current_bucket_start_ms = 0;
        int64_t current_bucket_end_ms = 0;
        int64_t current_bucket_ms = 0;
        int64_t series_bucket_ms = 0;
        bool open = false;
        bool have_last_sample = false;
        int64_t last_sample_ms = 0;
        int last_range_index = -1;

        void reset() {
            points.clear();
            bucket.clear();
            current_bucket = -1;
            current_bucket_start_ms = 0;
            current_bucket_end_ms = 0;
            current_bucket_ms = 0;
            series_bucket_ms = 0;
            open = false;
            have_last_sample = false;
            last_sample_ms = 0;
            last_range_index = -1;
        }
    };

    struct MaterializedResult {
        bool valid = false;
        uint64_t night_start_ms = 0;
        char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
        uint32_t last_used = 0;
        ReportResultStatus status;
        ReportIndexedNight night;
        PlotRange ranges[AC_REPORT_SUMMARY_SESSION_MAX] = {};
        size_t range_count = 0;
        ReportResultStream streams[AC_REPORT_RESULT_STREAM_MAX] = {};
        size_t stream_count = 0;
        ReportResultChunk chunks[AC_REPORT_RESULT_CHUNK_MAX] = {};
        size_t chunk_count = 0;
        EdfReportSessionDescriptor
            edf_sessions[AC_REPORT_EDF_SESSION_MAX] = {};
        size_t edf_session_count = 0;
        std::shared_ptr<ReportSpoolBuffer> plot;
    };

    enum class ResultCacheWritePhase : uint8_t {
        Idle,
        ClearOld,
        OpenPlotTmp,
        WritePlot,
        ClosePlotRename,
        OpenResultTmp,
        WriteResult,
        CloseResultRename,
    };

    struct ResultCacheWriteJob {
        bool active = false;
        ResultCacheWritePhase phase = ResultCacheWritePhase::Idle;
        ReportSummaryRecord night;
        char plot_path[192] = {};
        char plot_tmp_path[200] = {};
        char result_path[192] = {};
        char result_tmp_path[200] = {};
        std::shared_ptr<ReportSpoolBuffer> result_json;
        std::shared_ptr<ReportSpoolBuffer> plot;
        size_t offset = 0;
        File file;
    };

    struct ResultBuildJob {
        uint64_t night_start_ms = 0;
        size_t therapy_index = 0;
        bool refresh = false;
        bool idle_prebuild = false;
        uint32_t queued_ms = 0;
        uint32_t next_attempt_ms = 0;
    };

    enum class BuildQueueResult : uint8_t {
        Queued,
        AlreadyQueued,
        Full,
        Unavailable,
    };

    // Cache and prefetch constants
    static constexpr size_t PREFETCH_SKIP_MAX = 8;
    static constexpr size_t AC_REPORT_COALESCE_SLOTS = 64;
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

    // Provider callbacks
    static bool write_parsed_chunk(void *context,
                                   const ReportParsedChunk &chunk);
    static bool collect_result_chunk(void *context,
                                     const ReportProviderChunk &chunk);
    static bool collect_range_chunk(void *context,
                                    const ReportProviderChunk &chunk);

    // Summary store and snapshots
    bool ensure_summary_records();
    bool parse_summary_result(ReportSpoolResult &result);
    bool load_summary_from_store();
    void finalize_summary_records();
    void finish_summary_fetch();
    void fail_summary(const char *message);
    void clear_summary_records();
    const char *summary_state_name() const;
    bool take_summary_lock(TickType_t timeout) const;
    void give_summary_lock() const;
    bool take_summary_scratch(TickType_t timeout,
                              ReportSummaryRecord *&out);
    void give_summary_scratch();
    bool publish_summary_json_snapshot();
    bool build_indexed_nights(ReportIndexedNight *out,
                              size_t capacity,
                              size_t &count) const;
    bool build_indexed_nights_uncached(ReportIndexedNight *out,
                                       size_t capacity,
                                       size_t &count) const;
    bool load_durable_night_index();
    bool seed_index_from_durable(ReportNightIndex &index) const;
    void schedule_durable_night_index_save(const ReportIndexedNight *src,
                                           size_t count,
                                           uint32_t content_crc) const;
    bool indexed_night_by_therapy_index(size_t therapy_index,
                                        ReportIndexedNight &out) const;
    bool indexed_night_by_start(uint64_t night_start_ms,
                                ReportIndexedNight &out,
                                size_t *therapy_index_out = nullptr) const;
    bool index_cache_matches(uint32_t summary_revision,
                             bool catalog_present,
                             uint8_t catalog_state,
                             uint32_t catalog_refresh_id) const;
    bool copy_index_cache_locked(ReportIndexedNight *out,
                                 size_t capacity,
                                 size_t &count,
                                 uint32_t summary_revision,
                                 bool catalog_present,
                                 uint8_t catalog_state,
                                 uint32_t catalog_refresh_id) const;
    bool publish_index_cache_locked(const ReportIndexedNight *src,
                                    size_t count,
                                    uint32_t summary_revision,
                                    bool catalog_present,
                                    uint8_t catalog_state,
                                    uint32_t catalog_refresh_id) const;
    bool index_cache_key(uint32_t &summary_revision,
                         bool &catalog_present,
                         uint8_t &catalog_state,
                         uint32_t &catalog_refresh_id) const;

    // Spool cache fetch and writes
    bool cache_source_supported(ReportSourceId source) const;
    bool build_cache_plan(const ReportIndexedNight &night,
                          bool force,
                          bool latest_tail_refresh);
    bool start_next_cache_source();
    bool store_cache_round(ReportSpoolResult &result);
    bool write_cache_source_coverage(ReportSourceId source, int64_t from_ms);
    bool reset_cache_source_coverage_marks();
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
    bool ensure_cache_source_night_extents();
    bool ensure_cache_coalesce_slots();
    bool ensure_cache_write_queue_slots();
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
    uint32_t night_epoch_for_unlocked(uint64_t night_start_ms) const;
    void format_night_etag_unlocked(const ReportSummaryRecord &rec,
                                    uint64_t source_signature,
                                    char *out,
                                    size_t out_size) const;
    bool clear_cache_range(int64_t start_ms,
                           int64_t end_ms,
                           ReportCacheClearResult &out);
    int64_t night_start_for_timestamp(int64_t timestamp_ms) const;

    // Prefetch and EDF coverage
    void service_prefetch(bool realtime_active);
    void prefetch_yield_to_foreground();
    bool edf_report_catalog_pending() const;
    bool collect_edf_sessions_for_night(
        const ReportSummaryRecord &night,
        int64_t range_start_ms,
        int64_t range_end_ms,
        EdfReportSessionDescriptor *sessions,
        size_t session_capacity,
        size_t &session_count,
        bool *pending_out = nullptr) const;
    bool edf_catalog_session_reportable(
        const EdfReportSessionDescriptor &session,
        EdfReportSessionDescriptor &scratch) const;
    bool edf_catalog_session_has_annotation_marker(
        const EdfReportSessionDescriptor &session,
        EdfReportSessionDescriptor &scratch) const;
    bool edf_catalog_annotation_has_numeric_session(
        const EdfReportSessionDescriptor &session,
        EdfReportSessionDescriptor &scratch) const;
    bool append_edf_sessions_for_selected_days(
        EdfReportSessionDescriptor *sessions,
        size_t session_capacity,
        size_t &session_count) const;
    bool edf_report_complete_for_night(const ReportSummaryRecord &night,
                                       int64_t range_start_ms,
                                       int64_t range_end_ms,
                                       bool *pending_out = nullptr) const;
    bool edf_report_complete_for_indexed_night(
        const ReportIndexedNight &night,
        int64_t range_start_ms,
        int64_t range_end_ms,
        bool *pending_out = nullptr) const;
    bool edf_report_complete_for_night_sessions(
        const ReportSummaryRecord &night) const;
    void set_prefetch_phase(PrefetchPhase phase, uint64_t night_ms,
                            bool inc_completed, bool inc_failed);
    void prefetch_note_failure(uint64_t night_ms);
    bool prefetch_in_cooldown(uint64_t night_ms, uint32_t now_ms) const;
    void clear_sparse_event_empty_markers(uint64_t night_start_ms);
    void note_sparse_event_confirmed_empty(const ReportSummaryRecord &night,
                                           const ReportSourceDef &source);
    bool sparse_event_confirmed_empty(const ReportSummaryRecord &night,
                                      const ReportSourceDef &source) const;

    // Result materialization
    bool ensure_result_chunks();
    bool ensure_result_slots();
    bool ensure_result_edf_sessions();
    bool ensure_result_resolve_buffers();
    bool result_uses_edf_provider() const;
    void release_result_edf_sessions();
    void clear_result_prepare();
    void fail_result_prepare(const char *message);
    bool add_result_stream(ReportStoreChunkKind kind,
                           ReportSourceId source,
                           ReportSignalId signal,
                           const char *name,
                           bool required,
                           bool complete,
                           size_t &stream_index);
    bool add_provider_chunks_to_result_stream(
        const ReportDataProvider &provider,
        ReportStoreChunkKind kind,
        ReportSourceId source,
        ReportSignalId signal,
        const char *name,
        int64_t night_start_ms,
        int64_t start_ms,
        int64_t end_ms,
        bool required,
        bool complete,
        size_t stream_index);
    bool add_provider_result_chunk(const ReportProviderChunk &provider_chunk,
                                   bool required,
                                   size_t stream_index);
    bool find_edf_sessions_for_night(const ReportSummaryRecord &night,
                                     int64_t range_start_ms,
                                     int64_t range_end_ms,
                                     bool *pending_out = nullptr);
    bool resolve_and_materialize_result_for_night(
        const ReportIndexedNight &night,
        int64_t range_start_ms,
        int64_t range_end_ms,
        bool *edf_pending_out = nullptr);
    bool begin_materialization(const ReportIndexedNight &night,
                               const ReportResolvedPlan &plan) override;
    bool add_materialized_stream(const ReportResolvedStream &stream,
                                 size_t &result_stream_index) override;
    bool add_materialized_segment(const ReportResolvedSegment &segment,
                                  size_t result_stream_index) override;
    void finish_materialization(const ReportResolvedPlan &plan) override;
    bool read_result_chunk_payload(const ReportResultChunk &chunk,
                                   ReportStoreChunkMeta &meta,
                                   ReportSpoolBuffer &payload);
    void provider_chunk_from_result(const ReportResultChunk &chunk,
                                    ReportProviderChunk &out) const;
    bool provider_chunk_from_result_stream(
        const ReportResultChunk &chunk,
        size_t stream_index,
        const ReportResultStream *streams,
        size_t stream_count,
        const EdfReportSessionDescriptor *sessions,
        size_t session_count,
        ReportProviderChunk &out) const;
    bool result_chunk_has_stream(const ReportResultChunk &chunk,
                                 size_t stream_index) const;
    bool result_chunk_same_physical_edf(
        const ReportResultChunk &existing,
        const ReportProviderChunk &candidate) const;
    bool for_each_result_series_sample(
        const ReportResultChunk &chunk,
        size_t stream_index,
        ReportProviderSeriesReadStats &stats,
        ReportSeriesSampleCallback callback,
        void *context);
    ResultPrepareOutcome prepare_result_by_therapy_index_internal(
        size_t therapy_index,
        bool refresh_cache);
    ResultPrepareOutcome prepare_result_by_night_start_internal(
        uint64_t night_start_ms,
        size_t therapy_index,
        bool refresh_cache);
    bool defer_result_prepare_for_summary(size_t therapy_index,
                                          bool refresh_cache);
    bool load_result_night(size_t therapy_index,
                           ReportSummaryRecord &night);
    bool publish_existing_result_if_current(size_t therapy_index,
                                            const ReportIndexedNight &night,
                                            const char *current_etag,
                                            bool refresh_cache);
    void begin_result_prepare_for_night(size_t therapy_index,
                                        const ReportIndexedNight &night,
                                        const char *current_etag);
    bool refresh_result_cache_if_needed(const ReportIndexedNight &night,
                                        size_t therapy_index,
                                        bool refresh_cache,
                                        bool &deferred);
    bool activate_cache_plan_for_night(const ReportSummaryRecord &night);
    bool count_result_events_from_chunks();
    void apply_result_event_indices_from_counts();
    bool apply_result_series_metrics_from_chunks();
    bool result_timestamp_in_ranges(int64_t timestamp_ms) const;
    bool finalize_result_prepare(size_t therapy_index);
    const char *result_state_name() const;
    void clear_result_ranges();
    bool set_result_ranges_from_indexed_night(const ReportIndexedNight &night);
    bool set_result_ranges_from_edf_sessions();
    bool result_data_span(int64_t &span_start_ms,
                          int64_t &span_end_ms) const;

    // Result slot and JSON serving
    void clear_materialized_slot_locked(MaterializedResult &slot);
    bool publish_result_to_slot(bool cache_plot = false);
    void update_materialized_status_locked();
    void clear_range_plot_locked(uint64_t night_start_ms, bool all);
    void invalidate_materialized_locked(uint64_t night_start_ms, bool all);
    void invalidate_materialized(uint64_t night_start_ms, bool all);
    void build_result_json_from(const ReportResultStatus &status,
                                const ReportIndexedNight &night,
                                const PlotRange *ranges,
                                size_t range_count,
                                const ReportResultStream *streams,
                                size_t stream_count,
                                const ReportCacheFetchStatus &cache,
                                LargeTextBuffer &json) const;
    ResultRead read_result_for_indexed_night(
        size_t therapy_index,
        const ReportIndexedNight &indexed_night,
        const char *if_none_match,
        char *etag_out,
        size_t etag_out_size,
        LargeTextBuffer &json_out);

    // Full-night plot build
    void reset_plot_build();
    void build_empty_plot_bin(ReportSpoolBuffer &out) const;
    bool start_result_plot_build();
    void poll_result_plot_build();
    int plot_range_index(int64_t timestamp_ms) const;
    bool process_plot_event_chunk(const ReportResultChunk &chunk);
    bool open_plot_series_state(size_t stream_index);
    bool process_plot_series_sample_value(PlotSeriesBuildState &state,
                                          const ReportResultChunk &chunk,
                                          const ReportSeriesSample &sample,
                                          uint32_t interval_ms);
    bool append_plot_series_value(PlotSeriesBuildState &state,
                                  int64_t timestamp_ms,
                                  int32_t value_milli,
                                  int64_t bucket_ms);
    bool append_plot_series_point(PlotSeriesBuildState &state,
                                  int64_t timestamp_ms,
                                  int32_t value_milli,
                                  int64_t bucket_ms);
    bool append_plot_series_gap(PlotSeriesBuildState &state);
    bool process_plot_series_chunk(size_t chunk_index);
    bool process_plot_edf_series_batch(size_t seed_chunk_index,
                                       bool &processed);
    uint8_t flush_plot_bucket_to(ReportSpoolBuffer &out,
                                 PlotBuildBucket &bucket,
                                 int64_t base_ms,
                                 bool &ok);
    uint8_t emit_plot_gap_to(ReportSpoolBuffer &out,
                             PlotBuildBucket &bucket,
                             int64_t base_ms,
                             bool &ok);
    void flush_plot_bucket(PlotSeriesBuildState &state);
    bool finish_plot_series(size_t stream_index);
    bool finish_result_plot_build();

    // Zoom range plot build
    void reset_range_plot_build(bool clear_ready);
    bool start_range_plot_build(uint64_t night_start_ms,
                                size_t therapy_index_hint,
                                int64_t from_ms,
                                int64_t to_ms,
                                bool &waiting_for_result);
    void poll_range_plot_build();
    bool ensure_range_build_buffers();
    bool read_range_chunk_payload(const ReportResultChunk &chunk,
                                  ReportStoreChunkMeta &meta,
                                  ReportSpoolBuffer &payload);
    bool for_each_range_series_sample(
        const ReportResultChunk &chunk,
        size_t stream_index,
        ReportProviderSeriesReadStats &stats,
        ReportSeriesSampleCallback callback,
        void *context);
    bool process_range_event_chunk(const ReportResultChunk &chunk);
    bool open_range_series(const ReportResultStream &stream);
    int range_plot_range_index(int64_t timestamp_ms) const;
    bool result_chunk_matches_stream(const ReportResultChunk &chunk,
                                     size_t stream_index,
                                     const ReportResultStream &stream) const;
    bool add_range_provider_chunk(const ReportProviderChunk &provider_chunk,
                                  size_t stream_index);
    bool materialize_range_plan(const ReportIndexedNight &night,
                                const ReportResolvedPlan &plan);
    bool process_range_series_sample_value(const ReportSeriesSample &sample,
                                           ReportSignalId signal,
                                           ReportSourceId source,
                                           uint32_t interval_ms,
                                           int32_t scale,
                                           bool &capped,
                                           bool &overflow);
    bool process_range_series_chunk(const ReportResultChunk &chunk);
    bool process_range_series_chunk(const ReportResultChunk &chunk,
                                    size_t stream_index);
    bool finish_range_series();
    void finish_range_plot_build();
    void fail_range_plot_build(const char *message);
    void service_range_plot(bool realtime_active);

    // Durable result and plot cache
    bool result_plot_cache_path_for_night(const ReportIndexedNight &night,
                                          const char *etag,
                                          char *path,
                                          size_t path_size) const;
    bool result_plot_cache_path_for_etag(uint64_t night_start_ms,
                                         const char *etag,
                                         char *path,
                                         size_t path_size) const;
    bool result_json_cache_path_for_night(const ReportIndexedNight &night,
                                          const char *etag,
                                          char *path,
                                          size_t path_size) const;
    bool result_json_cache_path_for_etag(uint64_t night_start_ms,
                                         const char *etag,
                                         char *path,
                                         size_t path_size) const;
    bool result_plot_cache_path(char *path, size_t path_size) const;
    bool result_plot_cache_exists_for_night(const ReportIndexedNight &night,
                                            const char *etag) const;
    bool clear_plot_cache_for_night(const ReportSummaryRecord &night,
                                    uint32_t &deleted) const;
    bool clear_result_json_cache_for_night(const ReportSummaryRecord &night,
                                           uint32_t &deleted) const;
    bool load_result_json_cache_for_night(const ReportIndexedNight &night,
                                          const char *etag,
                                          LargeTextBuffer &out) const;
    bool load_result_json_cache_path(const char *path,
                                     LargeTextBuffer &out) const;
    bool load_result_plot_cache_for_night(const ReportIndexedNight &night,
                                          const char *etag,
                                          ReportSpoolBuffer &out) const;
    bool load_result_plot_cache_for_etag(uint64_t night_start_ms,
                                         const char *etag,
                                         ReportSpoolBuffer &out) const;
    bool load_result_plot_cache_path(const char *path,
                                     ReportSpoolBuffer &out) const;
    bool load_result_plot_cache();
    bool enqueue_result_cache_write(
        const ReportIndexedNight &night,
        const char *etag,
        const std::shared_ptr<ReportSpoolBuffer> &result_json,
        const std::shared_ptr<ReportSpoolBuffer> &plot);
    bool service_result_cache_writer();
    bool service_durable_night_index_writer();
    void reset_result_cache_write_locked();

    // Build queue and idle prebuild
    BuildQueueResult enqueue_build(uint64_t night_start_ms,
                                   size_t therapy_index,
                                   bool refresh,
                                   bool idle_prebuild = false);
    bool build_queue_has_capacity() const;
    void clear_build_queue(uint64_t night_start_ms, bool all);
    void service_build_queue(bool realtime_active);
    bool plot_cache_writer_active() const;
    bool idle_prebuild_gate_open(const char **reason = nullptr) const;
    bool plot_prebuild_key_matches(uint32_t summary_revision,
                                   bool catalog_present,
                                   uint8_t catalog_state,
                                   uint32_t catalog_refresh_id) const;
    void set_plot_prebuild_key(uint32_t summary_revision,
                               bool catalog_present,
                               uint8_t catalog_state,
                               uint32_t catalog_refresh_id);

    // Summary and night index state
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
    mutable SemaphoreHandle_t durable_index_lock_ = nullptr;
    mutable ReportIndexedNight *durable_index_ = nullptr;
    mutable size_t durable_index_count_ = 0;
    mutable bool durable_index_valid_ = false;
    mutable uint32_t durable_index_crc_ = 0;
    mutable ReportIndexedNight *durable_index_save_ = nullptr;
    mutable size_t durable_index_save_count_ = 0;
    mutable uint32_t durable_index_save_crc_ = 0;
    mutable bool durable_index_save_pending_ = false;
    mutable uint32_t durable_index_save_requested_ms_ = 0;

    // Spool summary and cache fetch state
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
    int64_t *cache_source_night_extent_ms_ = nullptr;  // PSRAM sidecar

    // Write-side coalescing: buffer parsed chunks per (kind,name) and flush
    // larger files, so a night reads back as a few files instead of hundreds.
    CacheCoalesceBuffer *cache_coalesce_ = nullptr;  // PSRAM slots
    CacheWriteQueueSlot *cache_write_queue_ = nullptr;  // PSRAM slots
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
    NightEpoch *night_epochs_ = nullptr;  // PSRAM, AC_REPORT_SUMMARY_RECORD_MAX
    size_t night_epoch_count_ = 0;
    uint32_t next_trash_cleanup_ms_ = 0;

    // Prefetch state
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
    SparseEventEmptyMarker sparse_event_empty_[4] = {};

    // Result build scratch
    ReportResultChunk *result_chunks_ = nullptr;
    size_t result_chunk_capacity_ = 0;
    ReportResultStream result_streams_[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t result_stream_count_ = 0;
    EdfReportCatalogJob *edf_catalog_ = nullptr;
    uint32_t edf_catalog_summary_refresh_id_ = 0;
    EdfReportSessionDescriptor *result_edf_sessions_ = nullptr;
    size_t result_edf_session_count_ = 0;
    ReportResolvedPlan *result_resolved_plan_ = nullptr;
    ReportResolveScratch *result_resolve_scratch_ = nullptr;
    ReportIndexedNight *prepare_indexed_night_ = nullptr;
    ReportIndexedNight *range_indexed_night_ = nullptr;

    mutable SemaphoreHandle_t index_cache_lock_ = nullptr;
    mutable ReportIndexedNight *index_cache_ = nullptr;
    mutable size_t index_cache_count_ = 0;
    mutable bool index_cache_valid_ = false;
    mutable uint32_t index_cache_summary_revision_ = 0;
    mutable bool index_cache_catalog_present_ = false;
    mutable uint8_t index_cache_catalog_state_ = 0;
    mutable uint32_t index_cache_catalog_refresh_id_ = 0;

    // Materialized result and range LRU
    // Served-result LRU. The result_* slot above is build SCRATCH only; GET-by-
    // index serves these immutable per-night entries, so two clients on different
    // nights never clobber each other. Published atomically at build finish.
    MaterializedResult *result_slots_ = nullptr;  // PSRAM, AC_REPORT_RESULT_SLOT_MAX
    SemaphoreHandle_t result_slots_lock_ = nullptr;

    // Zoom range plot: request set by the web thread, built on the main loop,
    // result held for serving. All guarded by result_slots_lock_
    bool range_req_active_ = false;
    size_t range_req_index_ = 0;
    uint64_t range_req_night_start_ms_ = 0;
    int64_t range_req_from_ = 0;
    int64_t range_req_to_ = 0;
    std::shared_ptr<ReportSpoolBuffer> range_plot_bytes_;
    size_t range_plot_index_ = 0;
    uint64_t range_plot_night_start_ms_ = 0;
    int64_t range_plot_from_ = 0;
    int64_t range_plot_to_ = 0;
    bool range_build_active_ = false;
    ReportPlotBuildPhase range_build_phase_ = ReportPlotBuildPhase::Idle;
    size_t range_build_index_ = 0;
    int64_t range_build_from_ = 0;
    int64_t range_build_to_ = 0;
    uint64_t range_night_start_ms_ = 0;
    ReportResultChunk *range_chunks_ = nullptr;
    size_t range_chunk_count_ = 0;
    ReportResultStream range_streams_[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t range_stream_count_ = 0;
    EdfReportSessionDescriptor *range_edf_sessions_ = nullptr;
    size_t range_edf_session_count_ = 0;
    PlotRange range_ranges_[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t range_range_count_ = 0;
    std::shared_ptr<ReportSpoolBuffer> range_build_bytes_;
    ReportSpoolBuffer range_tmp_;
    ReportSpoolBuffer range_seen_events_;
    uint32_t range_event_count_ = 0;
    uint32_t range_chunk_index_ = 0;
    size_t range_stream_index_ = 0;
    bool range_series_open_ = false;
    uint32_t range_series_points_ = 0;
    bool range_have_last_sample_ = false;
    int64_t range_last_sample_ms_ = 0;
    int range_last_range_index_ = -1;
    int64_t range_bucket_ms_ = 1;
    int64_t range_current_bucket_ = -1;
    PlotBuildBucket range_bucket_;
    bool range_build_ok_ = true;
    uint32_t range_build_started_ms_ = 0;
    uint32_t range_build_input_chunks_ = 0;
    uint32_t range_build_input_bytes_ = 0;

    // Durable cache writer
    SemaphoreHandle_t plot_cache_write_lock_ = nullptr;
    ResultCacheWriteJob plot_cache_write_;

    // Result serving helpers
    uint32_t result_slot_tick_ = 0;

    // Build queue and idle prebuild
    // Bounded dedup build queue: a GET-miss (or POST refresh) enqueues a night to
    // build; the main loop services one at a time, replacing the single pending slot.
    ResultBuildJob build_queue_[AC_REPORT_BUILD_QUEUE_MAX];
    size_t build_queue_head_ = 0;
    size_t build_queue_count_ = 0;
    uint32_t build_queue_enqueue_total_ = 0;
    uint32_t build_queue_queued_total_ = 0;
    uint32_t build_queue_already_total_ = 0;
    uint32_t build_queue_service_total_ = 0;
    uint64_t build_queue_last_enqueue_night_ms_ = 0;
    size_t build_queue_last_enqueue_therapy_index_ = 0;
    char build_queue_last_read_[24] = {};
    char build_queue_last_enqueue_result_[24] = {};
    char build_queue_last_service_block_[24] = {};
    uint64_t build_queue_last_night_ms_ = 0;
    size_t build_queue_last_therapy_index_ = 0;
    char build_queue_last_outcome_[16] = {};
    char build_queue_last_state_[16] = {};
    char build_queue_last_error_[48] = {};
    SemaphoreHandle_t build_queue_lock_ = nullptr;

    // Active result and plot build state
    ReportIndexedNight result_indexed_night_;
    ReportSummaryRecord result_night_;
    char result_etag_[AC_REPORT_RESULT_ETAG_MAX] = {};
    PlotRange result_ranges_[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t result_range_count_ = 0;
    ReportResultStatus result_status_;
    ReportSpoolBuffer result_plot_bin_;
    ReportSpoolBuffer plot_build_bin_;
    ReportSpoolBuffer plot_tmp_;
    ReportSpoolBuffer plot_seen_events_;
    bool result_skip_plot_cache_ = false;
    bool active_build_idle_prebuild_ = false;

    bool plot_bin_ok_ = true;
    bool plot_build_active_ = false;
    bool plot_build_idle_prebuild_ = false;
    std::atomic<uint64_t> plot_build_night_start_ms_{0};
    ReportPlotBuildPhase plot_build_phase_ = ReportPlotBuildPhase::Idle;
    PlotRange plot_ranges_[AC_REPORT_SUMMARY_SESSION_MAX] = {};
    size_t plot_range_count_ = 0;
    int64_t plot_start_ms_ = 0;
    int64_t plot_end_ms_ = 0;
    int64_t plot_bucket_ms_ = 1;
    uint32_t plot_chunk_index_ = 0;
    bool plot_chunk_done_[AC_REPORT_RESULT_CHUNK_MAX] = {};
    PlotSeriesBuildState plot_series_states_[AC_REPORT_RESULT_STREAM_MAX];
    uint32_t plot_build_started_ms_ = 0;
    uint32_t plot_build_input_chunks_ = 0;
    uint32_t plot_build_input_bytes_ = 0;

    bool plot_prebuild_key_valid_ = false;
    uint32_t plot_prebuild_summary_revision_ = 0;
    bool plot_prebuild_catalog_present_ = false;
    uint8_t plot_prebuild_catalog_state_ = 0;
    uint32_t plot_prebuild_catalog_refresh_id_ = 0;
    size_t plot_prebuild_cursor_ = 0;
    uint32_t plot_prebuild_next_scan_ms_ = 0;
};

}  // namespace aircannect
