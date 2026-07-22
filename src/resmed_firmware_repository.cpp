#include "resmed_firmware_repository.h"

#include <Arduino.h>
#include <string.h>

#include "debug_log.h"
#include "string_util.h"

namespace aircannect {

const char *resmed_firmware_repository_state_name(
    ResmedFirmwareRepositoryState state) {
    switch (state) {
        case ResmedFirmwareRepositoryState::Idle: return "idle";
        case ResmedFirmwareRepositoryState::EnsuringDirectory:
            return "preparing";
        case ResmedFirmwareRepositoryState::Scanning: return "scanning";
        case ResmedFirmwareRepositoryState::Removing: return "removing";
        case ResmedFirmwareRepositoryState::Ready: return "ready";
        case ResmedFirmwareRepositoryState::Error: return "error";
    }
    return "error";
}

ResmedFirmwareRepository::~ResmedFirmwareRepository() {
    if (mutex_) vSemaphoreDelete(mutex_);
}

bool ResmedFirmwareRepository::begin(StorageScanPort &scan_port,
                                     StoragePathPort &path_port) {
    scan_port_ = &scan_port;
    path_port_ = &path_port;
    if (!mutex_) {
        mutex_ = xSemaphoreCreateMutexStatic(&mutex_storage_);
    }
    return mutex_ != nullptr;
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

bool ResmedFirmwareRepository::request_refresh(bool foreground) {
    if (!lock()) return false;

    refresh_requested_ = true;
    foreground_refresh_ = foreground_refresh_ || foreground;
    retry_at_ms_ = 0;
    status_.refresh_pending = true;
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

bool ResmedFirmwareRepository::request_remove(const char *path) {
    if (!direct_repository_file(path) || !lock()) return false;
    if (remove_requested_ || !snapshot_ || !snapshot_->contains_file(path)) {
        unlock();
        return false;
    }

    copy_cstr(remove_path_, sizeof(remove_path_), path);
    remove_requested_ = true;
    retry_at_ms_ = 0;
    status_.state = ResmedFirmwareRepositoryState::Removing;
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
    unlock();
}

void ResmedFirmwareRepository::fail_action(const char *error) {
    action_ = Action::None;
    ticket_ = {};
    directory_ready_ = false;

    const char *reason = error && error[0] ? error : "repository_failed";
    if (lock(50)) {
        retry_at_ms_ = millis() + RetryDelayMs;
        status_.state = ResmedFirmwareRepositoryState::Error;
        copy_cstr(status_.error, sizeof(status_.error), reason);
        unlock();
    }

    Log::logf(CAT_OTA, LOG_WARN,
              "[RESMED] firmware repository failed: %s\n",
              reason);
}

void ResmedFirmwareRepository::poll_completion() {
    if (!ticket_.valid()) return;

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
        refresh_requested_ = false;
        foreground_refresh_ = false;
        retry_at_ms_ = 0;
        status_.state = ResmedFirmwareRepositoryState::Ready;
        status_.revision = next->revision();
        status_.entries = next->size();
        status_.truncated = next->truncated();
        status_.refresh_pending = false;
        status_.error[0] = '\0';
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
    if (completed_action == Action::Remove) {
        if (lock(50)) {
            remove_requested_ = false;
            remove_path_[0] = '\0';
            refresh_requested_ = true;
            foreground_refresh_ = true;
            retry_at_ms_ = 0;
            unlock();
        }
        publish_status(ResmedFirmwareRepositoryState::Idle);
    }
}

void ResmedFirmwareRepository::start_pending_operation() {
    bool refresh_requested = false;
    bool foreground_refresh = false;
    bool remove_requested = false;
    char remove_path[AC_STORAGE_PATH_MAX] = {};
    ActivitySnapshot activity;
    uint32_t retry_at_ms = 0;
    if (!lock()) return;
    refresh_requested = refresh_requested_;
    foreground_refresh = foreground_refresh_;
    remove_requested = remove_requested_;
    copy_cstr(remove_path, sizeof(remove_path), remove_path_);
    activity = activity_;
    retry_at_ms = retry_at_ms_;
    unlock();

    if (!refresh_requested && !remove_requested) return;
    const uint32_t now_ms = millis();
    if (retry_at_ms != 0 &&
        static_cast<int32_t>(now_ms - retry_at_ms) < 0) {
        return;
    }

    const bool hard_blocked = activity.therapy_active ||
                              activity.ota_install_active;
    const bool background_blocked = activity.realtime_stream_active ||
                                    activity.foreground_report_demand ||
                                    activity.export_active;
    if (hard_blocked ||
        (!foreground_refresh && !remove_requested && background_blocked)) {
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
        publish_status(ResmedFirmwareRepositoryState::Scanning);
    } else if (submitted.admission == OperationAdmission::Rejected) {
        fail_action("repository_scan_rejected");
    }
}

void ResmedFirmwareRepository::poll() {
    if (!scan_port_ || !path_port_ || !mutex_) return;

    poll_completion();
    if (action_ == Action::None) start_pending_operation();
}

}  // namespace aircannect
