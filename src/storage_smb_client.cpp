#include "storage_smb_client.h"

#include <Arduino.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <new>

#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>

#include "debug_log.h"
#include "string_util.h"

extern "C" {
#include "smb2/smb2.h"
#include "smb2/libsmb2.h"
}

namespace aircannect {
namespace {

static constexpr int SMB_COMMAND_TIMEOUT_SECONDS = 15;
static constexpr int SMB_POLL_INTERVAL_MS = 100;
static constexpr uint32_t SMB_EVENT_LOOP_TIMEOUT_MS =
    SMB_COMMAND_TIMEOUT_SECONDS * 1000UL;

class LwipCoreGuard {
public:
#ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
    LwipCoreGuard()
        : locked_(!sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER)) {
        if (locked_) LOCK_TCPIP_CORE();
    }

    ~LwipCoreGuard() {
        if (locked_) UNLOCK_TCPIP_CORE();
    }

private:
    bool locked_ = false;
#else
    LwipCoreGuard() = default;
#endif
};

struct SmbAsyncCbData {
    bool finished = false;
    bool orphaned = false;
    int status = 0;
    void *result = nullptr;
};

void smb_generic_cb(struct smb2_context *,
                    int status,
                    void *command_data,
                    void *private_data) {
    SmbAsyncCbData *cb = static_cast<SmbAsyncCbData *>(private_data);
    if (!cb) return;
    cb->status = status;
    cb->result = command_data;
    cb->finished = true;
    if (cb->orphaned) delete cb;
}

void feed_task() {
    taskYIELD();
    vTaskDelay(1);
}

SmbAsyncCbData *smb_alloc_cb() {
    return new (std::nothrow) SmbAsyncCbData();
}

int smb_finish_cb(SmbAsyncCbData *cb, int loop_rc,
                  void **result_out = nullptr) {
    if (!cb) return -ENOMEM;
    if (loop_rc < 0) {
        cb->orphaned = true;
        return loop_rc;
    }
    const int status = cb->status;
    if (result_out) *result_out = cb->result;
    delete cb;
    return status;
}

int smb_run_event_loop(
    struct smb2_context *ctx,
    SmbAsyncCbData *cb,
    const BackgroundOperationControl *operation) {
    if (!cb) return -ENOMEM;
    const uint32_t started_ms = millis();
    while (!cb->finished) {
        if (operation) {
            const BackgroundOperationStop reason =
                operation->stop_reason(millis());
            if (reason == BackgroundOperationStop::Aborted) {
                return -ECANCELED;
            }
            if (reason == BackgroundOperationStop::Deadline) {
                return -ETIMEDOUT;
            }
        }

        const uint32_t elapsed_ms =
            static_cast<uint32_t>(millis() - started_ms);
        if (elapsed_ms >= SMB_EVENT_LOOP_TIMEOUT_MS) return -ETIMEDOUT;

        const int fd = smb2_get_fd(ctx);
        if (fd < 0) return -1;

        pollfd pfd = {};
        pfd.fd = fd;
        pfd.events = smb2_which_events(ctx);

        uint32_t poll_ms = SMB_POLL_INTERVAL_MS;
        const uint32_t remaining_ms = SMB_EVENT_LOOP_TIMEOUT_MS - elapsed_ms;
        if (remaining_ms < poll_ms) poll_ms = remaining_ms;

        const int rc = poll(&pfd, 1, static_cast<int>(poll_ms));
        if (rc < 0) return errno ? -errno : -1;
        if (pfd.revents && smb2_service(ctx, pfd.revents) < 0) {
            return cb->finished ? 0 : -1;
        }
        feed_task();
    }
    return 0;
}

template <typename Submit>
int smb_submit_ev(struct smb2_context *ctx,
                  Submit submit,
                  void **result_out = nullptr,
                  const BackgroundOperationControl *operation = nullptr) {
    SmbAsyncCbData *cb = smb_alloc_cb();
    if (!cb) return -ENOMEM;
    const int rc = submit(cb);
    if (rc < 0) {
        delete cb;
        return rc;
    }
    return smb_finish_cb(cb, smb_run_event_loop(ctx, cb, operation),
                         result_out);
}

int smb_connect_share_ev(struct smb2_context *ctx,
                         const char *server,
                         const char *share,
                         const char *user,
                         const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx, server, share, user](SmbAsyncCbData *cb) {
        return smb2_connect_share_async(ctx, server, share, user,
                                        smb_generic_cb, cb);
    }, nullptr, operation);
}

