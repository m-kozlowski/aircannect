#include "edf_str_summary_refresh.h"

#include <limits.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "edf_bytes.h"
#include "edf_file_writer.h"
#include "edf_str_file_layout.h"
#include "edf_str_settings.h"
#include "edf_str_signal_table.h"
#include "edf_str_timeline.h"
#include "memory_manager.h"
#include "report_parser.h"

namespace aircannect {
namespace {

constexpr int64_t SUMMARY_FROM_MS = 946684800000LL;
constexpr size_t COPY_CHUNK_BYTES = 4096;
constexpr const char *STR_PATH = "/STR.edf";

bool record_has_content(const uint8_t *record) {
    if (!record) return false;

    const size_t date_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_DATE_SIGNAL);
    for (size_t i = 0; i < AC_EDF_STR_DATA_SAMPLES_PER_RECORD; ++i) {
        if (i == date_offset) continue;
        if (edf_read_i16_le_sample(record, i) != -1) return true;
    }
    return false;
}

void patch_crc(uint8_t *record) {
    const size_t crc_offset =
        edf_str_signal_sample_offset(AC_EDF_STR_CRC_SIGNAL);
    const uint16_t crc = edf_crc16_ccitt_false(record, crc_offset * 2);
    edf_write_i16_le_sample(record,
                            crc_offset,
                            static_cast<int16_t>(crc));
}

}  // namespace

struct EdfStrSummaryRefreshEntry {
    int32_t day = -1;
    int16_t samples[AC_EDF_STR_DATA_SAMPLES_PER_RECORD] = {};
    bool seen = false;
};

namespace {

struct SummaryCountContext {
    SleepDayId start_day;
    SleepDayId end_day;
    size_t count = 0;
    bool overflow = false;
};

bool count_summary_record(void *opaque, const ReportSummaryRecord &record) {
    SummaryCountContext *context =
        static_cast<SummaryCountContext *>(opaque);
    if (!context) return false;

    int32_t day = 0;
    if (!report_summary_sleep_day_epoch_days(record, day) ||
        day < context->start_day.epoch_days() ||
        day > context->end_day.epoch_days()) {
        return true;
    }

    if (context->count >= AC_EDF_STR_RECORD_LIMIT) {
        context->overflow = true;
        return false;
    }
    context->count++;
    return true;
}

struct SummaryFillContext {
    EdfStrSummaryRefreshEntry *entries = nullptr;
    size_t capacity = 0;
    size_t count = 0;
    SleepDayId start_day;
    SleepDayId end_day;
    const char *error = nullptr;
};

bool fill_summary_record(void *opaque, const ReportSummaryRecord &record) {
    SummaryFillContext *context =
        static_cast<SummaryFillContext *>(opaque);
    if (!context) return false;

    int32_t day = 0;
    if (!report_summary_sleep_day_epoch_days(record, day) ||
        day < context->start_day.epoch_days() ||
        day > context->end_day.epoch_days()) {
        return true;
    }
    if (day < 0 || day > INT16_MAX) {
        context->error = "str_refresh_day_out_of_range";
        return false;
    }

    size_t index = context->count;
    for (size_t i = 0; i < context->count; ++i) {
        if (context->entries[i].day == day) {
            index = i;
            break;
        }
    }
    if (index == context->count) {
        if (context->count >= context->capacity) {
            context->error = "str_refresh_summary_overflow";
            return false;
        }
        context->count++;
    }

    EdfStrSessionAccumulator accumulator;
    accumulator.reset_day(static_cast<uint16_t>(day), {});

    EdfStrSettingsApplyResult applied;
    if (!edf_str_apply_summary_record(record, accumulator, applied)) {
        context->error = applied.error ? applied.error
                                       : "str_refresh_summary_apply_failed";
        return false;
    }

    EdfStrSummaryRefreshEntry &entry = context->entries[index];
    entry.day = day;
    memcpy(entry.samples,
           accumulator.samples(),
           sizeof(entry.samples));
    entry.seen = false;
    return true;
}

}  // namespace

EdfStrSummaryRefresh::~EdfStrSummaryRefresh() {
    clear_work();
}

