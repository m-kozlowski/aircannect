#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_data_provider.h"

namespace aircannect {

class EdfReportProvider {
public:
    const char *name() const { return "edf"; }
    ReportProviderId provider_id() const { return ReportProviderId::Edf; }

    bool for_each_event_chunk(const EdfReportSessionDescriptor *sessions,
                              size_t session_count,
                              int64_t range_start_ms,
                              int64_t range_end_ms,
                              ReportProviderChunkCallback callback,
                              void *context) const;

    bool for_each_signal_chunk(const EdfReportSessionDescriptor *sessions,
                               size_t session_count,
                               ReportSignalId signal,
                               int64_t range_start_ms,
                               int64_t range_end_ms,
                               ReportProviderChunkCallback callback,
                               void *context) const;

    bool read_chunk(const ReportProviderChunk &chunk,
                    const EdfReportSessionDescriptor *sessions,
                    size_t session_count,
                    ReportStoreChunkMeta &meta,
                    ReportSpoolBuffer &payload) const;
};

}  // namespace aircannect