int smb_disconnect_share_ev(
    struct smb2_context *ctx,
    const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx](SmbAsyncCbData *cb) {
        return smb2_disconnect_share_async(ctx, smb_generic_cb, cb);
    }, nullptr, operation);
}

int smb_stat_ev(struct smb2_context *ctx,
                const char *path,
                smb2_stat_64 *st,
                const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx, path, st](SmbAsyncCbData *cb) {
        return smb2_stat_async(ctx, path, st, smb_generic_cb, cb);
    }, nullptr, operation);
}

int smb_mkdir_ev(struct smb2_context *ctx,
                 const char *path,
                 const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx, path](SmbAsyncCbData *cb) {
        return smb2_mkdir_async(ctx, path, smb_generic_cb, cb);
    }, nullptr, operation);
}

struct smb2fh *smb_open_ev(struct smb2_context *ctx,
                           const char *path,
                           int flags,
                           const BackgroundOperationControl *operation) {
    void *result = nullptr;
    const int status = smb_submit_ev(
        ctx, [ctx, path, flags](SmbAsyncCbData *cb) {
            return smb2_open_async(ctx, path, flags, smb_generic_cb, cb);
        }, &result, operation);
    if (status < 0) return nullptr;
    return static_cast<struct smb2fh *>(result);
}

int smb_close_ev(struct smb2_context *ctx,
                 struct smb2fh *fh,
                 const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx, fh](SmbAsyncCbData *cb) {
        return smb2_close_async(ctx, fh, smb_generic_cb, cb);
    }, nullptr, operation);
}

bool smb_queue_close_for_destroy(struct smb2_context *ctx, struct smb2fh *fh) {
    // destroy_context invokes queued PDU callbacks with SHUTDOWN; queue close
    // first so libsmb2's close_cb frees the file handle without a network wait.
    SmbAsyncCbData *cb = smb_alloc_cb();
    if (!cb) return false;
    cb->orphaned = true;
    const int rc = smb2_close_async(ctx, fh, smb_generic_cb, cb);
    if (rc < 0) {
        cb->orphaned = false;
        delete cb;
        return false;
    }
    return true;
}

int smb_write_ev(struct smb2_context *ctx,
                 struct smb2fh *fh,
                 const uint8_t *data,
                 uint32_t len,
                 const BackgroundOperationControl *operation) {
    return smb_submit_ev(ctx, [ctx, fh, data, len](SmbAsyncCbData *cb) {
        return smb2_write_async(ctx, fh, data, len, smb_generic_cb, cb);
    }, nullptr, operation);
}

bool is_path_not_found_error(const char *error) {
    return error &&
           (strstr(error, "STATUS_OBJECT_NAME_NOT_FOUND") ||
            strstr(error, "STATUS_OBJECT_PATH_NOT_FOUND") ||
            strstr(error, "STATUS_NO_SUCH_FILE") ||
            strstr(error, "PATH_NOT_FOUND") ||
            strstr(error, "No such file") ||
            strstr(error, "not found"));
}

bool is_path_not_found_status(uint32_t status) {
    return status == SMB2_STATUS_NO_SUCH_FILE ||
           status == SMB2_STATUS_OBJECT_NAME_NOT_FOUND ||
           status == SMB2_STATUS_OBJECT_PATH_NOT_FOUND ||
           status == SMB2_STATUS_OBJECT_PATH_INVALID;
}

bool is_path_not_found_status_hint(int status) {
    return status == -ENOENT || status == -ENOTDIR;
}

bool valid_endpoint_char(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return u >= 0x20 && u < 0x7F && c != '\\';
}

bool split_server_authority(const char *server,
                            char *host,
                            size_t host_size,
                            const char **port_suffix) {
    if (!server || !server[0] || !host || host_size == 0 || !port_suffix) {
        return false;
    }

    *port_suffix = "";
    const char *host_start = server;
    size_t host_len = strlen(server);

    if (server[0] == '[') {
        const char *closing = strchr(server + 1, ']');
        if (!closing) return false;

        host_start = server + 1;
        host_len = static_cast<size_t>(closing - host_start);
        if (closing[1] != '\0') {
            if (closing[1] != ':' || closing[2] == '\0') return false;
            *port_suffix = closing + 1;
        }
    } else {
        const char *first_colon = strchr(server, ':');
        const char *last_colon = strrchr(server, ':');
        if (first_colon && first_colon == last_colon) {
            if (first_colon == server || first_colon[1] == '\0') return false;
            host_len = static_cast<size_t>(first_colon - server);
            *port_suffix = first_colon;
        }
    }

    if (host_len == 0 || host_len >= host_size) return false;
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    return true;
}