void EdfStrSummaryRefresh::begin(ReportSpoolPort &spool_port,
                                 StorageReadPort &read_port,
                                 StorageAtomicWritePort &write_port,
                                 StoragePathPort &path_port) {
    clear_work();
    spool_port_ = &spool_port;
    storage_file_.begin(read_port, write_port, path_port);
    status_ = {};
}

OperationAdmission EdfStrSummaryRefresh::request(SleepDayId start_day,
                                                 SleepDayId end_day,
                                                 uint32_t generation) {
    if (!spool_port_ || !start_day.valid() || !end_day.valid() ||
        end_day < start_day || start_day.epoch_days() < 0 ||
        end_day.epoch_days() > INT16_MAX || generation == 0) {
        return OperationAdmission::Rejected;
    }
    if (status_.active()) return OperationAdmission::Busy;

    clear_work();
    status_ = {};
    status_.generation = generation;
    status_.start_day = start_day;
    status_.end_day = end_day;

    ReportSpoolFetchCommand command;
    command.source = ReportSourceId::Summary;
    command.from_ms = SUMMARY_FROM_MS;
    command.generation = generation;

    const OperationSubmission submission = spool_port_->request_fetch(command);
    if (!submission.accepted()) return submission.admission;

    spool_ticket_ = submission.ticket;
    status_.state = EdfStrSummaryRefreshState::FetchingSummary;
    return OperationAdmission::Accepted;
}

bool EdfStrSummaryRefresh::build_summary_entries(
    const ReportSpoolResult &result) {
    char error[64] = {};
    SummaryCountContext count;
    count.start_day = status_.start_day;
    count.end_day = status_.end_day;
    if (!report_parse_summary_spool(result,
                                    count_summary_record,
                                    &count,
                                    error,
                                    sizeof(error)) ||
        count.overflow) {
        fail(count.overflow ? "str_refresh_summary_too_large"
                            : (error[0] ? error
                                        : "str_refresh_summary_parse_failed"));
        return false;
    }
    if (count.count == 0) {
        finish();
        return true;
    }

    summary_entries_ = static_cast<EdfStrSummaryRefreshEntry *>(
        Memory::alloc_large(count.count * sizeof(EdfStrSummaryRefreshEntry),
                            false));
    if (!summary_entries_) {
        fail("str_refresh_summary_alloc_failed");
        return false;
    }
    summary_capacity_ = count.count;
    for (size_t i = 0; i < summary_capacity_; ++i) {
        new (&summary_entries_[i]) EdfStrSummaryRefreshEntry();
    }

    SummaryFillContext fill;
    fill.entries = summary_entries_;
    fill.capacity = summary_capacity_;
    fill.start_day = status_.start_day;
    fill.end_day = status_.end_day;
    if (!report_parse_summary_spool(result,
                                    fill_summary_record,
                                    &fill,
                                    error,
                                    sizeof(error)) ||
        fill.error) {
        fail(fill.error ? fill.error
                        : (error[0] ? error
                                    : "str_refresh_summary_build_failed"));
        return false;
    }

    summary_count_ = fill.count;
    status_.matched = static_cast<uint32_t>(summary_count_);
    return true;
}

void EdfStrSummaryRefresh::abort(const char *error) {
    if (!status_.active()) return;
    fail(error ? error : "str_refresh_aborted");
}

bool EdfStrSummaryRefresh::begin_file_copy(StoragePreparedFile file) {
    if (!file.exists() || !file.ready()) {
        fail("str_refresh_str_missing");
        return false;
    }

    EdfStrFileLayout layout;
    if (!edf_str_file_layout_from_size(file.size(), layout) ||
        layout.record_count > AC_EDF_STR_RECORD_LIMIT) {
        fail("str_refresh_str_layout_invalid");
        return false;
    }

    file_buffer_ = LargeByteBuffer::allocate(file.size());
    if (!file_buffer_) {
        fail("str_refresh_file_alloc_failed");
        return false;
    }

    source_file_ = std::move(file);
    record_count_ = layout.record_count;
    copy_offset_ = 0;
    status_.state = EdfStrSummaryRefreshState::CopyingFile;
    return true;
}

