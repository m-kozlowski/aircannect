#include "report_result_serving_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_log.h"
#include "board_report.h"
#include "report_index_scratch.h"
#include "report_range_tile.h"

namespace aircannect {
namespace {

const char *build_queue_result_name(uint8_t result) {
    switch (result) {
        case 0:
            return "queued";
        case 1:
            return "already";
        case 2:
            return "full";
        case 3:
            return "unavailable";
    }

    return "unknown";
}

bool parse_report_night_start_from_etag(const char *etag,
                                        uint64_t &night_start_ms) {
    if (!etag || !etag[0]) return false;

    char *end = nullptr;
    const unsigned long long parsed = strtoull(etag, &end, 10);
    if (end == etag || !end || *end != '-') return false;

    night_start_ms = static_cast<uint64_t>(parsed);
    return night_start_ms != 0;
}

}  // namespace

ReportResultServingService::ReportResultServingService(
    ReportNightIndexService &night_index,
    ReportBuildRuntime &build,
    ReportResultCacheRuntime &cache,
    ReportResultRuntime &result)
    : night_index_(night_index),
      build_(build),
      cache_(cache),
      result_(result) {}

ReportResultRead ReportResultServingService::read_result_by_start(
    uint64_t night_start_ms,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    ScopedIndexedNight indexed_night("read_result_start_index");
    size_t therapy_index = 0;
    if (!indexed_night ||
        !night_index_.by_start(night_start_ms,
                               indexed_night.get(),
                               &therapy_index)) {
        build_.note_read("not_found");
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ReportResultRead::NotFound;
    }

    return read_result_for_indexed_night(therapy_index,
                                         indexed_night.get(),
                                         if_none_match,
                                         etag_out,
                                         etag_out_size,
                                         json_out);
}

ReportResultRead ReportResultServingService::read_result_for_indexed_night(
    size_t therapy_index,
    const ReportIndexedNight &indexed_night,
    const char *if_none_match,
    char *etag_out,
    size_t etag_out_size,
    LargeTextBuffer &json_out) {
    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    night_index_.format_result_etag(indexed_night,
                                    current_etag,
                                    sizeof(current_etag));

    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }

    if (if_none_match && if_none_match[0] &&
        strcmp(if_none_match, current_etag) == 0) {
        build_.note_read("not_modified");
        return ReportResultRead::NotModified;
    }

    const ReportResultSlotRead slot_read =
        cache_.read_result(indexed_night.summary.start_ms,
                           current_etag,
                           json_out);
    if (slot_read == ReportResultSlotRead::Error) {
        build_.note_read("slot_copy_failed");
        return ReportResultRead::Unavailable;
    }
    if (slot_read != ReportResultSlotRead::NotFound) {
        build_.note_read("ready");
        return ReportResultRead::Ready;
    }

    const ReportCacheLoadRequest load_request =
        cache_.request_load(indexed_night.summary.start_ms,
                            current_etag);
    switch (load_request) {
        case ReportCacheLoadRequest::Queued:
        case ReportCacheLoadRequest::Pending:
            if (etag_out && etag_out_size) etag_out[0] = '\0';
            build_.note_read("cache_load");
            return ReportResultRead::Building;
        case ReportCacheLoadRequest::Full:
            build_.note_read("cache_load_full");
            return ReportResultRead::QueueFull;
        case ReportCacheLoadRequest::Failed:
        case ReportCacheLoadRequest::Unavailable:
            build_.note_read("cache_load_failed");
            return ReportResultRead::Unavailable;
        case ReportCacheLoadRequest::Missing:
            break;
    }

    if (indexed_night.edf_catalog_pending) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        build_.note_read("edf_catalog");
        return ReportResultRead::Building;
    }

    if (etag_out && etag_out_size) etag_out[0] = '\0';

    const ReportBuildRuntime::BuildQueueResult queued =
        build_.enqueue(indexed_night.summary.start_ms,
                       therapy_index,
                       false,
                       false);
    build_.note_read(build_queue_result_name(static_cast<uint8_t>(queued)));

    switch (queued) {
        case ReportBuildRuntime::BuildQueueResult::Queued:
        case ReportBuildRuntime::BuildQueueResult::AlreadyQueued:
            return ReportResultRead::Building;
        case ReportBuildRuntime::BuildQueueResult::Full:
            return ReportResultRead::QueueFull;
        case ReportBuildRuntime::BuildQueueResult::Unavailable:
        default:
            return ReportResultRead::Unavailable;
    }
}

