#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include "report_daily_metrics.h"
#include "report_sources.h"
#include "report_store.h"
#include "spool_client_status.h"

namespace aircannect {

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

enum class ReportResultState : uint8_t {
    Idle,
    Preparing,
    Ready,
    Incomplete,
    Partial,  // displayable best-effort; required coverage is incomplete
    Error,
};

inline ReportResultState report_result_settled_state(uint32_t missing_required) {
    return missing_required == 0 ? ReportResultState::Ready
                                 : ReportResultState::Partial;
}

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
    bool events_available = false;  // counts are real, not unknown

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

}  // namespace aircannect
