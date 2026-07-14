#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "edf_report_data_reader.h"
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

    bool for_each_series_sample(const ReportProviderChunk &chunk,
                                const EdfReportSessionDescriptor *sessions,
                                size_t session_count,
                                ReportProviderSeriesReadStats &stats,
                                ReportSeriesSampleCallback callback,
                                void *context) const;

};

class EdfReportDataProvider final : public ReportDataProvider {
public:
    EdfReportDataProvider(const EdfReportSessionDescriptor *sessions,
                          size_t session_count)
        : sessions_(sessions), session_count_(session_count) {}

    const char *name() const override { return "edf"; }
    ReportProviderId provider_id() const override {
        return ReportProviderId::Edf;
    }

    bool coverage_complete(const ReportSourceDef &source,
                           int64_t start_ms,
                           int64_t end_ms) const override;

    bool coverage_first_missing(const ReportSourceDef &source,
                                int64_t start_ms,
                                int64_t end_ms,
                                int64_t &missing_ms) const override;

    bool signal_coverage_complete(const ReportSourceDef &source,
                                  ReportSignalId signal,
                                  int64_t start_ms,
                                  int64_t end_ms) const override;

    bool for_each_chunk(ReportStoreChunkKind kind,
                        const ReportSourceDef &source,
                        ReportSignalId signal,
                        const char *name,
                        int64_t night_start_ms,
                        int64_t start_ms,
                        int64_t end_ms,
                        ReportProviderChunkCallback callback,
                        void *context) const override;

    bool read_chunk(const ReportProviderChunk &chunk,
                    int64_t night_start_ms,
                    ReportStoreChunkMeta &meta,
                    ReportSpoolBuffer &payload) const override;

    bool for_each_series_sample(const ReportProviderChunk &chunk,
                                int64_t night_start_ms,
                                ReportProviderSeriesReadStats &stats,
                                ReportSeriesSampleCallback callback,
                                void *context) const override;

    bool chunk_extent(ReportStoreChunkKind kind,
                      const ReportSourceDef &source,
                      const char *name,
                      int64_t night_start_ms,
                      int64_t start_ms,
                      int64_t end_ms,
                      ReportProviderChunkExtent &out) const override;

    bool latest_cached_end(const ReportSourceDef &source,
                           int64_t night_start_ms,
                           int64_t start_ms,
                           int64_t end_ms,
                           int64_t &out_end_ms) const override;

private:
    const EdfReportSessionDescriptor *sessions_ = nullptr;
    size_t session_count_ = 0;
};

}  // namespace aircannect
