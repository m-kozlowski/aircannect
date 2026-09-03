#include "resmed_firmware_repository.h"

#include <Arduino.h>
#include <string.h>

#include "debug_log.h"
#include "resmed_firmware_image.h"
#include "string_util.h"

namespace aircannect {

const char *resmed_firmware_repository_state_name(
    ResmedFirmwareRepositoryState state) {
    switch (state) {
        case ResmedFirmwareRepositoryState::Idle: return "idle";
        case ResmedFirmwareRepositoryState::EnsuringDirectory:
            return "preparing";
        case ResmedFirmwareRepositoryState::Scanning: return "scanning";
        case ResmedFirmwareRepositoryState::Inspecting: return "inspecting";
        case ResmedFirmwareRepositoryState::StoringBootloader:
            return "storing_bootloader";
        case ResmedFirmwareRepositoryState::Removing: return "removing";
        case ResmedFirmwareRepositoryState::Ready: return "ready";
        case ResmedFirmwareRepositoryState::Error: return "error";
    }
    return "error";
}

ResmedFirmwareRepository::~ResmedFirmwareRepository() {
    if (read_port_ && publication_prepared_.valid()) {
        read_port_->release_prepared(publication_prepared_);
    }
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool ResmedFirmwareRepository::begin(StorageScanPort &scan_port,
                                     StorageReadPort &read_port,
                                     StoragePathPort &path_port) {
    scan_port_ = &scan_port;
    read_port_ = &read_port;
    path_port_ = &path_port;
    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    if (!mutex_) return false;

    status_.generation = 1;
    status_generation_.store(status_.generation, std::memory_order_release);
    return true;
}

bool ResmedFirmwareRepository::lock(uint32_t timeout_ms) const {
    return mutex_ &&
           xSemaphoreTake(mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ResmedFirmwareRepository::unlock() const {
    if (mutex_) xSemaphoreGive(mutex_);
}

uint32_t ResmedFirmwareRepository::next_generation() {
    generation_++;
    if (generation_ == 0) generation_++;
    return generation_;
}

void ResmedFirmwareRepository::advance_status_generation_locked() {
    status_.generation++;
    if (status_.generation == 0) status_.generation++;
    status_generation_.store(status_.generation, std::memory_order_release);
}

void ResmedFirmwareRepository::request_refresh_locked(bool foreground) {
    refresh_generation_++;
    if (refresh_generation_ == 0) refresh_generation_++;

    refresh_requested_ = true;
    foreground_refresh_ = foreground_refresh_ || foreground;
    retry_at_ms_ = 0;
    status_.refresh_pending = true;
    advance_status_generation_locked();
}

bool ResmedFirmwareRepository::request_refresh(bool foreground) {
    if (!lock()) return false;

    request_refresh_locked(foreground);
    unlock();
    return true;
}

bool ResmedFirmwareRepository::direct_repository_file(
    const char *path) const {
    if (!path || !path[0]) return false;

    const size_t prefix_length = strlen(AC_RESMED_FIRMWARE_REPOSITORY_PATH);
    if (strncmp(path, AC_RESMED_FIRMWARE_REPOSITORY_PATH,
                prefix_length) != 0 ||
        path[prefix_length] != '/') {
        return false;
    }

    const char *filename = path + prefix_length + 1;
    return filename[0] != '\0' && strchr(filename, '/') == nullptr;
}

bool ResmedFirmwareRepository::consume_file_published(const char *path) {
    if (!direct_repository_file(path)) return true;
    if (!lock()) return false;
    if (publication_phase_ != PublicationPhase::None) {
        unlock();
        return false;
    }

    copy_cstr(publication_path_, sizeof(publication_path_), path);
    publication_phase_ = PublicationPhase::InspectPath;
    retry_at_ms_ = 0;
    status_.state = ResmedFirmwareRepositoryState::Inspecting;
    status_.refresh_pending = true;
    advance_status_generation_locked();
    unlock();
    return true;
}

bool ResmedFirmwareRepository::request_remove(const char *path) {
    if (!direct_repository_file(path) || !lock()) return false;
    if (remove_requested_ || !snapshot_ ||
        !snapshot_->contains_file(path)) {
        unlock();
        return false;
    }

    copy_cstr(remove_path_, sizeof(remove_path_), path);
    remove_requested_ = true;
    retry_at_ms_ = 0;
    status_.state = ResmedFirmwareRepositoryState::Removing;
    advance_status_generation_locked();
    unlock();
    return true;
}

void ResmedFirmwareRepository::publish_activity(
    const ActivitySnapshot &activity) {
    if (!lock()) return;
    activity_ = activity;
    unlock();
}

ResmedFirmwareRepositoryStatus ResmedFirmwareRepository::status() const {
    ResmedFirmwareRepositoryStatus result;
    if (!lock()) return result;
    result = status_;
    unlock();
    return result;
}

std::shared_ptr<const ResmedFirmwareCatalogSnapshot>
ResmedFirmwareRepository::snapshot() const {
    if (!lock()) return {};
    const std::shared_ptr<const ResmedFirmwareCatalogSnapshot> result =
        snapshot_;
    unlock();
    return result;
}

void ResmedFirmwareRepository::publish_status(
    ResmedFirmwareRepositoryState state,
    const char *error) {
    if (!lock(50)) return;

    status_.state = state;
    copy_cstr(status_.error, sizeof(status_.error), error ? error : "");
    if (snapshot_) {
        status_.revision = snapshot_->revision();
        status_.entries = snapshot_->size();
        status_.truncated = snapshot_->truncated();
    }
    advance_status_generation_locked();
    unlock();
}

void ResmedFirmwareRepository::fail_action(const char *error) {
    if (read_port_ && publication_prepared_.valid()) {
        read_port_->release_prepared(publication_prepared_);
        publication_prepared_ = {};
    }
    action_ = Action::None;
    ticket_ = {};
    active_refresh_generation_ = 0;
    directory_ready_ = false;

    const char *reason = error && error[0] ? error : "repository_failed";
    if (lock(50)) {
        retry_at_ms_ = millis() + RetryDelayMs;
        status_.state = ResmedFirmwareRepositoryState::Error;
        status_.refresh_pending =
            refresh_requested_ || publication_phase_ != PublicationPhase::None;
        copy_cstr(status_.error, sizeof(status_.error), reason);
        advance_status_generation_locked();
        unlock();
    }

    Log::logf(CAT_OTA, LOG_WARN,
              "[RESMED] firmware repository failed: %s\n",
              reason);
}

void ResmedFirmwareRepository::finish_publication() {
    if (read_port_ && publication_prepared_.valid()) {
        read_port_->release_prepared(publication_prepared_);
        publication_prepared_ = {};
    }
    if (!lock(50)) {
        fail_action("repository_status_busy");
        return;
    }

    publication_phase_ = PublicationPhase::None;
    publication_path_[0] = '\0';
    bootloader_version_[0] = '\0';
    bootloader_directory_[0] = '\0';
    bootloader_destination_[0] = '\0';
    request_refresh_locked(false);
    status_.state = ResmedFirmwareRepositoryState::Idle;
    unlock();
}

void ResmedFirmwareRepository::decode_published_boot_id() {
    if (!publication_prepared_.valid()) {
        fail_action("repository_boot_id_read_missing");
        return;
    }

    uint8_t boot_id[AC_RESMED_FGBL_BOOT_ID_BYTES] = {};
    const PreparedByteRead read = read_port_->read_prepared(
        publication_prepared_, 0, boot_id, sizeof(boot_id));
    if (read.state == PreparedByteReadState::Retry) return;

    read_port_->release_prepared(publication_prepared_);
    publication_prepared_ = {};
    if (read.state != PreparedByteReadState::Data ||
        read.bytes != sizeof(boot_id)) {
        fail_action("repository_boot_id_short_read");
        return;
    }

    if (!resmed_firmware_identify_fgbl(
            AC_RESMED_FGBL_BYTES, boot_id, sizeof(boot_id),
            bootloader_version_, sizeof(bootloader_version_))) {
        finish_publication();
        return;
    }
    if (!resmed_firmware_patched_bootloader_path(
            bootloader_version_, bootloader_directory_,
            sizeof(bootloader_directory_), bootloader_destination_,
            sizeof(bootloader_destination_))) {
        fail_action("repository_bootloader_path_invalid");
        return;
    }

    publication_phase_ = PublicationPhase::EnsureBootloaderRoot;
    publish_status(ResmedFirmwareRepositoryState::StoringBootloader);
}

void ResmedFirmwareRepository::poll_completion() {
    if (!ticket_.valid()) return;

    if (action_ == Action::ReadPublishedBootId) {
        StorageReadCompletion completion;
        if (!read_port_->take_completion(ticket_, completion)) return;

        ticket_ = {};
        action_ = Action::None;
        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.prepared.valid() ||
            completion.prepared.length != AC_RESMED_FGBL_BOOT_ID_BYTES) {
            if (completion.prepared.valid()) {
                read_port_->release_prepared(completion.prepared);
            }
            fail_action(completion.error[0] ? completion.error
                                            : "repository_boot_id_read_failed");
            return;
        }

        publication_prepared_ = completion.prepared;
        publication_phase_ = PublicationPhase::DecodeBootId;
        return;
    }

    if (action_ == Action::Scan) {
        StorageScanCompletion completion;
        if (!scan_port_->take_completion(ticket_, completion)) return;

        ticket_ = {};
        action_ = Action::None;
        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.snapshot) {
            fail_action(completion.error[0] ? completion.error
                                            : "repository_scan_failed");
            return;
        }

        const uint32_t revision = next_generation();
        const std::shared_ptr<const ResmedFirmwareCatalogSnapshot> next =
            ResmedFirmwareCatalogSnapshot::build(*completion.snapshot,
                                                 revision);
        if (!next) {
            fail_action("repository_catalog_allocation_failed");
            return;
        }

        if (!lock(50)) {
            fail_action("repository_status_busy");
            return;
        }
        snapshot_ = next;
        refresh_requested_ =
            refresh_generation_ != active_refresh_generation_;
        if (!refresh_requested_) foreground_refresh_ = false;
        active_refresh_generation_ = 0;
        retry_at_ms_ = 0;
        status_.state = ResmedFirmwareRepositoryState::Ready;
        status_.revision = next->revision();
        status_.entries = next->size();
        status_.truncated = next->truncated();
        status_.refresh_pending = refresh_requested_;
        status_.error[0] = '\0';
        advance_status_generation_locked();
        unlock();
        return;
    }

    StoragePathCompletion completion;
    if (!path_port_->take_completion(ticket_, completion)) return;

    const Action completed_action = action_;
    ticket_ = {};
    action_ = Action::None;
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        fail_action(completion.error[0] ? completion.error
                                        : "repository_path_failed");
        return;
    }

    if (completed_action == Action::EnsureDirectory) {
        directory_ready_ = true;
        publish_status(ResmedFirmwareRepositoryState::Idle);
        return;
    }
    if (completed_action == Action::InspectPublishedPath) {
        if (!completion.exists || completion.directory ||
            completion.size != AC_RESMED_FGBL_BYTES) {
            finish_publication();
            return;
        }

        publication_phase_ = PublicationPhase::ReadBootId;
        publish_status(ResmedFirmwareRepositoryState::Inspecting);
        return;
    }
    if (completed_action == Action::EnsureBootloaderRoot) {
        publication_phase_ = PublicationPhase::EnsureBootloaderDirectory;
        return;
    }
    if (completed_action == Action::EnsureBootloaderDirectory) {
        publication_phase_ = PublicationPhase::StoreBootloader;
        return;
    }
    if (completed_action == Action::StoreBootloader) {
        Log::logf(CAT_OTA, LOG_INFO,
                  "[RESMED] stored patched bootloader version=%s path=%s\n",
                  bootloader_version_, bootloader_destination_);
        finish_publication();
        return;
    }
    if (completed_action == Action::Remove) {
        if (lock(50)) {
            remove_requested_ = false;
            remove_path_[0] = '\0';
            request_refresh_locked(true);
            unlock();
        }
        publish_status(ResmedFirmwareRepositoryState::Idle);
        return;
    }
}

void ResmedFirmwareRepository::start_pending_operation() {
    bool refresh_requested = false;
    bool foreground_refresh = false;
    bool remove_requested = false;
    PublicationPhase publication_phase = PublicationPhase::None;
    uint32_t refresh_generation = 0;
    char remove_path[AC_STORAGE_PATH_MAX] = {};
    char publication_path[AC_STORAGE_PATH_MAX] = {};
    char bootloader_directory[AC_STORAGE_PATH_MAX] = {};
    char bootloader_destination[AC_STORAGE_PATH_MAX] = {};
    ActivitySnapshot activity;
    uint32_t retry_at_ms = 0;
    if (!lock()) return;
    refresh_requested = refresh_requested_;
    foreground_refresh = foreground_refresh_;
    remove_requested = remove_requested_;
    publication_phase = publication_phase_;
    refresh_generation = refresh_generation_;
    copy_cstr(remove_path, sizeof(remove_path), remove_path_);
    copy_cstr(publication_path, sizeof(publication_path), publication_path_);
    copy_cstr(bootloader_directory, sizeof(bootloader_directory),
              bootloader_directory_);
    copy_cstr(bootloader_destination, sizeof(bootloader_destination),
              bootloader_destination_);
    activity = activity_;
    retry_at_ms = retry_at_ms_;
    unlock();

    if (!refresh_requested && !remove_requested &&
        publication_phase == PublicationPhase::None) {
        return;
    }

    if (publication_phase == PublicationPhase::DecodeBootId) {
        decode_published_boot_id();
        return;
    }

    const uint32_t now_ms = millis();
    if (retry_at_ms != 0 &&
        static_cast<int32_t>(now_ms - retry_at_ms) < 0) {
        return;
    }

    const bool hard_blocked = activity.therapy_active ||
                              activity.ota_install_active;
    const bool background_blocked = activity.realtime_stream_active ||
                                    activity.foreground_report_demand ||
                                    activity.export_work_claimed;
    if (hard_blocked ||
        (!foreground_refresh && !remove_requested &&
         publication_phase == PublicationPhase::None &&
         background_blocked)) {
        return;
    }

    if (!directory_ready_) {
        StoragePathCommand command;
        command.operation = StoragePathOperation::EnsureDirectory;
        command.source = AC_RESMED_FIRMWARE_REPOSITORY_PATH;
        command.generation = next_generation();
        const OperationSubmission submitted = path_port_->request(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ = Action::EnsureDirectory;
            publish_status(ResmedFirmwareRepositoryState::EnsuringDirectory);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_directory_rejected");
        }
        return;
    }

    if (publication_phase == PublicationPhase::InspectPath) {
        StoragePathCommand command;
        command.operation = StoragePathOperation::Stat;
        command.source = publication_path;
        command.generation = next_generation();
        const OperationSubmission submitted = path_port_->request(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ = Action::InspectPublishedPath;
            publish_status(ResmedFirmwareRepositoryState::Inspecting);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_upload_stat_rejected");
        }
        return;
    }

    if (publication_phase == PublicationPhase::ReadBootId) {
        StorageReadCommand command;
        command.path = publication_path;
        command.mode = StorageReadMode::Range;
        command.offset = AC_RESMED_FGBL_BOOT_ID_OFFSET;
        command.length = AC_RESMED_FGBL_BOOT_ID_BYTES;
        command.lane = StorageReadLane::Foreground;
        command.generation = next_generation();
        const OperationSubmission submitted = read_port_->request_read(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ = Action::ReadPublishedBootId;
            publish_status(ResmedFirmwareRepositoryState::Inspecting);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_boot_id_read_rejected");
        }
        return;
    }

    if (publication_phase == PublicationPhase::EnsureBootloaderRoot ||
        publication_phase == PublicationPhase::EnsureBootloaderDirectory) {
        StoragePathCommand command;
        command.operation = StoragePathOperation::EnsureDirectory;
        command.source =
            publication_phase == PublicationPhase::EnsureBootloaderRoot
                ? AC_RESMED_BOOTLOADER_REPOSITORY_PATH
                : bootloader_directory;
        command.generation = next_generation();
        const OperationSubmission submitted = path_port_->request(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ =
                publication_phase == PublicationPhase::EnsureBootloaderRoot
                    ? Action::EnsureBootloaderRoot
                    : Action::EnsureBootloaderDirectory;
            publish_status(ResmedFirmwareRepositoryState::StoringBootloader);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_bootloader_directory_rejected");
        }
        return;
    }

    if (publication_phase == PublicationPhase::StoreBootloader) {
        StoragePathCommand command;
        command.operation = StoragePathOperation::MoveReplacing;
        command.source = publication_path;
        command.destination = bootloader_destination;
        command.generation = next_generation();
        const OperationSubmission submitted = path_port_->request(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ = Action::StoreBootloader;
            publish_status(ResmedFirmwareRepositoryState::StoringBootloader);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_bootloader_store_rejected");
        }
        return;
    }

    if (remove_requested) {
        StoragePathCommand command;
        command.operation = StoragePathOperation::Remove;
        command.source = remove_path;
        command.generation = next_generation();
        const OperationSubmission submitted = path_port_->request(command);
        if (submitted.accepted()) {
            ticket_ = submitted.ticket;
            action_ = Action::Remove;
            publish_status(ResmedFirmwareRepositoryState::Removing);
        } else if (submitted.admission == OperationAdmission::Rejected) {
            fail_action("repository_remove_rejected");
        }
        return;
    }

    StorageScanRoot root;
    root.path = AC_RESMED_FIRMWARE_REPOSITORY_PATH;
    root.recursive = false;
    StorageScanCommand command;
    command.roots = &root;
    command.root_count = 1;
    command.include_directories = false;
    command.generation = next_generation();
    const OperationSubmission submitted = scan_port_->request_scan(command);
    if (submitted.accepted()) {
        ticket_ = submitted.ticket;
        action_ = Action::Scan;
        active_refresh_generation_ = refresh_generation;
        publish_status(ResmedFirmwareRepositoryState::Scanning);
    } else if (submitted.admission == OperationAdmission::Rejected) {
        fail_action("repository_scan_rejected");
    }
}

void ResmedFirmwareRepository::poll() {
    if (!scan_port_ || !read_port_ || !path_port_ || !mutex_) return;

    poll_completion();
    if (action_ == Action::None) start_pending_operation();
}

}  // namespace aircannect