bool EdfStrSummaryRefresh::prepare_file_update() {
    std::unique_ptr<LargeByteBuffer> expected =
        LargeByteBuffer::allocate(edf_str_header_size());
    if (!expected) {
        fail("str_refresh_header_alloc_failed");
        return false;
    }

    EdfHeaderInfo info;
    size_t written = 0;
    if (!edf_render_str_header(info,
                               expected->data(),
                               expected->size(),
                               written) ||
        written != expected->size() ||
        !edf_str_header_schema_matches(file_buffer_->data(),
                                       expected->data(),
                                       expected->size()) ||
        !edf_str_header_start_day(file_buffer_->data(),
                                  edf_str_header_size(),
                                  header_start_day_)) {
        fail("str_refresh_str_schema_invalid");
        return false;
    }

    record_index_ = 0;
    status_.state = EdfStrSummaryRefreshState::UpdatingRecords;
    return true;
}

EdfStrSummaryRefreshEntry *EdfStrSummaryRefresh::find_summary(int32_t day) {
    for (size_t i = 0; i < summary_count_; ++i) {
        if (summary_entries_[i].day == day) return &summary_entries_[i];
    }
    return nullptr;
}

void EdfStrSummaryRefresh::update_next_record() {
    if (!file_buffer_ || record_index_ >= record_count_) {
        finish_update();
        return;
    }

    uint8_t *record = file_buffer_->data() +
        edf_str_record_offset(record_index_);
    int32_t day = -1;
    if (!edf_str_timeline_record_day(
            header_start_day_,
            record_index_,
            edf_str_record_date_sample(record, edf_str_record_size()),
            day)) {
        fail("str_refresh_timeline_invalid");
        return;
    }
    record_index_++;

    EdfStrSummaryRefreshEntry *summary = find_summary(day);
    if (!summary) return;

    summary->seen = true;
    if (!record_has_content(record)) {
        status_.missing++;
        return;
    }

    bool changed = false;
    for (size_t i = 0; i < AC_EDF_STR_SOURCE_FIELD_COUNT; ++i) {
        const EdfStrSignalDescriptor *signal =
            edf_str_signal_descriptor(i);
        if (!signal || signal->source != EdfStrFieldSource::Summary) {
            continue;
        }

        const size_t offset = edf_str_signal_sample_offset(i);
        if (offset >= AC_EDF_STR_DATA_SAMPLES_PER_RECORD) {
            fail("str_refresh_summary_offset_invalid");
            return;
        }

        const int16_t incoming = summary->samples[offset];
        if (incoming == -1 ||
            edf_read_i16_le_sample(record, offset) == incoming) {
            continue;
        }
        edf_write_i16_le_sample(record, offset, incoming);
        changed = true;
    }

    if (changed) {
        patch_crc(record);
        status_.updated++;
    } else {
        status_.unchanged++;
    }
}

void EdfStrSummaryRefresh::finish_update() {
    for (size_t i = 0; i < summary_count_; ++i) {
        if (!summary_entries_[i].seen) status_.missing++;
    }
    if (status_.updated == 0) {
        finish();
        return;
    }

    output_file_ = LargeByteBuffer::freeze(std::move(file_buffer_));
    if (!output_file_) {
        fail("str_refresh_output_freeze_failed");
        return;
    }
    status_.state = EdfStrSummaryRefreshState::SubmittingWrite;
}