ReportPlotRead ReportResultServingService::read_plot(
    size_t therapy_index,
    const char *version,
    char *etag_out,
    size_t etag_out_size,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    ScopedIndexedNight indexed_night("read_plot_index");
    if (!indexed_night) return ReportPlotRead::Unavailable;

    size_t resolved_therapy_index = therapy_index;
    if (!resolve_plot_night(therapy_index,
                            version,
                            indexed_night.get(),
                            resolved_therapy_index,
                            etag_out,
                            etag_out_size)) {
        return ReportPlotRead::NotFound;
    }

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    night_index_.format_result_etag(indexed_night.get(),
                                    current_etag,
                                    sizeof(current_etag));
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return ReportPlotRead::Stale;
    }

    bool matching_result_without_plot = false;
    switch (cache_.read_plot(indexed_night->summary.start_ms,
                             current_etag,
                             out)) {
        case ReportCachedPlotRead::Ready:
            return ReportPlotRead::Ready;
        case ReportCachedPlotRead::Error:
            return ReportPlotRead::Error;
        case ReportCachedPlotRead::ResultWithoutPlot:
            matching_result_without_plot = true;
            break;
        case ReportCachedPlotRead::NotFound:
        default:
            break;
    }

    const ReportCacheLoadRequest load_request =
        cache_.request_load(indexed_night->summary.start_ms,
                            current_etag);
    switch (load_request) {
        case ReportCacheLoadRequest::Queued:
        case ReportCacheLoadRequest::Pending:
            return ReportPlotRead::Building;
        case ReportCacheLoadRequest::Full:
            return ReportPlotRead::QueueFull;
        case ReportCacheLoadRequest::Failed:
        case ReportCacheLoadRequest::Unavailable:
            return ReportPlotRead::Unavailable;
        case ReportCacheLoadRequest::Missing:
            break;
    }

    if (indexed_night->edf_catalog_pending) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ReportPlotRead::Building;
    }

    if (matching_result_without_plot &&
        result_.plot().night_start_ms.load() ==
            indexed_night->summary.start_ms) {
        return ReportPlotRead::Building;
    }

    switch (build_.enqueue(indexed_night->summary.start_ms,
                           resolved_therapy_index,
                           false,
                           false)) {
        case ReportBuildRuntime::BuildQueueResult::Queued:
        case ReportBuildRuntime::BuildQueueResult::AlreadyQueued:
            return ReportPlotRead::Building;
        case ReportBuildRuntime::BuildQueueResult::Full:
            return ReportPlotRead::QueueFull;
        case ReportBuildRuntime::BuildQueueResult::Unavailable:
        default:
            return ReportPlotRead::Unavailable;
    }
}

ReportPlotRead ReportResultServingService::read_plot_range(
    size_t therapy_index,
    const char *version,
    char *etag_out,
    size_t etag_out_size,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (etag_out && etag_out_size) etag_out[0] = '\0';

    int64_t normalized_from_ms = 0;
    int64_t normalized_to_ms = 0;
    if (!normalize_report_range_tiles(from_ms,
                                      to_ms,
                                      AC_REPORT_RANGE_TILE_MS,
                                      AC_REPORT_RANGE_PLOT_MAX_WINDOW_MS,
                                      normalized_from_ms,
                                      normalized_to_ms)) {
        return ReportPlotRead::NotFound;
    }

    ScopedIndexedNight indexed_night("read_plot_range_index");
    if (!indexed_night) return ReportPlotRead::Unavailable;

    size_t resolved_therapy_index = therapy_index;
    if (!resolve_plot_night(therapy_index,
                            version,
                            indexed_night.get(),
                            resolved_therapy_index,
                            etag_out,
                            etag_out_size)) {
        return ReportPlotRead::NotFound;
    }

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    night_index_.format_result_etag(indexed_night.get(),
                                    current_etag,
                                    sizeof(current_etag));
    if (version && version[0] && strcmp(version, current_etag) != 0) {
        return ReportPlotRead::Stale;
    }
    if (indexed_night->edf_catalog_pending) {
        if (etag_out && etag_out_size) etag_out[0] = '\0';
        return ReportPlotRead::Building;
    }

    const uint64_t night_start_ms = indexed_night->summary.start_ms;
    switch (cache_.read_or_request_range(resolved_therapy_index,
                                         night_start_ms,
                                         current_etag,
                                         normalized_from_ms,
                                         normalized_to_ms,
                                         out)) {
        case ReportRangePlotRead::Ready:
            return ReportPlotRead::Ready;
        case ReportRangePlotRead::Empty:
            return ReportPlotRead::Empty;
        case ReportRangePlotRead::Error:
            return ReportPlotRead::Error;
        case ReportRangePlotRead::Building:
        default:
            return ReportPlotRead::Building;
    }
}

bool ReportResultServingService::resolve_plot_night(
    size_t therapy_index,
    const char *version,
    ReportIndexedNight &indexed_night,
    size_t &resolved_therapy_index,
    char *etag_out,
    size_t etag_out_size) {
    if (etag_out && etag_out_size) etag_out[0] = '\0';

    uint64_t version_night_start_ms = 0;
    const bool have_version_start =
        parse_report_night_start_from_etag(version, version_night_start_ms);
    const bool found_night =
        have_version_start
            ? night_index_.by_start(version_night_start_ms,
                                    indexed_night,
                                    &resolved_therapy_index)
            : night_index_.by_therapy_index(therapy_index, indexed_night);
    if (!found_night) return false;

    char current_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    night_index_.format_result_etag(indexed_night,
                                    current_etag,
                                    sizeof(current_etag));
    if (etag_out && etag_out_size) {
        snprintf(etag_out, etag_out_size, "%s", current_etag);
    }

    return true;
}

}  // namespace aircannect
