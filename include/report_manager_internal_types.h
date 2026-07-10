#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include <FS.h>

#include "edf_report_catalog.h"
#include "report_data_provider.h"
#include "report_manager_limits.h"
#include "report_night_index.h"
#include "report_result_types.h"
#include "report_spool_types.h"

namespace aircannect {
namespace report_manager_internal {

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
    ReportSessionRange ranges[AC_REPORT_NIGHT_SESSION_MAX] = {};
    size_t range_count = 0;
    ReportResultStream streams[AC_REPORT_RESULT_STREAM_MAX] = {};
    size_t stream_count = 0;
    ReportResultChunk chunks[AC_REPORT_RESULT_CHUNK_MAX] = {};
    size_t chunk_count = 0;
    EdfReportSessionDescriptor edf_sessions[AC_REPORT_EDF_SESSION_MAX] = {};
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

}  // namespace report_manager_internal
}  // namespace aircannect
