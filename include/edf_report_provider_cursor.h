#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_reader.h"
#include "report_data_provider.h"

namespace aircannect {

struct EdfReportSessionDescriptor;

class EdfReportProviderBatchCursor {
public:
    EdfReportProviderBatchCursor();
    ~EdfReportProviderBatchCursor();

    EdfReportProviderBatchCursor(const EdfReportProviderBatchCursor &) = delete;
    EdfReportProviderBatchCursor &operator=(
        const EdfReportProviderBatchCursor &) = delete;

    bool start_samples(
        const ReportProviderChunk *chunks,
        size_t chunk_count,
        const EdfReportSessionDescriptor *sessions,
        size_t session_count,
        EdfReportSeriesBatchSampleCallback callback,
        void *context);

    bool start_plot(
        const ReportProviderChunk *chunks,
        size_t chunk_count,
        const EdfReportSeriesPlotConfig *configs,
        const EdfReportSessionDescriptor *sessions,
        size_t session_count,
        EdfReportSeriesBatchPlotCallback callback,
        void *context);

    EdfReportBatchPollResult poll(uint32_t budget_ms);
    void reset();

    bool active() const;
    EdfReportDataReadStatus status() const;
    const EdfReportDataReadStats &stats() const;

private:
    bool ensure_impl();

    struct Impl;
    Impl *impl_ = nullptr;
};

}  // namespace aircannect