bool append_path_segment(char *path,
                         size_t path_size,
                         const char *segment,
                         size_t segment_len) {
    if (!path || !segment || segment_len == 0) return false;
    const size_t len = strlen(path);
    const size_t slash = len == 0 ? 0 : 1;
    if (len + slash + segment_len >= path_size) return false;
    if (slash) path[len] = '/';
    memcpy(path + len + slash, segment, segment_len);
    path[len + slash + segment_len] = '\0';
    return true;
}

}  // namespace

struct StorageSmbDnsCallback {
    static void complete(const char *,
                         const ip_addr_t *address,
                         void *context) {
        StorageSmbClient *client =
            static_cast<StorageSmbClient *>(context);
        if (!client) return;
        client->publish_host_resolution(address,
                                        address ? ERR_OK : ERR_TIMEOUT);
    }
};

StorageSmbClient::~StorageSmbClient() {
    disconnect();
}

void StorageSmbClient::set_error(char *error_out,
                                 size_t error_out_size,
                                 const char *error) const {
    copy_cstr(error_out, error_out_size, error ? error : "");
}

const char *StorageSmbClient::last_error() const {
    if (!ctx_) return "not_connected";
    const char *err = smb2_get_error(ctx_);
    return err && *err ? err : "smb_error";
}

uint32_t StorageSmbClient::last_status(int status_hint) const {
    if (status_hint != 0) {
        return static_cast<uint32_t>(status_hint);
    }
    return ctx_ ? static_cast<uint32_t>(smb2_get_nterror(ctx_)) : 0;
}

void StorageSmbClient::set_smb_error(char *error_out,
                                     size_t error_out_size,
                                     const char *operation,
                                     int status_hint) const {
    if (!error_out || error_out_size == 0) return;
    if (!operation || !*operation) operation = "smb";
    if (status_hint == -ECANCELED) {
        set_error(error_out, error_out_size, "preempted");
        return;
    }
    if (status_hint == -ETIMEDOUT) {
        set_error(error_out, error_out_size, "operation_timeout");
        return;
    }

    const char *err = ctx_ ? smb2_get_error(ctx_) : "";
    if (err && *err) {
        snprintf(error_out, error_out_size, "%s:%s", operation, err);
        return;
    }

    if (status_hint < 0 && status_hint > -4096 &&
        (!ctx_ || smb2_get_nterror(ctx_) == 0)) {
        const int local_errno = -status_hint;
        snprintf(error_out, error_out_size, "%s:errno_%d:%s",
                 operation, local_errno, strerror(local_errno));
        return;
    }

    const uint32_t status = last_status(status_hint);
    if (status != 0) {
        const char *name = nterror_to_str(status);
        if (name && *name && strcmp(name, "Unknown error") != 0) {
            snprintf(error_out, error_out_size, "%s:%s", operation, name);
        } else {
            snprintf(error_out, error_out_size, "%s:0x%08lx", operation,
                     static_cast<unsigned long>(status));
        }
        return;
    }

    snprintf(error_out, error_out_size, "%s:smb_error", operation);
}

