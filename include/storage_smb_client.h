#pragma once

#include <stddef.h>
#include <stdint.h>

#include "background_operation_control.h"
#include "storage_path.h"

struct smb2_context;
struct smb2fh;

namespace aircannect {

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

class StorageSmbClient {
public:
    StorageSmbClient() = default;
    ~StorageSmbClient();

    bool configure(const char *endpoint,
                   const char *user,
                   const char *password,
                   char *error_out = nullptr,
                   size_t error_out_size = 0,
                   const BackgroundOperationControl *operation = nullptr);

    bool connect(char *error_out = nullptr,
                 size_t error_out_size = 0,
                 const BackgroundOperationControl *operation = nullptr);
    void disconnect(const BackgroundOperationControl *operation = nullptr);
    bool connected() const { return connected_; }

    bool make_remote_path(const char *absolute_path,
                          char *out,
                          size_t out_size) const;

    bool stat(const char *remote_path,
              StorageSmbRemoteStat &out,
              char *error_out = nullptr,
              size_t error_out_size = 0,
              const BackgroundOperationControl *operation = nullptr);
    bool ensure_directory(const char *remote_path,
                          char *error_out = nullptr,
                          size_t error_out_size = 0,
                          const BackgroundOperationControl *operation = nullptr);

    bool open_writer(const char *remote_path,
                     char *error_out = nullptr,
                     size_t error_out_size = 0,
                     const BackgroundOperationControl *operation = nullptr);
    int write(const uint8_t *data,
              size_t len,
              char *error_out = nullptr,
              size_t error_out_size = 0,
              const BackgroundOperationControl *operation = nullptr);
    bool close_writer(char *error_out = nullptr,
                      size_t error_out_size = 0,
                      const BackgroundOperationControl *operation = nullptr);
    void abort_connection();

private:
    bool parse_endpoint(const char *endpoint,
                        char *error_out,
                        size_t error_out_size);
    bool create_directory_once(const char *remote_path,
                               char *error_out,
                               size_t error_out_size,
                               const BackgroundOperationControl *operation);
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

    smb2_context *ctx_ = nullptr;
    struct smb2fh *writer_ = nullptr;
    bool connected_ = false;
};

}  // namespace aircannect
