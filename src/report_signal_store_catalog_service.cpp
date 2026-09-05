#include "report_signal_store_catalog_service.h"

#include <new>
#include <utility>

#include "large_scratch_array.h"
#include "report_signal_store.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr size_t MAXIMUM_METADATA_BYTES = 64 * 1024;

}  // namespace

struct ReportSignalStoreCatalogLoadService::Runtime {
    std::shared_ptr<const NightCatalog> source;
    std::unique_ptr<LargeScratchArray<ReportSignalStoreCatalogInput>> inputs;
    std::shared_ptr<const ReportSignalStoreCatalog> completed;
    size_t cursor = 0;
};

ReportSignalStoreCatalogLoadService::~ReportSignalStoreCatalogLoadService() {
    cancel();
    delete runtime_;
}

void ReportSignalStoreCatalogLoadService::begin(StorageReadPort &read_port) {
    loader_.begin(read_port);
    if (!runtime_) runtime_ = new (std::nothrow) Runtime();
}

OperationAdmission ReportSignalStoreCatalogLoadService::start(
    std::shared_ptr<const NightCatalog> source,
    uint32_t generation) {
    if (!runtime_ || status_.active()) return OperationAdmission::Busy;
    if (!source || generation == 0) return OperationAdmission::Rejected;

    reset();
    runtime_->source = std::move(source);
    runtime_->inputs = std::make_unique<
        LargeScratchArray<ReportSignalStoreCatalogInput>>();
    if (!runtime_->inputs ||
        !runtime_->inputs->allocate(runtime_->source->size())) {
        fail("report_store_catalog_allocation_failed");
        return OperationAdmission::Rejected;
    }

    status_.state = ReportSignalStoreCatalogLoadState::Loading;
    status_.generation = generation;
    if (runtime_->source->size() == 0) {
        return finish_catalog() ? OperationAdmission::Accepted
                                : OperationAdmission::Rejected;
    }
    return OperationAdmission::Accepted;
}

bool ReportSignalStoreCatalogLoadService::start_current() {
    if (!runtime_ || !runtime_->source ||
        runtime_->cursor >= runtime_->source->size()) {
        return finish_catalog();
    }

    const NightCatalogRecord *night =
        runtime_->source->record(runtime_->cursor);
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!night || !night->sleep_day.valid() ||
        !report_signal_store_night_path(
            night->sleep_day, path, sizeof(path))) {
        ++runtime_->cursor;
        ++status_.nights_checked;
        ++status_.nights_skipped;
        return true;
    }

    const OperationAdmission admitted = loader_.start(
        path,
        MAXIMUM_METADATA_BYTES,
        status_.generation,
        StorageReadLane::Maintenance);
    if (admitted == OperationAdmission::Busy) return false;
    if (admitted == OperationAdmission::Rejected) {
        fail("report_store_catalog_read_rejected");
    }
    return true;
}

bool ReportSignalStoreCatalogLoadService::finish_current() {
    if (!runtime_ || !runtime_->source ||
        runtime_->cursor >= runtime_->source->size()) {
        fail("report_store_catalog_state_invalid");
        return true;
    }

    const StorageBoundedFileLoadStatus load = loader_.status();
    if (!load.terminal()) return false;

    const NightCatalogRecord *night =
        runtime_->source->record(runtime_->cursor);
    if (load.state == StorageBoundedFileLoadState::Ready) {
        std::shared_ptr<const LargeByteBuffer> metadata =
            loader_.take_completed();
        ReportSignalStoreNightView view;
        if (night && metadata &&
            ReportSignalStoreNightCodec::decode(
                metadata->data(), metadata->size(), view) &&
            view.night.sleep_day == night->sleep_day) {
            // Keep the prior generation available when sources have advanced.
            ReportSignalStoreCatalogInput *input =
                runtime_->inputs->append();
            if (!input) {
                fail("report_store_catalog_capacity_exceeded");
                return true;
            }
            input->metadata = std::move(metadata);
            ++status_.nights_loaded;
        } else {
            ++status_.nights_skipped;
        }
    } else if (load.state == StorageBoundedFileLoadState::Missing) {
        ++status_.nights_skipped;
        loader_.reset();
    } else if (load.state == StorageBoundedFileLoadState::Failed) {
        fail(load.error[0]
                 ? load.error
                 : "report_store_catalog_read_failed");
        return true;
    } else if (load.state == StorageBoundedFileLoadState::Cancelled) {
        status_.state = ReportSignalStoreCatalogLoadState::Cancelled;
        return true;
    }

    ++runtime_->cursor;
    ++status_.nights_checked;
    if (runtime_->cursor >= runtime_->source->size()) {
        return finish_catalog();
    }
    return true;
}

bool ReportSignalStoreCatalogLoadService::finish_catalog() {
    if (!runtime_ || !runtime_->inputs) {
        fail("report_store_catalog_state_invalid");
        return false;
    }

    runtime_->completed = ReportSignalStoreCatalogBuilder::build(
        runtime_->inputs->data(), runtime_->inputs->size());
    if (!runtime_->completed) {
        fail("report_store_catalog_build_failed");
        return false;
    }

    status_.state = ReportSignalStoreCatalogLoadState::Ready;
    status_.error[0] = '\0';
    return true;
}

bool ReportSignalStoreCatalogLoadService::poll() {
    if (!status_.active() || !runtime_) return false;

    const StorageBoundedFileLoadStatus load = loader_.status();
    if (load.state == StorageBoundedFileLoadState::Idle) {
        return start_current();
    }
    if (load.active()) return loader_.poll();
    return finish_current();
}

void ReportSignalStoreCatalogLoadService::fail(const char *error) {
    char failure[AC_STORAGE_ERROR_MAX] = {};
    copy_cstr(failure, sizeof(failure), error);

    loader_.reset();
    status_.state = ReportSignalStoreCatalogLoadState::Failed;
    copy_cstr(status_.error, sizeof(status_.error), failure);
}

void ReportSignalStoreCatalogLoadService::cancel() {
    if (!status_.active()) return;

    loader_.cancel();
    status_.state = ReportSignalStoreCatalogLoadState::Cancelled;
}

void ReportSignalStoreCatalogLoadService::reset() {
    loader_.reset();
    if (runtime_) *runtime_ = {};
    status_ = {};
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportSignalStoreCatalogLoadService::take_completed() {
    if (!runtime_ ||
        status_.state != ReportSignalStoreCatalogLoadState::Ready) {
        return {};
    }

    std::shared_ptr<const ReportSignalStoreCatalog> completed =
        std::move(runtime_->completed);
    runtime_->source.reset();
    runtime_->inputs.reset();
    runtime_->cursor = 0;
    status_ = {};
    return completed;
}

}  // namespace aircannect