bool StorageSmbClient::parse_endpoint(const char *endpoint,
                                      char *error_out,
                                      size_t error_out_size) {
    server_[0] = '\0';
    share_[0] = '\0';
    base_path_[0] = '\0';

    if (!endpoint || strncmp(endpoint, "//", 2) != 0) {
        set_error(error_out, error_out_size, "bad_endpoint");
        return false;
    }
    for (const char *p = endpoint; *p; ++p) {
        if (!valid_endpoint_char(*p)) {
            set_error(error_out, error_out_size, "bad_endpoint_char");
            return false;
        }
    }

    const char *host_start = endpoint + 2;
    const char *share_start = strchr(host_start, '/');
    if (!share_start || share_start == host_start) {
        set_error(error_out, error_out_size, "missing_smb_host");
        return false;
    }
    const size_t host_len = static_cast<size_t>(share_start - host_start);
    if (host_len >= sizeof(server_)) {
        set_error(error_out, error_out_size, "smb_host_too_long");
        return false;
    }
    memcpy(server_, host_start, host_len);
    server_[host_len] = '\0';

    share_start++;
    const char *base_start = strchr(share_start, '/');
    const size_t share_len =
        base_start ? static_cast<size_t>(base_start - share_start)
                   : strlen(share_start);
    if (share_len == 0 || share_len >= sizeof(share_)) {
        set_error(error_out, error_out_size, "bad_smb_share");
        return false;
    }
    memcpy(share_, share_start, share_len);
    share_[share_len] = '\0';

    if (base_start && base_start[1] != '\0') {
        base_start++;
        size_t base_len = strlen(base_start);
        while (base_len > 0 && base_start[base_len - 1] == '/') base_len--;
        if (base_len >= sizeof(base_path_)) {
            set_error(error_out, error_out_size, "smb_base_too_long");
            return false;
        }
        memcpy(base_path_, base_start, base_len);
        base_path_[base_len] = '\0';
    }

    return true;
}

bool StorageSmbClient::configure(const char *endpoint,
                                 const char *user,
                                 const char *password,
                                 char *error_out,
                                 size_t error_out_size,
                                 const BackgroundOperationControl *operation) {
    disconnect(operation);
    if (!parse_endpoint(endpoint, error_out, error_out_size)) return false;
    copy_cstr(user_, sizeof(user_), user);
    copy_cstr(password_, sizeof(password_), password);

    const HostResolutionState state = static_cast<HostResolutionState>(
        host_resolution_state_.load(std::memory_order_acquire));
    if (state != HostResolutionState::Pending) reset_host_resolution();

    set_error(error_out, error_out_size, "");
    return true;
}

void StorageSmbClient::publish_host_resolution(const void *address,
                                               int error) {
    const ip_addr_t *ip = static_cast<const ip_addr_t *>(address);
    char raw_address[IPADDR_STRLEN_MAX] = {};
    if (!ip || !ipaddr_ntoa_r(ip, raw_address, sizeof(raw_address))) {
        resolved_address_[0] = '\0';
        host_resolution_error_ = error ? error : ERR_VAL;
        host_resolution_state_.store(
            static_cast<uint8_t>(HostResolutionState::Error),
            std::memory_order_release);
        return;
    }

    if (IP_IS_V6(ip)) {
        const int written = snprintf(resolved_address_,
                                     sizeof(resolved_address_),
                                     "[%s]",
                                     raw_address);
        if (written <= 0 ||
            static_cast<size_t>(written) >= sizeof(resolved_address_)) {
            resolved_address_[0] = '\0';
            host_resolution_error_ = ERR_BUF;
            host_resolution_state_.store(
                static_cast<uint8_t>(HostResolutionState::Error),
                std::memory_order_release);
            return;
        }
    } else {
        copy_cstr(resolved_address_, sizeof(resolved_address_), raw_address);
    }

    host_resolution_error_ = ERR_OK;
    host_resolution_state_.store(
        static_cast<uint8_t>(HostResolutionState::Ready),
        std::memory_order_release);
}

void StorageSmbClient::reset_host_resolution() {
    resolving_server_[0] = '\0';
    resolved_address_[0] = '\0';
    resolved_server_[0] = '\0';
    host_resolution_started_ms_ = 0;
    host_resolution_error_ = ERR_OK;
    host_resolution_state_.store(
        static_cast<uint8_t>(HostResolutionState::Idle),
        std::memory_order_release);
}

