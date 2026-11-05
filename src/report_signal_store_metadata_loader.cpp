#include "report_signal_store_metadata_loader.h"

#include <utility>

#include "string_util.h"

namespace aircannect {

void ReportSignalStoreMetadataLoader::begin(StorageReadPort &read_port) {
    reset();
    loader_.begin(read_port);
}

OperationAdmission ReportSignalStoreMetadataLoader::start(
    SleepDayId sleep_day,
    uint32_t generation,
    StorageReadLane lane,
    bool whole_file_size_only) {
    if (status_.state != StorageBoundedFileLoadState::Idle) {
        return OperationAdmission::Busy;
    }
    if (!sleep_day.valid()) return OperationAdmission::Rejected;

    char path[AC_STORAGE_PATH_MAX] = {};
    if (!report_signal_store_night_path(sleep_day, path, sizeof(path))) {
        return OperationAdmission::Rejected;
    }

    const OperationAdmission admitted = loader_.start(
        path, ReportSignalStoreNightCodec::MaxBytes, generation, lane,
        whole_file_size_only);
    if (admitted == OperationAdmission::Accepted) {
        sleep_day_ = sleep_day;
        status_ = loader_.status();
    }
    return admitted;
}

bool ReportSignalStoreMetadataLoader::poll() {
    if (!status_.active()) return false;

    const bool worked = loader_.poll();
    status_ = loader_.status();
    if (status_.active() || status_.state != StorageBoundedFileLoadState::Ready) {
        return worked;
    }

    std::shared_ptr<const LargeByteBuffer> bytes = loader_.take_completed();
    ReportSignalStoreNightView view;
    if (!bytes || !ReportSignalStoreNightCodec::decode(
                      bytes->data(), bytes->size(), view) ||
        view.night.sleep_day != sleep_day_) {
        completed_.view = {};
        completed_.metadata.reset();
        status_.state = StorageBoundedFileLoadState::Failed;
        copy_cstr(status_.error, sizeof(status_.error),
                  "report_store_metadata_invalid");
        return true;
    }

    completed_ = ReportSignalStoreMetadata(std::move(bytes), view);
    return true;
}

void ReportSignalStoreMetadataLoader::reset() {
    loader_.reset();
    status_ = {};
    sleep_day_ = {};
    completed_.view = {};
    completed_.metadata.reset();
}

void ReportSignalStoreMetadataLoader::cancel() {
    if (status_.active()) {
        loader_.cancel();
        status_ = loader_.status();
    }
    completed_.view = {};
    completed_.metadata.reset();
}

ReportSignalStoreMetadata
ReportSignalStoreMetadataLoader::take_completed() {
    if (status_.state != StorageBoundedFileLoadState::Ready ||
        !completed_.metadata) {
        return {};
    }

    ReportSignalStoreMetadata completed = std::move(completed_);
    completed_.view = {};
    completed_.metadata.reset();
    return completed;
}

}  // namespace aircannect
