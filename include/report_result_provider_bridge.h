#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_data_provider.h"
#include "report_manager_internal_types.h"
#include "report_result_types.h"
#include "report_spool_types.h"
#include "report_store.h"

namespace aircannect {

void report_provider_chunk_from_result(
    const report_manager_internal::ReportResultChunk &chunk,
    ReportProviderChunk &out);

bool report_result_chunk_has_stream(
    const report_manager_internal::ReportResultChunk &chunk,
    size_t stream_index);

bool report_result_chunk_same_physical_edf(
    const report_manager_internal::ReportResultChunk &existing,
    const ReportProviderChunk &candidate);

bool report_result_chunk_matches_stream(
    const report_manager_internal::ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream &stream);

bool report_provider_chunk_from_result_stream(
    const report_manager_internal::ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportProviderChunk &out);

bool report_read_result_chunk_payload(
    const report_manager_internal::ReportResultChunk &chunk,
    int64_t night_start_ms,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    ReportStoreChunkMeta &meta,
    ReportSpoolBuffer &payload);

bool report_for_each_result_series_sample(
    const report_manager_internal::ReportResultChunk &chunk,
    size_t stream_index,
    const ReportResultStream *streams,
    size_t stream_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    int64_t night_start_ms,
    ReportProviderSeriesReadStats &stats,
    ReportSeriesSampleCallback callback,
    void *context);

}  // namespace aircannect