StorageSmbHostResult StorageSmbClient::resolve_host(char *error_out,
                                                    size_t error_out_size) {
    if (!server_[0]) {
        set_error(error_out, error_out_size, "not_configured");
        return StorageSmbHostResult::Error;
    }

    HostResolutionState state = static_cast<HostResolutionState>(
        host_resolution_state_.load(std::memory_order_acquire));
    if (state == HostResolutionState::Pending) {
        return StorageSmbHostResult::Waiting;
    }

    if (state != HostResolutionState::Idle &&
        strcmp(resolving_server_, server_) != 0) {
        reset_host_resolution();
        state = HostResolutionState::Idle;
    }

    if (state == HostResolutionState::Error) {
        char error[AC_STORAGE_ERROR_MAX] = {};
        snprintf(error,
                 sizeof(error),
                 "dns_lookup_failed:%d",
                 host_resolution_error_);
        set_error(error_out, error_out_size, error);
        return StorageSmbHostResult::Error;
    }

    if (state == HostResolutionState::Ready) {
        char host[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
        const char *port_suffix = "";
        if (!split_server_authority(server_,
                                    host,
                                    sizeof(host),
                                    &port_suffix)) {
            set_error(error_out, error_out_size, "bad_smb_host");
            return StorageSmbHostResult::Error;
        }

        const int written = snprintf(resolved_server_,
                                     sizeof(resolved_server_),
                                     "%s%s",
                                     resolved_address_,
                                     port_suffix);
        if (written <= 0 ||
            static_cast<size_t>(written) >= sizeof(resolved_server_)) {
            set_error(error_out, error_out_size, "resolved_host_too_long");
            return StorageSmbHostResult::Error;
        }

        Log::logf(CAT_STORAGE,
                  LOG_INFO,
                  "[SMB] resolved host=%s address=%s ms=%lu\n",
                  host,
                  resolved_address_,
                  static_cast<unsigned long>(
                      millis() - host_resolution_started_ms_));
        set_error(error_out, error_out_size, "");
        return StorageSmbHostResult::Ready;
    }

    char host[AC_STORAGE_SMB_ENDPOINT_HOST_MAX] = {};
    const char *port_suffix = "";
    if (!split_server_authority(server_,
                                host,
                                sizeof(host),
                                &port_suffix)) {
        set_error(error_out, error_out_size, "bad_smb_host");
        return StorageSmbHostResult::Error;
    }

    copy_cstr(resolving_server_, sizeof(resolving_server_), server_);
    host_resolution_started_ms_ = millis();
    host_resolution_state_.store(
        static_cast<uint8_t>(HostResolutionState::Pending),
        std::memory_order_release);
    Log::logf(CAT_STORAGE, LOG_INFO, "[SMB] resolving host=%s\n", host);

    ip_addr_t address = {};
    err_t rc = ERR_OK;
    {
        LwipCoreGuard guard;
        rc = dns_gethostbyname_addrtype(
            host,
            &address,
            StorageSmbDnsCallback::complete,
            this,
            LWIP_DNS_ADDRTYPE_IPV4_IPV6);
    }

    if (rc == ERR_OK) {
        publish_host_resolution(&address, ERR_OK);
        return resolve_host(error_out, error_out_size);
    }
    if (rc == ERR_INPROGRESS) return StorageSmbHostResult::Waiting;

    publish_host_resolution(nullptr, rc);
    return resolve_host(error_out, error_out_size);
}

void StorageSmbClient::configure_socket_options() {
    const int fd = smb2_get_fd(ctx_);
    if (fd < 0) return;

    const timeval tv = {SMB_COMMAND_TIMEOUT_SECONDS, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SMB] setsockopt(SO_SNDTIMEO) errno=%d\n",
                  errno);
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SMB] setsockopt(SO_RCVTIMEO) errno=%d\n",
                  errno);
    }

    const int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        Log::logf(CAT_STORAGE,
                  LOG_WARN,
                  "[SMB] setsockopt(TCP_NODELAY) errno=%d\n",
                  errno);
    }
}

bool StorageSmbClient::connect(
    char *error_out,
    size_t error_out_size,
    const BackgroundOperationControl *operation) {
    if (connected_) return true;
    if (!server_[0] || !share_[0]) {
        set_error(error_out, error_out_size, "not_configured");
        return false;
    }
    if (!resolved_server_[0]) {
        set_error(error_out, error_out_size, "host_not_resolved");
        return false;
    }

    ctx_ = smb2_init_context();
    if (!ctx_) {
        set_error(error_out, error_out_size, "smb_context_alloc");
        Log::logf(CAT_STORAGE, LOG_ERROR,
                  "[SMB] context allocation failed\n");
        return false;
    }

    smb2_set_timeout(ctx_, SMB_COMMAND_TIMEOUT_SECONDS);
    smb2_set_security_mode(ctx_, SMB2_NEGOTIATE_SIGNING_ENABLED);
    if (user_[0]) {
        smb2_set_user(ctx_, user_);
        smb2_set_password(ctx_, password_);
    }

    Log::logf(CAT_STORAGE, LOG_INFO,
              "[SMB] connecting //%s/%s\n", server_, share_);
    const int rc = smb_connect_share_ev(ctx_, resolved_server_, share_, nullptr,
                                        operation);
    if (rc != 0) {
        char error[AC_STORAGE_ERROR_MAX] = {};
        set_smb_error(error, sizeof(error), "connect", rc);
        set_error(error_out, error_out_size, error);
        Log::logf(CAT_STORAGE, LOG_WARN,
                  "[SMB] connect failed error=%s\n", error);
        smb2_destroy_context(ctx_);
        ctx_ = nullptr;
        return false;
    }

    connected_ = true;
    configure_socket_options();
    set_error(error_out, error_out_size, "");
    return true;
}

