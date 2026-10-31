#include "edf_str_record_publisher.h"

#include "edf_storage_catalog.h"
#include "storage_path.h"
#include "string_util.h"

namespace aircannect {

bool EdfStrRecordPublisher::publish(const EdfStrSessionAccumulator &record,
                                   const EdfHeaderInfo &header,
                                   bool replace_existing) {
    if (!storage_ || pending_.full()) return false;

    Publication publication;
    publication.record = record;
    copy_cstr(publication.patient_id, sizeof(publication.patient_id), header.patient_id);
    copy_cstr(publication.recording_id, sizeof(publication.recording_id), header.recording_id);
    copy_cstr(publication.date, sizeof(publication.date), header.start_date);
    copy_cstr(publication.time, sizeof(publication.time), header.start_time);
    publication.replace_existing = replace_existing;
    if (++generation_ == 0) ++generation_;
    publication.generation = generation_;

    if (idle()) {
        active_ = publication;
        return true;
    }
    return pending_.push(publication);
}

void EdfStrRecordPublisher::poll(uint32_t now_ms) {
    if (!storage_) return;

    if (ticket_.valid()) {
        StorageEdfStrWriteCompletion completion;
        if (!storage_->take_completion(ticket_, completion)) return;

        ticket_ = {};
        if (completion.outcome.disposition == OperationDisposition::Succeeded) {
            active_ = {};
            retry_at_ms_ = 0;
            last_error_[0] = 0;
        } else {
            copy_cstr(last_error_, sizeof(last_error_),
                      completion.error[0] ? completion.error : "str_write_failed");
            retry_at_ms_ = now_ms + 1000;
        }
    }

    if (active_.generation == 0 && !pending_.pop(active_)) return;
    if (retry_at_ms_ != 0 && static_cast<int32_t>(now_ms - retry_at_ms_) < 0) return;

    char path[AC_STORAGE_WRITE_PATH_MAX] = {};
    StorageEdfStrWriteCommand command;
    if (!edf_str_path(path, sizeof(path)) ||
        !SleepDayId::from_epoch_days(active_.record.day_epoch_days(), command.changed_day)) {
        copy_cstr(last_error_, sizeof(last_error_), "str_publication_invalid");
        retry_at_ms_ = now_ms + 1000;
        return;
    }

    command.path = path;
    command.header.patient_id = active_.patient_id;
    command.header.recording_id = active_.recording_id;
    command.header.start_date = active_.date;
    command.header.start_time = active_.time;
    command.record.digital_samples = active_.record.samples();
    command.record.sample_count = active_.record.sample_count();
    command.replace_existing = active_.replace_existing;
    command.generation = active_.generation;

    const auto submission = storage_->request_write(command);
    if (submission.accepted()) {
        ticket_ = submission.ticket;
    } else {
        if (submission.admission == OperationAdmission::Rejected) {
            copy_cstr(last_error_, sizeof(last_error_), "str_write_rejected");
        }
        retry_at_ms_ = now_ms + 1000;
    }
}

}  // namespace aircannect
