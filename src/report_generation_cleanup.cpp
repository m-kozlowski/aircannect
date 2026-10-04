#include "report_generation_cleanup.h"

#include <algorithm>
#include <cstring>

#ifdef ARDUINO
#include "debug_log.h"
#endif

namespace aircannect {
namespace {

// Accept only a direct generation directory from the storage inventory.
bool generation_directory(const StorageScanEntryView &entry,
                          const char *base, uint32_t &generation) {
    if (!entry.directory || !entry.path) return false;
    const size_t length = strlen(base);
    if (strncmp(entry.path, base, length) != 0 ||
        entry.path[length] != '/') return false;
    const char *name = entry.path + length + 1;
    if (strlen(name) != 9 || name[0] != 'g') return false;

    generation = 0;
    for (size_t i = 1; i < 9; ++i) {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
        generation = (generation << 4) | (c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return generation != 0;
}

}  // namespace

ReportGenerationCleanup::~ReportGenerationCleanup() {
    if (delete_id_) deletes_->cancel(delete_id_);
    if (scan_ticket_.valid()) scans_->abandon(scan_ticket_);
}

void ReportGenerationCleanup::begin(StorageReadPort &reads,
                                    StorageScanPort &scans,
                                    StorageDeletePort &deletes) {
    metadata_.begin(reads);
    scans_ = &scans;
    deletes_ = &deletes;
}

void ReportGenerationCleanup::publish(
    std::shared_ptr<const ReportSignalStoreCatalog> catalog) {
    if (catalog == catalog_) return;

    release_readers();
    try {
        if (catalog_) retired_.push_back(catalog_);
        for (size_t i = 0; catalog && i < catalog->size(); ++i) {
            const auto &next = *catalog->record(i);
            const auto *previous = catalog_ ? catalog_->find(next.sleep_day) : nullptr;
            if (!previous || previous->generation != next.generation) {
                pending_.push_back(next.sleep_day);
            }
        }
    } catch (const std::bad_alloc &) {
        // Losing reader tracking must never make a directory eligible.
        tracking_failed_ = true;
        finish_night("reader_tracking_alloc_failed");
    }
    catalog_ = std::move(catalog);
    restart_ = true;
}

void ReportGenerationCleanup::release_readers() {
    const size_t count = retired_.size();
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
        [](const Retired &value) { return value.expired(); }), retired_.end());
    if (count == retired_.size() || deferred_.empty()) return;

    try {
        pending_.insert(pending_.end(), deferred_.begin(), deferred_.end());
        deferred_.clear();
    } catch (const std::bad_alloc &) {
        tracking_failed_ = true;
        finish_night("reader_tracking_alloc_failed");
    }
}

bool ReportGenerationCleanup::protected_generation(uint32_t generation) const {
    const auto protects = [&](const ReportSignalStoreCatalog &catalog) {
        const auto *record = catalog.find(day_);
        return record && record->generation == generation;
    };
    if (generation == disk_generation_ || (catalog_ && protects(*catalog_))) {
        return true;
    }
    for (const auto &weak : retired_) {
        if (const auto catalog = weak.lock()) {
            if (protects(*catalog)) return true;
        }
    }
    return false;
}

bool ReportGenerationCleanup::discard_scan() {
    if (scan_ticket_.valid()) {
        if (!scans_->abandon(scan_ticket_)) return false;
        scan_ticket_ = {};
    }
    entries_.reset();
    entry_cursor_ = 0;
    metadata_.reset();
    disk_generation_ = 0;
    day_ = {};
    waiting_for_readers_ = false;
    return true;
}

void ReportGenerationCleanup::finish_night(const char *error) {
#ifdef ARDUINO
    if (error) {
        char day[9] = {};
        day_.format_yyyymmdd(day, sizeof(day));
        Log::logf(CAT_REPORT, LOG_WARN, "generation cleanup skipped night=%s error=%s",
                  day, error);
    }
#else
    (void)error;
#endif
    if (waiting_for_readers_ && !tracking_failed_) {
        try {
            deferred_.push_back(day_);
        } catch (const std::bad_alloc &) {
            // Keep the work pending; it can be retried without new allocation.
            restart_ = true;
            return;
        }
    }
    metadata_.reset();
    entries_.reset();
    entry_cursor_ = 0;
    disk_generation_ = 0;
    day_ = {};
    waiting_for_readers_ = false;
    if (night_cursor_ < pending_.size()) ++night_cursor_;
    if (night_cursor_ == pending_.size()) {
        pending_.clear();
        night_cursor_ = 0;
    }
}

bool ReportGenerationCleanup::poll(bool allowed, uint32_t now_ms) {
    if (!scans_ || !deletes_) return false;

    if (delete_id_) {
        if (!allowed || restart_ || tracking_failed_) deletes_->cancel(delete_id_);
        StorageDeleteStatus status;
        if (!deletes_->status(status, 0) ||
            (status.id == delete_id_ && status.state == StorageDeleteState::Deleting)) {
            return false;
        }
        delete_id_ = 0;
        ++entry_cursor_;
        if (status.state == StorageDeleteState::Error) finish_night(status.error);
        if (status.state == StorageDeleteState::Cancelled) restart_ = true;
        return true;
    }

    if (!allowed || restart_ || tracking_failed_) {
        if (!discard_scan()) return false;
        restart_ = false;
        if (!allowed || tracking_failed_) return false;
    }
    if (!catalog_) return false;

    if (!day_.valid()) release_readers();
    if (tracking_failed_) return false;

    if (scan_ticket_.valid()) {
        StorageScanCompletion completion;
        if (!scans_->take_completion(scan_ticket_, completion)) return false;
        scan_ticket_ = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            !completion.snapshot) {
            finish_night(completion.error[0] ? completion.error : "scan_failed");
        } else {
            entries_ = std::move(completion.snapshot);
        }
        return true;
    }