void StorageSmbClient::disconnect(
    const BackgroundOperationControl *operation) {
    if (!ctx_) {
        connected_ = false;
        writer_ = nullptr;
        return;
    }

    if (writer_) {
        (void)smb_close_ev(ctx_, writer_, operation);
        writer_ = nullptr;
    }
    if (connected_) (void)smb_disconnect_share_ev(ctx_, operation);
    smb2_destroy_context(ctx_);
    ctx_ = nullptr;
    connected_ = false;
}

bool StorageSmbClient::make_remote_path(const char *absolute_path,
                                        char *out,
                                        size_t out_size) const {
    if (!absolute_path || absolute_path[0] != '/' || !out || out_size == 0) {
        return false;
    }
    const char *rel = absolute_path[1] ? absolute_path + 1 : "";
    int written = 0;
    if (base_path_[0] && rel[0]) {
        written = snprintf(out, out_size, "%s/%s", base_path_, rel);
    } else if (base_path_[0]) {
        written = snprintf(out, out_size, "%s", base_path_);
    } else {
        written = snprintf(out, out_size, "%s", rel);
    }
    return written >= 0 && static_cast<size_t>(written) < out_size;
}

bool StorageSmbClient::stat(const char *remote_path,
                            StorageSmbRemoteStat &out,
                            char *error_out,
                            size_t error_out_size,
                            const BackgroundOperationControl *operation) {
    out = StorageSmbRemoteStat();
    if (!connected_ || !ctx_) {
        set_error(error_out, error_out_size, "not_connected");
        return false;
    }
    if (!remote_path || remote_path[0] == '\0') {
        out.exists = true;
        out.directory = true;
        set_error(error_out, error_out_size, "");
        return true;
    }

    smb2_stat_64 st = {};
    const int rc = smb_stat_ev(ctx_, remote_path, &st, operation);
    if (rc == 0) {
        out.exists = true;
        out.directory = st.smb2_type == SMB2_TYPE_DIRECTORY;
        out.size = st.smb2_size;
        set_error(error_out, error_out_size, "");
        return true;
    }
    const char *err = last_error();
    if (is_path_not_found_status_hint(rc) ||
        is_path_not_found_error(err) ||
        is_path_not_found_status(last_status(rc))) {
        set_error(error_out, error_out_size, "");
        return true;
    }
    set_smb_error(error_out, error_out_size, "stat", rc);
    return false;
}

bool StorageSmbClient::create_directory_once(const char *remote_path,
                                             char *error_out,
                                             size_t error_out_size,
                                             const BackgroundOperationControl *operation) {
    StorageSmbRemoteStat st;
    if (!stat(remote_path, st, error_out, error_out_size, operation)) {
        return false;
    }
    if (st.exists) {
        if (!st.directory) {
            set_error(error_out, error_out_size, "remote_not_directory");
            return false;
        }
        return true;
    }

    const int rc = smb_mkdir_ev(ctx_, remote_path, operation);
    if (rc == 0) {
        set_error(error_out, error_out_size, "");
        return true;
    }

    // Another client may have raced us; verify before reporting failure.
    if (stat(remote_path, st, nullptr, 0, operation) &&
        st.exists && st.directory) {
        set_error(error_out, error_out_size, "");
        return true;
    }
    set_smb_error(error_out, error_out_size, "mkdir", rc);
    Log::logf(CAT_STORAGE,
              LOG_WARN,
              "[SMB] mkdir failed path=%s error=%s\n",
              remote_path ? remote_path : "--",
              error_out && error_out[0] ? error_out : "mkdir_failed");
    return false;
}

