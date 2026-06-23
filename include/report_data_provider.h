#pragma once

#include <stdint.h>

#include "report_sources.h"
#include "report_store.h"

namespace aircannect {

struct ReportProviderChunkExtent {
    bool found = false;
    int64_t min_start_ms = 0;
    int64_t max_end_ms = 0;
};

// Report data providers expose report-ready data to ReportManager. The current
// implementation is the AS11 spool-backed cache; EDF will add a local provider
// without making ReportManager parse EDF files.
class ReportDataProvider {
public:
    virtual ~ReportDataProvider() = default;

    virtual const char *name() const = 0;
    virtual ReportStoreChunkOrigin origin() const = 0;

    virtual bool coverage_complete(const ReportSourceDef &source,
                                   int64_t start_ms,
                                   int64_t end_ms) const = 0;

    virtual bool coverage_first_missing(const ReportSourceDef &source,
                                        int64_t start_ms,
                                        int64_t end_ms,
                                        int64_t &missing_ms) const = 0;

    virtual bool for_each_chunk(ReportStoreChunkKind kind,
                                const ReportSourceDef &source,
                                const char *name,
                                int64_t night_start_ms,
                                int64_t start_ms,
                                int64_t end_ms,
                                ReportStoreChunkCallback callback,
                                void *context) const = 0;

    virtual bool chunk_extent(ReportStoreChunkKind kind,
                              const ReportSourceDef &source,
                              const char *name,
                              int64_t night_start_ms,
                              int64_t start_ms,
                              int64_t end_ms,
                              ReportProviderChunkExtent &out) const = 0;

    virtual bool latest_cached_end(const ReportSourceDef &source,
                                   int64_t night_start_ms,
                                   int64_t start_ms,
                                   int64_t end_ms,
                                   int64_t &out_end_ms) const = 0;
};

class SpoolReportProvider final : public ReportDataProvider {
public:
    const char *name() const override { return "spool"; }
    ReportStoreChunkOrigin origin() const override {
        return ReportStoreChunkOrigin::Spool;
    }

    bool coverage_complete(const ReportSourceDef &source,
                           int64_t start_ms,
                           int64_t end_ms) const override;

    bool coverage_first_missing(const ReportSourceDef &source,
                                int64_t start_ms,
                                int64_t end_ms,
                                int64_t &missing_ms) const override;

    bool for_each_chunk(ReportStoreChunkKind kind,
                        const ReportSourceDef &source,
                        const char *name,
                        int64_t night_start_ms,
                        int64_t start_ms,
                        int64_t end_ms,
                        ReportStoreChunkCallback callback,
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
};

}  // namespace aircannect
