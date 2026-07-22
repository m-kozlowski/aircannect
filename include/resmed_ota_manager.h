#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "operation_outcome.h"
#include "resmed_firmware_preparer.h"
#include "rpc_request_port.h"

namespace aircannect {

class As11DeviceService;
class StoragePathPort;
class StorageStreamPort;

enum class ResmedOtaPhase {
    Idle,
    Opening,
    Initiating,
    Ready,
    Uploading,
    Uploaded,
    Checking,
    Verified,
    Applying,
    Complete,
    Error,
};

struct ResmedOtaStatus {
    ResmedOtaPhase phase = ResmedOtaPhase::Idle;
    bool waiting = false;
    size_t total_size = 0;
    size_t uploaded_bytes = 0;
    size_t xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    uint8_t progress_percent = 0;
    String filename;
    String expected_sha256;
    String computed_sha256;
    String apply_mode;
    String input_type;
    String target;
    String source_path;
    String last_result;
    String last_error;
};

class ResmedOtaManager {
public:
    bool begin(RpcRequestPort &rpc,
               As11DeviceService &device,
               StorageStreamPort &stream_port,
               StoragePathPort &path_port);
    void poll();

    bool begin_upload(size_t total_size,
                      const String &expected_sha256,
                      const String &filename);
    bool begin_prepared_upload(const ResmedPreparedFirmware &firmware);
    bool discard_prepared_firmware(const ResmedPreparedFirmware &firmware);
    bool submit_block(size_t offset, const String &hex_data);
    bool request_check();
    bool request_apply_plain(bool reset_settings, const String &confirm);
    bool request_apply_authenticated(const String &authentication,
                                     const String &confirm);
    void abort(const char *reason);

    bool active() const;
    bool transport_active() const;
    ResmedOtaStatus status() const;
    const char *phase_name() const;

private:
    class ScopedLock {
    public:
        ScopedLock(const ResmedOtaManager &manager, uint32_t timeout_ms);
        ~ScopedLock();
        explicit operator bool() const { return locked_; }

    private:
        const ResmedOtaManager &manager_;
        bool locked_ = false;
    };

    enum class WaitingFor {
        None,
        Initiate,
        Block,
        Check,
        Apply,
    };

    struct ColdState;

    bool begin_protocol(size_t total_size,
                        const String &expected_sha256,
                        const String &filename);
    bool queue_request(const char *method,
                       const std::string &params,
                       uint32_t timeout_ms);
    void poll_rpc_completion();
    void cancel_rpc_request();
    void handle_response(const std::string &payload);

    void poll_prepared_transfer();
    bool open_prepared_stream();
    bool fill_prepared_block();
    void finish_pending_block();
    void close_prepared_stream(bool complete);

    void schedule_prepared_cleanup();
    void poll_cleanup();
    void clear_cleanup();

    bool finish_hash();
    void clear_session();
    void set_error(const char *error);
    void update_progress();

    bool guard_device_idle_for_upgrade();
    bool device_idle_for_upgrade(const char **reason) const;
    bool lock(uint32_t timeout_ms) const;
    void unlock() const;

    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    StorageStreamPort *stream_port_ = nullptr;
    StoragePathPort *path_port_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    ColdState *cold_ = nullptr;

    // RPC protocol
    WaitingFor waiting_for_ = WaitingFor::None;
    OperationTicket rpc_ticket_;
    uint32_t rpc_generation_ = 0;
    size_t pending_block_offset_ = 0;
    size_t pending_block_bytes_ = 0;
    bool sha_started_ = false;
    bool sha_finished_ = false;
    uint32_t last_activity_ms_ = 0;
    mbedtls_sha256_context sha_ctx_;

    // Prepared storage source
    size_t prepared_block_bytes_ = 0;
    size_t prepared_block_wanted_ = 0;
    bool prepared_transfer_ = false;
    bool prepared_check_requested_ = false;

    // Transient source cleanup
    size_t cleanup_count_ = 0;
    size_t cleanup_index_ = 0;
    OperationTicket cleanup_ticket_;
    uint32_t cleanup_generation_ = 0;
};

}  // namespace aircannect