bool StorageSmbClient::ensure_directory(const char *remote_path,
                                        char *error_out,
                                        size_t error_out_size,
                                        const BackgroundOperationControl *operation) {
    if (!connected_ || !ctx_) {
        set_error(error_out, error_out_size, "not_connected");
        return false;
    }
    if (!remote_path || remote_path[0] == '\0') {
        set_error(error_out, error_out_size, "");
        return true;
    }

    char current[AC_STORAGE_SMB_REMOTE_PATH_MAX] = {};
    const char *segment = remote_path;
    for (const char *p = remote_path;; ++p) {
        if (*p != '/' && *p != '\0') continue;
        const size_t len = static_cast<size_t>(p - segment);
        if (len > 0) {
            if (!append_path_segment(current, sizeof(current), segment, len)) {
                set_error(error_out, error_out_size, "remote_path_too_long");
                return false;
            }
            if (!create_directory_once(current, error_out, error_out_size,
                                       operation)) {
                return false;
            }
        }
        if (*p == '\0') break;
        segment = p + 1;
    }
    set_error(error_out, error_out_size, "");
    return true;
}

bool StorageSmbClient::open_writer(const char *remote_path,
                                   char *error_out,
                                   size_t error_out_size,
                                   const BackgroundOperationControl *operation) {
    if (!connected_ || !ctx_) {
        set_error(error_out, error_out_size, "not_connected");
        return false;
    }
    if (writer_) {
        set_error(error_out, error_out_size, "writer_busy");
        return false;
    }
    if (!remote_path || remote_path[0] == '\0') {
        set_error(error_out, error_out_size, "bad_remote_path");
        return false;
    }

    writer_ = smb_open_ev(ctx_, remote_path,
                          O_WRONLY | O_CREAT | O_TRUNC, operation);
    if (!writer_) {
        set_smb_error(error_out, error_out_size, "open", 0);
        return false;
    }
    set_error(error_out, error_out_size, "");
    return true;
}

int StorageSmbClient::write(const uint8_t *data,
                            size_t len,
                            char *error_out,
                            size_t error_out_size,
                            const BackgroundOperationControl *operation) {
    if (!connected_ || !ctx_ || !writer_) {
        set_error(error_out, error_out_size, "writer_not_open");
        return -1;
    }
    if (!data || len == 0 || len > UINT32_MAX) {
        set_error(error_out, error_out_size, "bad_write");
        return -1;
    }

    size_t offset = 0;
    uint32_t max_write = smb2_get_max_write_size(ctx_);
    if (max_write == 0) max_write = UINT32_MAX;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_write) chunk = max_write;
        if (chunk > UINT32_MAX) chunk = UINT32_MAX;
        const int written = smb_write_ev(ctx_,
                                         writer_,
                                         data + offset,
                                         static_cast<uint32_t>(chunk),
                                         operation);
        if (written < 0) {
            set_smb_error(error_out, error_out_size, "write", written);
            return -1;
        }
        if (written == 0 || static_cast<size_t>(written) > chunk) {
            set_error(error_out, error_out_size, "write_short");
            return -1;
        }
        offset += static_cast<size_t>(written);
    }
    set_error(error_out, error_out_size, "");
    return static_cast<int>(offset);
}

bool StorageSmbClient::close_writer(
    char *error_out,
    size_t error_out_size,
    const BackgroundOperationControl *operation) {
    if (!writer_) {
        set_error(error_out, error_out_size, "");
        return true;
    }
    struct smb2fh *fh = writer_;
    writer_ = nullptr;
    const int rc = smb_close_ev(ctx_, fh, operation);
    if (rc < 0) {
        set_smb_error(error_out, error_out_size, "close", rc);
        return false;
    }
    set_error(error_out, error_out_size, "");
    return true;
}

void StorageSmbClient::abort_connection() {
    if (!ctx_) {
        connected_ = false;
        writer_ = nullptr;
        return;
    }
    if (writer_) {
        struct smb2fh *fh = writer_;
        writer_ = nullptr;
        if (!smb_queue_close_for_destroy(ctx_, fh)) {
            Log::logf(CAT_STORAGE, LOG_WARN,
                      "[SMB] failed to queue writer close before abort\n");
        }
    }
    writer_ = nullptr;
    connected_ = false;
    smb2_destroy_context(ctx_);
    ctx_ = nullptr;
}

}  // namespace aircannect