void EdfStrSummaryRefresh::poll() {
    if (!status_.active()) return;

    if (status_.state == EdfStrSummaryRefreshState::FetchingSummary) {
        ReportSpoolFetchRound round;
        if (spool_port_->take_round(spool_ticket_, round)) {
            round.clear();
            const OperationTicket ticket = spool_ticket_;
            (void)spool_port_->cancel(ticket);
            ReportSpoolFetchCompletion cancelled;
            (void)spool_port_->take_completion(ticket, cancelled);
            spool_ticket_ = {};
            fail("str_refresh_summary_round_unexpected");
            return;
        }

        ReportSpoolFetchCompletion completion;
        if (!spool_port_->take_completion(spool_ticket_, completion)) return;

        spool_ticket_ = {};
        if (completion.outcome.disposition !=
            OperationDisposition::Succeeded) {
            fail(completion.error[0] ? completion.error
                                     : "str_refresh_summary_fetch_failed");
            return;
        }
        if (!build_summary_entries(completion.result) ||
            status_.state == EdfStrSummaryRefreshState::Complete) {
            return;
        }
        completion.clear();
        status_.state = EdfStrSummaryRefreshState::SubmittingRead;
    }

    if (status_.state == EdfStrSummaryRefreshState::SubmittingRead) {
        const size_t max_size = edf_str_header_size() +
            AC_EDF_STR_RECORD_LIMIT * edf_str_record_size();
        const OperationAdmission admission = storage_file_.request_read(
            STR_PATH, max_size, status_.generation);
        if (admission == OperationAdmission::Busy) return;
        if (admission != OperationAdmission::Accepted) {
            fail("str_refresh_read_rejected");
            return;
        }
        status_.state = EdfStrSummaryRefreshState::ReadingFile;
    }

    if (status_.state == EdfStrSummaryRefreshState::ReadingFile) {
        const StorageFileClientResult result = storage_file_.poll();
        if (result == StorageFileClientResult::Waiting) return;
        if (result == StorageFileClientResult::Error) {
            fail(storage_file_.error());
            return;
        }
        if (result == StorageFileClientResult::Ready) {
            (void)begin_file_copy(storage_file_.take_file());
        }
        return;
    }

    if (status_.state == EdfStrSummaryRefreshState::CopyingFile) {
        const size_t remaining = source_file_.size() - copy_offset_;
        const size_t capacity = remaining < COPY_CHUNK_BYTES
            ? remaining : COPY_CHUNK_BYTES;
        const PreparedByteRead read = source_file_.read(
            copy_offset_, file_buffer_->data() + copy_offset_, capacity);
        if (read.state == PreparedByteReadState::Retry) return;
        if (read.state != PreparedByteReadState::Data || read.bytes == 0) {
            fail("str_refresh_file_copy_failed");
            return;
        }

        copy_offset_ += read.bytes;
        if (copy_offset_ < source_file_.size()) return;

        source_file_.reset();
        (void)prepare_file_update();
        return;
    }

    if (status_.state == EdfStrSummaryRefreshState::UpdatingRecords) {
        update_next_record();
        return;
    }

    if (status_.state == EdfStrSummaryRefreshState::SubmittingWrite) {
        const OperationAdmission admission = storage_file_.request_replace(
            STR_PATH, output_file_, status_.generation);
        if (admission == OperationAdmission::Busy) return;
        if (admission != OperationAdmission::Accepted) {
            fail("str_refresh_write_rejected");
            return;
        }

        output_file_.reset();
        status_.state = EdfStrSummaryRefreshState::WritingFile;
    }

    if (status_.state == EdfStrSummaryRefreshState::WritingFile) {
        const StorageFileClientResult result = storage_file_.poll();
        if (result == StorageFileClientResult::Waiting) return;
        if (result == StorageFileClientResult::Error) {
            fail(storage_file_.error());
            return;
        }
        if (result == StorageFileClientResult::Ready) finish();
    }
}

void EdfStrSummaryRefresh::finish() {
    clear_work();
    status_.state = EdfStrSummaryRefreshState::Complete;
    status_.error[0] = '\0';
}

void EdfStrSummaryRefresh::fail(const char *error) {
    clear_work();
    status_.state = EdfStrSummaryRefreshState::Failed;
    snprintf(status_.error,
             sizeof(status_.error),
             "%s",
             error && error[0] ? error : "str_refresh_failed");
}

void EdfStrSummaryRefresh::clear_work() {
    if (spool_port_ && spool_ticket_.valid()) {
        (void)spool_port_->cancel(spool_ticket_);
    }
    spool_ticket_ = {};
    storage_file_.reset();
    source_file_.reset();
    file_buffer_.reset();
    output_file_.reset();

    for (size_t i = 0; i < summary_capacity_; ++i) {
        summary_entries_[i].~EdfStrSummaryRefreshEntry();
    }
    Memory::free(summary_entries_);
    summary_entries_ = nullptr;
    summary_capacity_ = 0;
    summary_count_ = 0;

    copy_offset_ = 0;
    header_start_day_ = -1;
    record_count_ = 0;
    record_index_ = 0;
}

}  // namespace aircannect
