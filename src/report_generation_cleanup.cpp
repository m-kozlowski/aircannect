#include "report_generation_cleanup.h"

#include <algorithm>
#include <cstring>

#include "night_catalog_store_service.h"
#include "storage_space_policy.h"

#ifdef ARDUINO
#include "debug_log.h"
#endif

namespace aircannect {

ReportGenerationCleanup::~ReportGenerationCleanup() {
    if (delete_id_) deletes_->cancel(delete_id_);
    if (scan_ticket_.valid()) scans_->abandon(scan_ticket_);
}

void ReportGenerationCleanup::begin(StorageReadPort &reads,
                                    StorageScanPort &scans,
                                    StorageDeletePort &deletes,
                                    StorageAtomicWritePort *writes) {
    metadata_.begin(reads);
    scans_ = &scans;
    deletes_ = &deletes;
    can_publish_ = writes != nullptr;
    if (writes) publication_.begin(*writes);
}

void ReportGenerationCleanup::publish(
    std::shared_ptr<const ReportSignalStoreCatalog> catalog) {
    if (catalog == catalog_) return;

    release_readers();
    bool changed = !catalog_ || !catalog || catalog_->size() != catalog->size();
    try {
        if (catalog_) retired_.push_back(catalog_);
        for (size_t i = 0; catalog && i < catalog->size(); ++i) {
            const auto &next = *catalog->record(i);
            const auto *previous = catalog_ ? catalog_->find(next.sleep_day) : nullptr;
            if (!previous || previous->generation != next.generation) {
                pending_.push_back(next.sleep_day);
                changed = true;
            }
        }
    } catch (const std::bad_alloc &) {
        // Losing reader tracking must never make a directory eligible.
        tracking_failed_ = true;
        finish_night("reader_tracking_alloc_failed");
    }
    catalog_ = std::move(catalog);
    restart_ = restart_ || changed;
    if (changed) reclaim_cursor_ = 0;
}

void ReportGenerationCleanup::publish_layout(
    std::shared_ptr<const NightCatalogLayoutIssues> issues) {
    layout_issues_ = std::move(issues);
    restart_ = true;
}

bool ReportGenerationCleanup::forget_media() {
    // The caller has already waited for poll(false) to acknowledge deletion.
    metadata_.cancel();
    if (!discard_scan()) return false;
    catalog_.reset();
    layout_issues_.reset();
    retired_.clear();
    pending_.clear();
    deferred_.clear();
    entries_.reset();
    night_cursor_ = 0;
    entry_cursor_ = 0;
    day_ = {};
    restart_ = false;
    waiting_for_readers_ = false;
    tracking_failed_ = false;
    retry_at_ms_ = 0;
    publication_.reset();
    reclaim_requested_ = false;
    reclaim_cursor_ = 0;
    reclaim_phase_ = ReclaimPhase::Idle;
    reclaim_metadata_ = {};
    return true;
}

bool ReportGenerationCleanup::pending() const {
    return pending_generations() || reclaim_phase_ != ReclaimPhase::Idle ||
        (scans_ && deletes_ && catalog_ && !tracking_failed_ &&
         can_publish_ && reclaim_requested_ && reclaim_cursor_ < catalog_->size());
}

bool ReportGenerationCleanup::pending_generations() const {
    return (delete_id_ && reclaim_phase_ == ReclaimPhase::Idle) ||
        (scans_ && deletes_ && catalog_ && !tracking_failed_ && night_cursor_ < pending_.size());
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
    if (generation == disk_generation_ || generation == pinned_generation_ ||
        (catalog_ && protects(*catalog_))) {
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
    publication_.reset();
    reclaim_phase_ = ReclaimPhase::Idle;
    reclaim_metadata_ = {};
    disk_generation_ = 0;
    pinned_generation_ = 0;
    pin_checked_ = false;
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
    pinned_generation_ = 0;
    pin_checked_ = false;
    day_ = {};
    waiting_for_readers_ = false;
    if (night_cursor_ < pending_.size()) ++night_cursor_;
    if (night_cursor_ == pending_.size()) {
        pending_.clear();
        night_cursor_ = 0;
    }
}

bool ReportGenerationCleanup::poll(bool allowed, uint32_t now_ms,
                                  StorageSpaceSnapshot space) {
    if (!scans_ || !deletes_) return false;

    if (space.known && space.free_bytes >= StorageSpacePolicy::ReclaimTarget) {
        reclaim_requested_ = false;
        reclaim_cursor_ = 0;
    } else if (space.known && space.free_bytes < StorageSpacePolicy::ReclaimBelow) {
        reclaim_requested_ = true;
    }

    if (delete_id_) {
        if (!allowed || restart_ || tracking_failed_) deletes_->cancel(delete_id_);
        StorageDeleteStatus status;
        if (!deletes_->status(status, 0) ||
            (status.id == delete_id_ && status.state == StorageDeleteState::Deleting)) {
            return false;
        }
        delete_id_ = 0;
        if (reclaim_phase_ != ReclaimPhase::Idle) {
            finish_reclaim(status.state == StorageDeleteState::Error ? status.error : nullptr);
            if (status.state == StorageDeleteState::Cancelled && reclaim_cursor_) {
                --reclaim_cursor_;
            }
            return true;
        }
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

    if (reclaim_phase_ != ReclaimPhase::Idle ||
        (night_cursor_ >= pending_.size() && can_publish_ &&
         reclaim_requested_ && reclaim_cursor_ < catalog_->size())) {
        if (!space.known || !reclaim_requested_) {
            if (reclaim_phase_ != ReclaimPhase::Idle) return discard_scan();
            return false;
        }
        return poll_reclaim(now_ms);
    }

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
            !report_signal_store_generation_directory(entry, base_path_, generation)) {
            ++entry_cursor_;
            return true;
        }
        if (protected_generation(generation)) {
            const auto *current = catalog_->find(day_);
            if (generation != disk_generation_ && generation != pinned_generation_ &&
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
        const bool pin = disk_generation_ != 0;
        const bool missing = metadata_.status().state == StorageBoundedFileLoadState::Missing;
        auto bytes = metadata_.take_completed();
        ReportSignalStoreNightView view;
        const bool valid = bytes && ReportSignalStoreNightCodec::decode(
            bytes->data(), bytes->size(), view) && view.night.sleep_day == day_;
        if (!valid && !(pin && missing)) {
            finish_night(metadata_.status().error[0]
                ? metadata_.status().error : "metadata_unavailable_or_invalid");
            return true;
        }
        if (pin) {
            pinned_generation_ = valid ? view.night.generation : 0;
            pin_checked_ = true;
        } else {
            disk_generation_ = view.night.generation;
        }
        metadata_.reset();
    }

    if (disk_generation_ && !pin_checked_) {
        char path[AC_STORAGE_PATH_MAX] = {};
        report_signal_store_night_path(day_, path, sizeof(path), true);
        if (metadata_.start(path, ReportSignalStoreNightCodec::MaxBytes,
                            operation_generation_, StorageReadLane::Maintenance) ==
            OperationAdmission::Rejected) {
            finish_night("history_metadata_read_rejected");
        }
        return true;
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
    if (layout_issues_ && layout_issues_->blocked(day_)) {
        finish_night("report_layout_night_blocked");
        return true;
    }

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

ReportSignalStoreMetadata ReportGenerationCleanup::take_published_metadata() {
    return publication_.take_published_metadata();
}

void ReportGenerationCleanup::finish_reclaim(const char *error) {
#ifdef ARDUINO
    if (error) {
        char day[9] = {};
        day_.format_yyyymmdd(day, sizeof(day));
        Log::logf(CAT_REPORT, LOG_WARN, "HTTP cache cleanup skipped night=%s error=%s",
                  day, error);
    }
#else
    (void)error;
#endif
    (void)discard_scan();
    ++reclaim_cursor_;
}

bool ReportGenerationCleanup::poll_reclaim(uint32_t now_ms) {
    if (reclaim_phase_ == ReclaimPhase::Idle) {
        day_ = catalog_->record(catalog_->size() - 1 - reclaim_cursor_)->sleep_day;
        if (layout_issues_ && layout_issues_->blocked(day_)) {
            finish_reclaim();
            return true;
        }
        char path[AC_STORAGE_PATH_MAX] = {};
        report_signal_store_night_path(day_, path, sizeof(path));
        if (++operation_generation_ == 0) ++operation_generation_;
        const auto admitted = metadata_.start(path, ReportSignalStoreNightCodec::MaxBytes,
            operation_generation_, StorageReadLane::Maintenance);
        if (admitted == OperationAdmission::Rejected) finish_reclaim("metadata_read_rejected");
        else reclaim_phase_ = ReclaimPhase::Metadata;
        return true;
    }

    if (metadata_.status().active()) return metadata_.poll();
    if (reclaim_phase_ == ReclaimPhase::Metadata) {
        reclaim_metadata_ = ReportSignalStoreMetadata(metadata_.take_completed());
        metadata_.reset();
        const auto *current = catalog_->find(day_);
        if (!reclaim_metadata_.metadata || !current ||
            reclaim_metadata_.view.night.sleep_day != day_ ||
            reclaim_metadata_.view.night.generation != current->generation) {
            finish_reclaim("metadata_unavailable_or_changed");
            return true;
        }
        if (reclaim_metadata_.view.night.flags & REPORT_SIGNAL_STORE_NIGHT_OPEN_SESSION) {
            finish_reclaim();
            return true;
        }

        // A clock-conflict pin can refer to the same generation. Leave both
        // copies alone; changing just night.meta would lose its completion state.
        char pin[AC_STORAGE_PATH_MAX] = {};
        report_signal_store_night_path(day_, pin, sizeof(pin), true);
        if (metadata_.start(pin, ReportSignalStoreNightCodec::MaxBytes,
                            operation_generation_, StorageReadLane::Maintenance) ==
            OperationAdmission::Rejected) finish_reclaim("history_metadata_read_rejected");
        else reclaim_phase_ = ReclaimPhase::Pin;
        return true;
    }

    if (reclaim_phase_ == ReclaimPhase::Pin) {
        if (metadata_.status().state != StorageBoundedFileLoadState::Missing) {
            finish_reclaim();
            return true;
        }
        metadata_.reset();
        char generation[AC_STORAGE_PATH_MAX] = {};
        report_signal_store_generation_path(day_, reclaim_metadata_.view.night.generation,
                                            generation, sizeof(generation));
        if (!storage_append_child_path(generation, "signals", base_path_, sizeof(base_path_))) {
            finish_reclaim("signals_path_invalid");
            return true;
        }
        reclaim_phase_ = ReclaimPhase::Scan;
    }

    if (reclaim_phase_ == ReclaimPhase::Scan) {
        if (!scan_ticket_.valid()) {
            const StorageScanRoot root{base_path_, false};
            const auto submitted = scans_->request_scan({&root, 1, true, operation_generation_});
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) finish_reclaim("signals_scan_rejected");
            else scan_ticket_ = submitted.ticket;
            return true;
        }
        StorageScanCompletion completion;
        if (!scans_->take_completion(scan_ticket_, completion)) return false;
        scan_ticket_ = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            !completion.snapshot) {
            finish_reclaim("signals_scan_failed");
            return true;
        }
        entries_ = std::move(completion.snapshot);
        verify_track_ = 0;
        reclaim_phase_ = ReclaimPhase::Verify;
    }

    if (reclaim_phase_ == ReclaimPhase::Verify) {
        const auto &view = reclaim_metadata_.view;
        if (verify_track_ < view.night.track_count) {
            ReportSignalStoreTrack track;
            if (!view.track(verify_track_++, track)) {
                finish_reclaim("track_unavailable");
                return true;
            }
            for (auto level : {ReportSignalStoreLevel::Raw,
                               ReportSignalStoreLevel::OneSecond,
                               ReportSignalStoreLevel::TenSeconds}) {
                if (!track.has_level(level)) continue;

                char path[AC_STORAGE_PATH_MAX] = {};
                ReportSignalStorePlaneRange range;
                report_signal_store_signal_path(track, level, path, sizeof(path));
                const bool ranged = ReportSignalStoreFileCodec::plane_range(track,
                    track.first_block_start_ms, track.block_slot_count, level, range);
                bool present = false;
                for (size_t i = 0; ranged && i < entries_->size(); ++i) {
                    StorageScanEntryView entry;
                    if (entries_->entry(i, entry) && !entry.directory &&
                        strcmp(entry.path, path) == 0 && entry.size >= range.offset + range.length) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    finish_reclaim("tile_source_missing_or_short");
                    return true;
                }
            }
            return true;
        }

        entries_.reset();
        if (view.night.compression_version == 0) {
            reclaim_phase_ = ReclaimPhase::Delete;
        } else {
            auto pending = ReportSignalStoreNightCodec::with_compression_version(reclaim_metadata_, 0);
            if (!pending.metadata || publication_.start_metadata(std::move(pending),
                    operation_generation_, StorageAtomicWriteLane::Maintenance) !=
                    OperationAdmission::Accepted) finish_reclaim("metadata_clear_rejected");
            else reclaim_phase_ = ReclaimPhase::Publish;
        }
        return true;
    }

    if (reclaim_phase_ == ReclaimPhase::Publish) {
        const bool worked = publication_.poll();
        if (!publication_.status().terminal()) return worked;
        if (publication_.status().state != ReportSignalStoreState::Ready) {
            finish_reclaim(publication_.status().error);
        } else {
            // ReportTask publishes this metadata before the next owner turn.
            reclaim_phase_ = ReclaimPhase::Delete;
        }
        return true;
    }

    if (reclaim_phase_ == ReclaimPhase::Delete) {
        if (retry_at_ms_ && static_cast<int32_t>(now_ms - retry_at_ms_) < 0) return false;
        const char *name = "http";
        char error[AC_STORAGE_ERROR_MAX] = {};
        if (!deletes_->start_selected(base_path_, &name, 1, &delete_id_, error,
                                      sizeof(error), StorageSpacePolicy::ReclaimTarget)) {
            if (strstr(error, "busy") || strstr(error, "unavailable") || !*error) {
                retry_at_ms_ = now_ms + 1000;
            } else finish_reclaim(error);
            return false;
        }
        retry_at_ms_ = 0;
        return true;
    }
    return false;
}

}  // namespace aircannect
