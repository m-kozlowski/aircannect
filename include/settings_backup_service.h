#pragma once

#include <time.h>

#include "as11_settings_backup.h"
#include "config_backup.h"
#include "storage_atomic_write_port.h"
#include "storage_bounded_file_loader.h"
#include "storage_path_port.h"

namespace aircannect {

class As11DeviceService;
class As11SettingsManager;
class RpcRequestPort;

enum class SettingsBackupKind : uint8_t { Config, Clinical };
enum class SettingsBackupAction : uint8_t { Create, Preview, Restore, Cancel, Directory };

struct SettingsBackupCommand {
    SettingsBackupKind kind = SettingsBackupKind::Config;
    SettingsBackupAction action = SettingsBackupAction::Create;
    ConfigBackupOptions options;
    std::string path;
    uint32_t plan = 0;
};

struct SettingsBackupResult {
    bool ready = false;
    std::string error;
    std::string path;
    uint32_t plan = 0;
    ConfigRestoreResult config;
};

// Main-loop owner of explicit backup/restore operations. Storage owns every
// file access; the settings owners remain responsible for applying changes.
class SettingsBackupService {
public:
    void begin(ConfigBackup &config, As11SettingsManager &settings,
               As11DeviceService &device, RpcRequestPort &rpc,
               StorageReadPort &reads, StorageAtomicWritePort &writes,
               StoragePathPort &paths);
    bool start(const SettingsBackupCommand &command, uint32_t now_ms, time_t utc);
    void poll(uint32_t now_ms);
    void set_storage_available(bool available);
    void cancel();

    bool busy() const { return phase_ != Phase::Idle; }
    const SettingsBackupResult &result() const { return result_; }
    const As11SettingsBackupPlan &clinical_plan() const { return clinical_plan_; }

private:
    enum class Phase : uint8_t { Idle, Reading, Writing, Readback, Directory };
    static constexpr size_t FileLimit = 64 * 1024;
    static constexpr uint32_t OperationTimeoutMs = 30000;
    static constexpr uint32_t PreviewTimeoutMs = 5 * 60 * 1000;

    void create(const ConfigBackupOptions &options, time_t utc, uint32_t now_ms);
    void preview(const std::string &path);
    void restore(uint32_t plan, uint32_t now_ms);
    void complete_read();
    void finish(const char *error = nullptr);
    void clear_preview();
    bool clinical_ready() const;

    ConfigBackup *config_ = nullptr;
    As11SettingsManager *settings_ = nullptr;
    As11DeviceService *device_ = nullptr;
    RpcRequestPort *rpc_ = nullptr;
    StorageAtomicWritePort *writes_ = nullptr;
    StoragePathPort *paths_ = nullptr;
    StorageBoundedFileLoader loader_;

    Phase phase_ = Phase::Idle;
    SettingsBackupKind kind_ = SettingsBackupKind::Config;
    SettingsBackupResult result_;
    bool available_ = false;
    uint32_t generation_ = 0;
    uint32_t started_ms_ = 0;
    StorageAtomicWriteCommand write_;
    OperationTicket write_ticket_;
    OperationTicket directory_ticket_;
    size_t directory_end_ = 0;

    // Only the previewed bytes/prepared write may be restored. No reread and
    // no automatic backup, including when a restore fails.
    std::shared_ptr<const LargeByteBuffer> preview_file_;
    As11SettingsBackupPlan clinical_plan_;
    std::string preview_path_;
    std::string target_serial_;
    uint32_t preview_id_ = 0;
    uint32_t preview_ms_ = 0;
    uint32_t settings_revision_ = 0;
    SettingsBackupKind preview_kind_ = SettingsBackupKind::Config;
};

}  // namespace aircannect