    if (entries_) {
        if (entry_cursor_ == entries_->size()) {
            finish_night();
            return true;
        }
        StorageScanEntryView entry;
        uint32_t generation = 0;
        if (!entries_->entry(entry_cursor_, entry) ||
            !generation_directory(entry, base_path_, generation)) {
            ++entry_cursor_;
            return true;
        }
        if (protected_generation(generation)) {
            const auto *current = catalog_->find(day_);
            if (generation != disk_generation_ &&
                (!current || current->generation != generation)) {
                waiting_for_readers_ = true;
            }
            ++entry_cursor_;
            return true;
        }

        if (retry_at_ms_ && static_cast<int32_t>(now_ms - retry_at_ms_) < 0) {
            return false;
        }
        const char *name = storage_basename_from_path(entry.path);
        char error[AC_STORAGE_ERROR_MAX] = {};
        if (!deletes_->start_selected(base_path_, &name, 1, &delete_id_,
                                      error, sizeof(error))) {
            if (strstr(error, "busy") || strstr(error, "unavailable") || !*error) {
                retry_at_ms_ = now_ms + 1000;
            } else {
                finish_night(error);
            }
            return false;
        }
        retry_at_ms_ = 0;
        return true;
    }

    if (metadata_.status().active()) return metadata_.poll();
    if (metadata_.status().terminal()) {
        auto bytes = metadata_.take_completed();
        ReportSignalStoreNightView view;
        if (!bytes || !ReportSignalStoreNightCodec::decode(
                bytes->data(), bytes->size(), view) || view.night.sleep_day != day_) {
            finish_night(metadata_.status().error[0]
                ? metadata_.status().error : "metadata_unavailable_or_invalid");
            return true;
        }
        disk_generation_ = view.night.generation;
        metadata_.reset();
    }

    if (disk_generation_) {
        const StorageScanRoot root{base_path_, false};
        const auto submission = scans_->request_scan(
            {&root, 1, true, operation_generation_});
        if (submission.admission == OperationAdmission::Busy) return false;
        if (submission.admission != OperationAdmission::Accepted) {
            finish_night("scan_rejected");
        } else {
            scan_ticket_ = submission.ticket;
        }
        return true;
    }

    if (night_cursor_ >= pending_.size()) return false;
    day_ = pending_[night_cursor_];
    char path[AC_STORAGE_PATH_MAX] = {};
    report_signal_store_night_path(day_, path, sizeof(path));
    const char *slash = strrchr(path, '/');
    const size_t length = static_cast<size_t>(slash - path);
    memcpy(base_path_, path, length);
    base_path_[length] = '\0';
    if (++operation_generation_ == 0) ++operation_generation_;
    if (metadata_.start(path, ReportSignalStoreNightCodec::MaxBytes,
                        operation_generation_, StorageReadLane::Maintenance) ==
        OperationAdmission::Rejected) {
        finish_night("metadata_read_rejected");
    }
    return true;
}

}  // namespace aircannect
