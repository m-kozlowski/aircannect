#include "edf_report_provider_cursor.h"

#include "report_legacy_storage.h"
#include <new>

#include <Arduino.h>

#include "debug_log.h"
#include "edf_report_batch_reader.h"
#include "edf_report_data_file.h"
#include "edf_report_provider_token.h"
#include "memory_manager.h"

namespace aircannect {

struct EdfReportProviderBatchCursor::Impl {
    EdfReportBatchReaderCursor reader;
    EdfReportDataPlanEntry *entries = nullptr;
    size_t entry_count = 0;
    EdfReportFileDescriptor file_desc;
    uint8_t *header = nullptr;
    size_t header_size = 0;
    ReportLegacyFile file;
    EdfReportDataReadStats stats;
    EdfReportDataReadStatus status = EdfReportDataReadStatus::Ok;
    EdfReportProviderToken seed_token;
    EdfReportDataPlanEntry seed_entry;
    uint32_t started_ms = 0;
    bool active = false;

    ~Impl() { reset(); }

    void release_io() {
        reader.reset();
        if (header) Memory::free(header);
        header = nullptr;
        header_size = 0;
        edf_report_data_close_file(file);
        if (entries) Memory::free(entries);
        entries = nullptr;
        entry_count = 0;
        active = false;
    }

    void reset() {
        release_io();
        file_desc = {};
        stats = {};
        status = EdfReportDataReadStatus::Ok;
        seed_token = {};
        seed_entry = {};
        started_ms = 0;
    }

    bool prepare_entries(const ReportProviderChunk *chunks,
                         size_t chunk_count,
                         const EdfReportSessionDescriptor *sessions,
                         size_t session_count) {
        reset();
        if (!chunks || chunk_count == 0 || !sessions || session_count == 0 ||
            chunk_count > SIZE_MAX / sizeof(EdfReportDataPlanEntry)) {
            status = EdfReportDataReadStatus::InvalidArgument;
            return false;
        }

        entries = static_cast<EdfReportDataPlanEntry *>(Memory::calloc_large(
            chunk_count, sizeof(EdfReportDataPlanEntry), false));
        if (!entries) {
            status = EdfReportDataReadStatus::RecordReadFailed;
            return false;
        }
        entry_count = chunk_count;

        if (!edf_report_provider_entry_from_chunk(chunks[0],
                                                  session_count,
                                                  seed_token,
                                                  seed_entry) ||
            seed_entry.kind != EdfReportDataKind::Series) {
            status = EdfReportDataReadStatus::InvalidArgument;
            return false;
        }

        entries[0] = seed_entry;
        for (size_t i = 1; i < chunk_count; ++i) {
            EdfReportProviderToken token;
            if (!edf_report_provider_entry_from_chunk(chunks[i],
                                                      session_count,
                                                      token,
                                                      entries[i]) ||
                entries[i].kind != EdfReportDataKind::Series ||
                token.session_index != seed_token.session_index ||
                token.file_slot != seed_token.file_slot ||
                token.file_kind != seed_token.file_kind ||
                token.data_kind != seed_token.data_kind) {
                status = EdfReportDataReadStatus::InvalidArgument;
                return false;
            }
        }

        const EdfReportSessionFileDescriptor *session_file =
            edf_report_data_entry_file(sessions[seed_token.session_index],
                                       seed_entry);
        if (!session_file) {
            status = EdfReportDataReadStatus::InvalidArgument;
            return false;
        }

        status = edf_report_data_read_header(*session_file,
                                             file_desc,
                                             header,
                                             header_size,
                                             file);
        if (status != EdfReportDataReadStatus::Ok) return false;

        started_ms = millis();
        return true;
    }

    const EdfReportSessionFileDescriptor *session_file(
        const EdfReportSessionDescriptor *sessions) const {
        if (!sessions || !entries || entry_count == 0) return nullptr;
        return edf_report_data_entry_file(sessions[seed_token.session_index],
                                          entries[0]);
    }

    void log_completion(const char *operation) const {
        Log::logf(CAT_REPORT,
                  LOG_DEBUG,
                  "EDF provider %s session=%u file_slot=%u entries=%lu "
                  "records=%lu samples=%lu emitted=%lu missing=%lu "
                  "elapsed_ms=%lu\n",
                  operation,
                  static_cast<unsigned>(seed_token.session_index),
                  static_cast<unsigned>(seed_token.file_slot),
                  static_cast<unsigned long>(entry_count),
                  static_cast<unsigned long>(stats.records_read),
                  static_cast<unsigned long>(stats.samples_seen),
                  static_cast<unsigned long>(stats.samples_emitted),
                  static_cast<unsigned long>(stats.samples_missing),
                  static_cast<unsigned long>(millis() - started_ms));
    }

