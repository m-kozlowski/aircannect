#pragma once

#include <FS.h>
#include <stddef.h>
#include <stdint.h>

#include "edf_report_data_reader.h"

namespace aircannect {

class EdfReportBatchReaderCursor {
public:
    ~EdfReportBatchReaderCursor();

    bool start_samples(
        const EdfReportSessionFileDescriptor &session_file,
        const EdfReportDataPlanEntry *entries,
        size_t entry_count,
        EdfReportFileDescriptor &file_desc,
        const uint8_t *header,
        size_t header_size,
        File &file,
        ReportStoreChunkMeta *metas,
        EdfReportDataReadStats &stats,
        uint32_t *interval_ms_out,
        EdfReportSeriesBatchSampleCallback callback,
        void *context);

    bool start_plot(
        const EdfReportSessionFileDescriptor &session_file,
        const EdfReportDataPlanEntry *entries,
        size_t entry_count,
        const EdfReportSeriesPlotConfig *configs,
        EdfReportFileDescriptor &file_desc,
        const uint8_t *header,
        size_t header_size,
        File &file,
        ReportStoreChunkMeta *metas,
        EdfReportDataReadStats &stats,
        uint32_t *interval_ms_out,
        EdfReportSeriesBatchPlotCallback callback,
        void *context);

    EdfReportBatchPollResult poll(uint32_t budget_ms);
    void reset();

    bool active() const { return state_ == State::Active; }
    EdfReportDataReadStatus status() const { return status_; }

private:
    enum class Mode : uint8_t {
        None,
        Samples,
        Plot,
    };

    enum class State : uint8_t {
        Idle,
        Active,
        Complete,
        Failed,
    };

    bool start_common(const EdfReportSessionFileDescriptor &session_file,
                      const EdfReportDataPlanEntry *entries,
                      size_t entry_count,
                      EdfReportFileDescriptor &file_desc,
                      File &file,
                      EdfReportDataReadStats &stats);
    bool prepare_sample_items(ReportStoreChunkMeta *metas,
                              uint32_t *interval_ms_out,
                              EdfReportSeriesBatchSampleCallback callback,
                              void *context);
    bool prepare_plot_items(const EdfReportSeriesPlotConfig *configs,
                            ReportStoreChunkMeta *metas,
                            uint32_t *interval_ms_out,
                            EdfReportSeriesBatchPlotCallback callback,
                            void *context);
    bool process_record(uint32_t record_index);
    bool finish();
    void fail(EdfReportDataReadStatus status);

    Mode mode_ = Mode::None;
    State state_ = State::Idle;
    EdfReportDataReadStatus status_ = EdfReportDataReadStatus::Ok;

    const EdfReportSessionFileDescriptor *session_file_ = nullptr;
    const EdfReportDataPlanEntry *entries_ = nullptr;
    size_t entry_count_ = 0;
    EdfReportFileDescriptor *file_desc_ = nullptr;
    const uint8_t *header_ = nullptr;
    size_t header_size_ = 0;
    File *file_ = nullptr;
    EdfReportDataReadStats *stats_ = nullptr;

    void *items_ = nullptr;
    uint8_t *record_ = nullptr;
    uint32_t current_record_ = 0;
    uint32_t end_record_ = 0;
};

}  // namespace aircannect
