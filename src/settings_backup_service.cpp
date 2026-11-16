#include "settings_backup_service.h"

#include <stdio.h>
#include <string.h>

#include "as11_device_service.h"
#include "as11_settings_manager.h"
#include "debug_log.h"
#include "rpc_request_port.h"

namespace aircannect {
namespace {

const char *root(SettingsBackupKind kind) {
    return kind == SettingsBackupKind::Config ? "/aircannect/backups/ac/"
                                            : "/aircannect/backups/airsense/";
}

}  // namespace

void SettingsBackupService::begin(
    ConfigBackup &config, As11SettingsManager &settings,
    As11DeviceService &device, RpcRequestPort &rpc,
    StorageReadPort &reads, StorageAtomicWritePort &writes, StoragePathPort &paths) {
    config_ = &config;
    settings_ = &settings;
    device_ = &device;
    rpc_ = &rpc;
    writes_ = &writes;
    paths_ = &paths;
    loader_.begin(reads);
}

bool SettingsBackupService::clinical_ready() const {
    return device_->state().available() && settings_->state().valid() &&
           !settings_->write_active() &&
           !settings_->refresh_pending();
}

bool SettingsBackupService::start(
    const SettingsBackupCommand &command, uint32_t now_ms, time_t utc) {
    if (busy()) return false;
    result_ = {};
    kind_ = command.kind;
    started_ms_ = now_ms;
    if (++generation_ == 0) ++generation_;

    if (command.action == SettingsBackupAction::Cancel) {
        if (command.plan == preview_id_) clear_preview();
        finish();
        return true;
    }
    if (!available_) {
        finish("storage_unavailable");
        return true;
    }

    try {
        switch (command.action) {
            case SettingsBackupAction::Create:
                clear_preview();
                create(command.options, utc, now_ms);
                break;
            case SettingsBackupAction::Preview:
                clear_preview();
                preview(command.path);
                break;
            case SettingsBackupAction::Restore:
                restore(command.plan, now_ms);
                break;
            case SettingsBackupAction::Cancel:
                break;
            case SettingsBackupAction::Directory:
                result_.path = root(kind_);
                result_.path.pop_back();
                directory_end_ = result_.path.find('/', 1);
                phase_ = Phase::Directory;
                break;
        }
    } catch (const std::bad_alloc &) {
        finish("backup_alloc");
        clear_preview();
    }
    return true;
}

void SettingsBackupService::create(
    const ConfigBackupOptions &options, time_t utc, uint32_t now_ms) {
    if (utc < 1577836800) {
        finish("clock_not_set");
        return;
    }
    struct tm date {};
    gmtime_r(&utc, &date);
    char month[8], stamp[24], created[24];
    strftime(month, sizeof(month), "%Y-%m", &date);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H%M%SZ", &date);
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", &date);
    std::string directory = root(kind_);

    write_ = {};
    if (kind_ == SettingsBackupKind::Clinical) {
        const auto &device = device_->state();
        if (!clinical_ready() || !storage_valid_child_name(device.serial_number().c_str())) {
            finish("settings_not_ready");
            return;
        }
        directory += device.serial_number() + '/';
        write_.bytes = as11_settings_backup_encode(settings_->state(), device, created);
    } else {
        LargeTextBuffer text;
        if (config_->export_text(options, text)) {
            write_.bytes = LargeByteBuffer::copy_and_freeze(text.c_str(), text.length());
        }
    }
    if (!write_.bytes) {
        finish("backup_alloc");
        return;
    }

    char suffix[32];
    snprintf(suffix, sizeof(suffix), "-%08lx-%lu.%s",
             static_cast<unsigned long>(now_ms), static_cast<unsigned long>(generation_),
             kind_ == SettingsBackupKind::Config ? "txt" : "json");
    result_.path = directory + month + '/' + stamp + suffix;
    write_.path = result_.path;
    write_.generation = generation_;
    write_.replace_existing = false;
    write_.lane = StorageAtomicWriteLane::Foreground;
    phase_ = Phase::Writing;
}

void SettingsBackupService::preview(const std::string &path) {
    if (!storage_user_path_valid(path.c_str()) || path.compare(0, strlen(root(kind_)), root(kind_)) != 0) {
        finish("invalid_path");
        return;
    }
    if (kind_ == SettingsBackupKind::Clinical && !clinical_ready()) {
        finish("settings_not_ready");
        return;
    }
    preview_path_ = path;
    result_.path = path;
    preview_kind_ = kind_;
    if (loader_.start(path.c_str(), FileLimit, generation_, StorageReadLane::Foreground) !=
        OperationAdmission::Accepted) {
        finish("backup_read_failed");
        return;
    }
    phase_ = Phase::Reading;
}

void SettingsBackupService::complete_read() {
    const auto status = loader_.status();
    preview_file_ = loader_.take_completed();
    loader_.reset();
    if (!preview_file_ || preview_file_->size() != status.file_size) {
        finish(status.file_size > FileLimit ? "backup_too_large" :
               (status.error[0] ? status.error : "backup_read_failed"));
        clear_preview();
        return;
    }
    if (kind_ == SettingsBackupKind::Clinical) {
        const char *error = clinical_ready() ? as11_settings_backup_prepare(
            *preview_file_, settings_->state(), clinical_plan_) : "settings_not_ready";
        if (error) {
            finish(error);
            clear_preview();
            return;
        }
        target_serial_ = device_->state().serial_number();
        settings_revision_ = settings_->revision();
        preview_file_.reset();
    } else {
        if (memchr(preview_file_->data(), 0, preview_file_->size())) {
            finish("invalid_backup");
            clear_preview();
            return;
        }
        config_plan_ = config_->prepare_restore(
            reinterpret_cast<const char *>(preview_file_->data()), preview_file_->size());
        preview_file_.reset();
        const char *error = config_plan_ ? config_plan_->prepare().error : "backup_unavailable";
        if (error) {
            finish(error);
            clear_preview();
            return;
        }
    }
    preview_id_ = generation_;
    preview_ms_ = started_ms_;
    result_.plan = preview_id_;
    result_.path = preview_path_;
    finish();
}

void SettingsBackupService::restore(uint32_t plan, uint32_t now_ms) {
    if (!plan || plan != preview_id_ || kind_ != preview_kind_ ||
        now_ms - preview_ms_ >= PreviewTimeoutMs) {
        finish("preview_expired");
        return;
    }
    result_.path = preview_path_;
    if (kind_ == SettingsBackupKind::Config) {
        result_.config = config_plan_->finish();
        const auto &restored = result_.config;
        finish(restored.error ? restored.error :
               !restored.wifi_saved ? "wifi_not_saved" :
               !restored.sensors_saved ? "sensors_not_saved" : nullptr);
        clear_preview();
        return;
    }

    if (!clinical_ready() || settings_->revision() != settings_revision_ ||
        device_->state().serial_number() != target_serial_) {
        finish("settings_changed");
        clear_preview();
        return;
    }
    if (clinical_plan_.write.empty()) {
        finish();
        clear_preview();
        return;
    }
    const auto submission = settings_->write(*rpc_, clinical_plan_.write, RpcSource::HttpApi, now_ms);
    if (!submission.accepted()) {
        finish("settings_busy");
        return;
    }
    clear_preview();
    phase_ = Phase::Readback;
}

void SettingsBackupService::poll(uint32_t now_ms) {
    if (preview_id_ && now_ms - preview_ms_ >= PreviewTimeoutMs) clear_preview();
    if (!busy()) return;
    if (now_ms - started_ms_ >= OperationTimeoutMs) {
        finish("backup_timeout");
        return;
    }

    try {
        if (phase_ == Phase::Directory) {
            if (!directory_ticket_.valid()) {
                StoragePathCommand command;
                command.operation = StoragePathOperation::EnsureDirectory;
                command.source = result_.path.substr(0, directory_end_);
                command.generation = generation_;
                const auto submission = paths_->request(command);
                if (submission.admission == OperationAdmission::Busy) return;
                if (!submission.accepted()) {
                    finish("backup_directory_failed");
                    return;
                }
                directory_ticket_ = submission.ticket;
            }
            StoragePathCompletion completion;
            if (!paths_->take_completion(directory_ticket_, completion)) return;
            directory_ticket_ = {};
            if (completion.outcome.disposition != OperationDisposition::Succeeded) {
                finish(completion.error[0] ? completion.error : "backup_directory_failed");
            } else if (directory_end_ == std::string::npos) {
                finish();
            } else {
                directory_end_ = result_.path.find('/', directory_end_ + 1);
            }
        } else if (phase_ == Phase::Reading) {
            loader_.poll();
            if (loader_.status().terminal()) complete_read();
        } else if (phase_ == Phase::Writing) {
            if (!write_ticket_.valid()) {
                const auto submission = writes_->request_write(write_);
                if (submission.admission == OperationAdmission::Busy) return;
                if (!submission.accepted()) {
                    finish("backup_write_rejected");
                    return;
                }
                write_ticket_ = submission.ticket;
                write_ = {};
            }
            StorageAtomicWriteCompletion completion;
            if (!writes_->take_completion(write_ticket_, completion)) return;
            write_ticket_ = {};
            finish(completion.outcome.disposition == OperationDisposition::Succeeded ? nullptr :
                   (completion.error[0] ? completion.error : "backup_write_failed"));
        } else if (phase_ == Phase::Readback) {
            const auto &state = settings_->state();
            if (settings_->write_active()) return;
            const std::string status = state.last_write_status();
            finish(status == "confirmed" ? nullptr :
                   (status.empty() ? "settings_unavailable" : status.c_str()));
        }
    } catch (const std::bad_alloc &) {
        finish("backup_alloc");
        clear_preview();
    }
}

void SettingsBackupService::finish(const char *error) {
    result_.error = error ? error : "";
    result_.ready = true;
    loader_.reset();
    if (write_ticket_.valid()) writes_->abandon(write_ticket_);
    if (directory_ticket_.valid()) paths_->abandon(directory_ticket_);
    write_ticket_ = {};
    directory_ticket_ = {};
    write_ = {};
    phase_ = Phase::Idle;

    if (error) Log::logf(CAT_CONFIG, LOG_WARN, "backup failed path=%s error=%s\n",
                         result_.path.c_str(), error);
}

void SettingsBackupService::clear_preview() {
    preview_file_.reset();
    config_plan_.reset();
    clinical_plan_ = {};
    preview_path_.clear();
    target_serial_.clear();
    preview_id_ = 0;
}

void SettingsBackupService::cancel() {
    if (busy()) finish("backup_cancelled");
    clear_preview();
}

void SettingsBackupService::set_storage_available(bool available) {
    available_ = available;
    if (!available) cancel();
}

}  // namespace aircannect