    void log_failure() const {
        Log::logf(CAT_REPORT,
                  LOG_WARN,
                  "EDF provider cursor failed status=%s session=%u "
                  "file_slot=%u entries=%lu records=%lu samples=%lu "
                  "emitted=%lu missing=%lu elapsed_ms=%lu\n",
                  edf_report_data_read_status_name(status),
                  static_cast<unsigned>(seed_token.session_index),
                  static_cast<unsigned>(seed_token.file_slot),
                  static_cast<unsigned long>(entry_count),
                  static_cast<unsigned long>(stats.records_read),
                  static_cast<unsigned long>(stats.samples_seen),
                  static_cast<unsigned long>(stats.samples_emitted),
                  static_cast<unsigned long>(stats.samples_missing),
                  static_cast<unsigned long>(millis() - started_ms));
    }
};

EdfReportProviderBatchCursor::EdfReportProviderBatchCursor() = default;

EdfReportProviderBatchCursor::~EdfReportProviderBatchCursor() {
    if (!impl_) return;

    impl_->~Impl();
    Memory::free(impl_);
}

bool EdfReportProviderBatchCursor::ensure_impl() {
    if (impl_) return true;

    void *memory = Memory::alloc_large(sizeof(Impl), false);
    if (!memory) return false;

    impl_ = new (memory) Impl();
    return true;
}

bool EdfReportProviderBatchCursor::start_samples(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    EdfReportSeriesBatchSampleCallback callback,
    void *context) {
    if (!callback || !ensure_impl()) return false;
    if (!impl_->prepare_entries(chunks,
                                chunk_count,
                                sessions,
                                session_count)) {
        impl_->release_io();
        return false;
    }

    const EdfReportSessionFileDescriptor *session_file =
        impl_->session_file(sessions);
    if (!session_file ||
        !impl_->reader.start_samples(*session_file,
                                     impl_->entries,
                                     impl_->entry_count,
                                     impl_->file_desc,
                                     impl_->header,
                                     impl_->header_size,
                                     impl_->file,
                                     nullptr,
                                     impl_->stats,
                                     nullptr,
                                     callback,
                                     context)) {
        impl_->status = impl_->reader.status();
        impl_->release_io();
        return false;
    }

    impl_->active = true;
    return true;
}

bool EdfReportProviderBatchCursor::start_plot(
    const ReportProviderChunk *chunks,
    size_t chunk_count,
    const EdfReportSeriesPlotConfig *configs,
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    EdfReportSeriesBatchPlotCallback callback,
    void *context) {
    if (!configs || !callback || !ensure_impl()) return false;
    if (!impl_->prepare_entries(chunks,
                                chunk_count,
                                sessions,
                                session_count)) {
        impl_->release_io();
        return false;
    }

    const EdfReportSessionFileDescriptor *session_file =
        impl_->session_file(sessions);
    if (!session_file ||
        !impl_->reader.start_plot(*session_file,
                                  impl_->entries,
                                  impl_->entry_count,
                                  configs,
                                  impl_->file_desc,
                                  impl_->header,
                                  impl_->header_size,
                                  impl_->file,
                                  nullptr,
                                  impl_->stats,
                                  nullptr,
                                  callback,
                                  context)) {
        impl_->status = impl_->reader.status();
        impl_->release_io();
        return false;
    }

    impl_->active = true;
    return true;
}

EdfReportBatchPollResult EdfReportProviderBatchCursor::poll(
    uint32_t budget_ms) {
    if (!impl_ || !impl_->active) return EdfReportBatchPollResult::Failed;

    const EdfReportBatchPollResult result = impl_->reader.poll(budget_ms);
    if (result == EdfReportBatchPollResult::Pending) return result;

    impl_->status = impl_->reader.status();
    if (result == EdfReportBatchPollResult::Complete) {
        impl_->log_completion("cursor");
    } else {
        impl_->log_failure();
    }
    impl_->release_io();
    return result;
}

void EdfReportProviderBatchCursor::reset() {
    if (impl_) impl_->reset();
}

bool EdfReportProviderBatchCursor::active() const {
    return impl_ && impl_->active;
}

EdfReportDataReadStatus EdfReportProviderBatchCursor::status() const {
    return impl_ ? impl_->status : EdfReportDataReadStatus::InvalidArgument;
}

const EdfReportDataReadStats &EdfReportProviderBatchCursor::stats() const {
    static const EdfReportDataReadStats empty;
    return impl_ ? impl_->stats : empty;
}

}  // namespace aircannect
