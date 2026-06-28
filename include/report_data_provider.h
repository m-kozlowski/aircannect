#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "report_sources.h"
#include "report_store.h"
#include "report_records.h"

namespace aircannect {

enum class ReportProviderId : uint8_t {
    Spool,
    Edf,
};

static constexpr size_t AC_REPORT_PROVIDER_TOKEN_BYTES = 96;

struct ReportProviderChunkRef {
    ReportProviderId provider = ReportProviderId::Spool;
    uint8_t data[AC_REPORT_PROVIDER_TOKEN_BYTES] = {};
};

inline bool report_provider_chunk_ref_equal(
    const ReportProviderChunkRef &a,
    const ReportProviderChunkRef &b) {
    return a.provider == b.provider &&
           memcmp(a.data, b.data, AC_REPORT_PROVIDER_TOKEN_BYTES) == 0;
}

struct ReportProviderChunk {
    ReportProviderChunkRef ref;
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

using ReportProviderChunkCallback =
    bool (*)(void *context, const ReportProviderChunk &chunk);

struct ReportProviderChunkExtent {
    bool found = false;
    int64_t min_start_ms = 0;
    int64_t max_end_ms = 0;
};

struct ReportProviderSeriesReadStats {
    uint32_t record_count = 0;
    uint32_t payload_bytes = 0;
    uint32_t interval_ms = 0;
};

// Report data providers expose report-ready chunks to ReportManager without
// making the report layer parse source-specific storage formats.
class ReportDataProvider {
public:
    virtual ~ReportDataProvider() = default;

    virtual const char *name() const = 0;
    virtual ReportProviderId provider_id() const = 0;

    virtual bool coverage_complete(const ReportSourceDef &source,
                                   int64_t start_ms,
                                   int64_t end_ms) const = 0;

    virtual bool coverage_first_missing(const ReportSourceDef &source,
                                        int64_t start_ms,
                                        int64_t end_ms,
                                        int64_t &missing_ms) const = 0;

    virtual bool for_each_chunk(ReportStoreChunkKind kind,
                                const ReportSourceDef &source,
                                ReportSignalId signal,
                                const char *name,
                                int64_t night_start_ms,
                                int64_t start_ms,
                                int64_t end_ms,
                                ReportProviderChunkCallback callback,
                                void *context) const = 0;

    virtual bool read_chunk(const ReportProviderChunk &chunk,
                            int64_t night_start_ms,
                            ReportStoreChunkMeta &meta,
                            ReportSpoolBuffer &payload) const = 0;

    virtual bool for_each_series_sample(
        const ReportProviderChunk &chunk,
        int64_t night_start_ms,
        ReportProviderSeriesReadStats &stats,
        ReportSeriesSampleCallback callback,
        void *context) const;

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
    ReportProviderId provider_id() const override {
        return ReportProviderId::Spool;
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
