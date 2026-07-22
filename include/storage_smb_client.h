#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "background_operation_control.h"
#include "storage_path.h"

struct smb2_context;
struct smb2fh;

namespace aircannect {

struct StorageSmbPendingOperation;

static constexpr size_t AC_STORAGE_SMB_REMOTE_PATH_MAX = 256;
static constexpr size_t AC_STORAGE_SMB_ENDPOINT_HOST_MAX = 96;
static constexpr size_t AC_STORAGE_SMB_SHARE_MAX = 64;
static constexpr size_t AC_STORAGE_SMB_BASE_PATH_MAX = 128;
static constexpr size_t AC_STORAGE_SMB_USER_MAX = 65;
static constexpr size_t AC_STORAGE_SMB_PASSWORD_MAX = 129;

struct StorageSmbRemoteStat {
    bool exists = false;
    bool directory = false;
    uint64_t size = 0;
};

enum class StorageSmbOperationResult : uint8_t {
    Waiting,
    Ready,
    Error,
};

class StorageSmbClient {
public:
    StorageSmbClient() = default;
    ~StorageSmbClient();

    bool configure(const char *endpoint,
                   const char *user,
                   const char *password,
                   char *error_out = nullptr,
                   size_t error_out_size = 0);

    StorageSmbOperationResult resolve_host(char *error_out = nullptr,
                                           size_t error_out_size = 0);
    StorageSmbOperationResult step_connect(
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);
    StorageSmbOperationResult step_disconnect(
        const BackgroundOperationControl *operation = nullptr);
    bool connected() const { return connected_; }

    bool make_remote_path(const char *absolute_path,
                          char *out,
                          size_t out_size) const;

    StorageSmbOperationResult step_stat(
        const char *remote_path,
        StorageSmbRemoteStat &out,
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);
    StorageSmbOperationResult step_ensure_directory(
        const char *remote_path,
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);

    StorageSmbOperationResult step_open_writer(
        const char *remote_path,
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);
    StorageSmbOperationResult step_write(
        const uint8_t *data,
        size_t len,
        size_t &written_out,
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);
    StorageSmbOperationResult step_close_writer(
        char *error_out = nullptr,
        size_t error_out_size = 0,
        const BackgroundOperationControl *operation = nullptr);
    void abort_connection();

private:
    friend struct StorageSmbDnsCallback;

    enum class HostResolutionState : uint8_t {
        Idle,
        Pending,
        Ready,
        Error,
    };

    enum class OperationKind : uint8_t {
        None,
        Connect,
        Stat,
        Mkdir,
        Open,
        Write,
        Close,
        Disconnect,
    };

    enum class EnsureDirectoryPhase : uint8_t {
        Idle,
        Stat,
        Mkdir,
        Verify,
    };

    bool parse_endpoint(const char *endpoint,
                        char *error_out,
                        size_t error_out_size);
    void publish_host_resolution(const void *address, int error);
    void reset_host_resolution();
    bool begin_operation(OperationKind kind,
                         char *error_out,
                         size_t error_out_size);
    StorageSmbOperationResult poll_operation(
        OperationKind kind,
        int &status_out,
        const BackgroundOperationControl *operation,
        char *error_out,
        size_t error_out_size);
    void clear_operation();
    StorageSmbOperationResult step_mkdir(
        const char *remote_path,
        char *error_out,
        size_t error_out_size,
        const BackgroundOperationControl *operation);
    bool begin_ensure_directory(const char *remote_path,
                                char *error_out,
                                size_t error_out_size);
    bool advance_ensure_directory(char *error_out,
                                  size_t error_out_size);
    void reset_ensure_directory();
    void set_error(char *error_out,
                   size_t error_out_size,
                   const char *error) const;
    const char *last_error() const;
    uint32_t last_status(int status_hint = 0) const;
    void set_smb_error(char *error_out,
                       size_t error_out_size,
                       const char *operation,
                       int status_hint = 0) const;
    void configure_socket_options();

    char server_[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
    char share_[AC_STORAGE_SMB_SHARE_MAX] = {};
    char base_path_[AC_STORAGE_SMB_BASE_PATH_MAX] = {};
    char user_[AC_STORAGE_SMB_USER_MAX] = {};
    char password_[AC_STORAGE_SMB_PASSWORD_MAX] = {};

    std::atomic<uint8_t> host_resolution_state_{
        static_cast<uint8_t>(HostResolutionState::Idle)};
    char resolving_server_[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
    char resolved_address_[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
    char resolved_server_[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
    uint32_t host_resolution_started_ms_ = 0;
    int host_resolution_error_ = 0;

    smb2_context *ctx_ = nullptr;
    struct smb2fh *writer_ = nullptr;
    struct smb2fh *closing_writer_ = nullptr;
    StorageSmbPendingOperation *operation_ = nullptr;
    OperationKind operation_kind_ = OperationKind::None;
    bool connected_ = false;

    char ensure_target_[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    char ensure_current_[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    char ensure_mkdir_error_[AC_STORAGE_ERROR_MAX] = {};
    size_t ensure_cursor_ = 0;
    EnsureDirectoryPhase ensure_phase_ = EnsureDirectoryPhase::Idle;

    const uint8_t *write_data_ = nullptr;
    size_t write_size_ = 0;
    size_t write_offset_ = 0;
    size_t write_chunk_size_ = 0;
};

}  // namespace aircannect
