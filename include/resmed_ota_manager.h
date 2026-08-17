#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <atomic>
#include <stdint.h>
#include <string>

#include "board.h"
#include "operation_outcome.h"
#include "resmed_firmware_preparer.h"
#include "rpc_request_port.h"

namespace aircannect {

static constexpr char AC_RESMED_DUMP_BOOTLOADER_CONFIRM[] =
    "INSTALL_PATCHED_BOOTLOADER";

class As11DeviceService;
class As11ServiceManager;
struct As11ServicePacketHeader;
class LargeByteBuffer;
class StoragePathPort;
class StorageStreamPort;
class StorageUploadPort;

enum class ResmedOtaOperation : uint8_t {
    None,
    Install,
    Dump,
};

const char *resmed_ota_operation_name(ResmedOtaOperation operation);

enum class ResmedOtaPhase {
    Idle,
    ReadingIdentity,
    CheckingStorage,
    Opening,
    EnteringService,
    Dumping,
    Publishing,
    BootloaderRequired,
    PreparingBootloader,
    Erasing,
    Initiating,
    Ready,
    Uploading,
    Uploaded,
    Checking,
    Verified,
    Applying,
    Resetting,
    Complete,
    Error,
};

struct ResmedOtaStatus {
    ResmedOtaPhase phase = ResmedOtaPhase::Idle;
    ResmedOtaOperation operation = ResmedOtaOperation::None;
    bool waiting = false;
    bool confirmation_required = false;
    bool recovery_available = false;
    size_t total_size = 0;
    size_t uploaded_bytes = 0;
    size_t xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    uint8_t progress_percent = 0;
    String filename;
    String expected_sha256;
    String computed_sha256;
    String apply_mode;
    String input_type;
    ResmedFirmwareInstallTransport transport =
        AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT;
    String target;
    String source_path;
    String output_path;
    String recovery_path;
    String last_result;
    String last_error;
};

class ResmedOtaManager {
public:
    bool begin(RpcRequestPort &rpc,
               As11DeviceService &device,
               As11ServiceManager &service,
               ResmedFirmwarePreparer &preparer,
               StorageStreamPort &stream_port,
               StoragePathPort &path_port,
               StorageUploadPort &upload_port);
    void poll();

    bool begin_upload(size_t total_size,
                      const String &expected_sha256,
                      const String &filename);
    bool begin_prepared_install(const ResmedPreparedFirmware &firmware);
    bool discard_prepared_firmware(const ResmedPreparedFirmware &firmware);
    bool request_firmware_dump();
    bool confirm_dump_bootloader(const String &confirm);
    bool submit_block(size_t offset, const String &hex_data);
    bool request_check();
    bool request_apply_plain(bool reset_settings, const String &confirm);
    bool request_apply_authenticated(const String &authentication,
                                     const String &confirm);
    void abort(const char *reason);
    bool reset_terminal_state();

    bool active() const;
    bool transport_active() const;
    bool storage_upload_active() const;
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

    enum class ServiceWaitingFor : uint8_t {
        None,
        Enter,
        Read,
        Erase,
        Write,
        WriteLz4,
        Reset,
    };

    struct ColdState;

    enum class DumpPathCheck : uint8_t {
        None,
        Output,
        Bootloader,
    };

    bool begin_protocol(size_t total_size,
                        const String &expected_sha256,
                        const String &filename);
    bool queue_plain_apply(bool reset_settings);
    bool queue_request(const char *method,
                       const std::string &params,
                       uint32_t timeout_ms);
    void poll_rpc_completion();
    void cancel_rpc_request();
    void handle_response(RpcPayloadView payload);

    void poll_preparer_result();
    bool begin_recovery_install(const ResmedPreparedFirmware &firmware);
    void poll_recovery_boot();

    void poll_firmware_dump();
    bool begin_dump_identity_refresh(bool after_recovery);
    bool capture_dump_identity();
    bool request_dump_path_check(DumpPathCheck check);
    void poll_dump_path_check();
    bool begin_dump_service();
    bool begin_dump_upload();
    void poll_dump_upload();
    bool submit_dump_read();
    bool submit_dump_chunk();
    void finish_dump_read(const std::shared_ptr<const LargeByteBuffer> &response,
                          const As11ServicePacketHeader &header);
    void require_dump_bootloader();
    void finish_firmware_dump();
    void cancel_dump_upload();

    bool begin_service_install();
    void poll_service_completion();
    void poll_service_transfer();
    bool submit_service_request(uint8_t command,
                                const uint8_t *payload,
                                size_t payload_size,
                                ServiceWaitingFor waiting_for);
    void handle_service_response(
        const std::shared_ptr<const LargeByteBuffer> &response);
    bool submit_service_erase();
    bool submit_service_write();
    bool submit_service_write_lz4(bool &submitted);
    bool submit_service_reset();
    void poll_service_reset();
    void log_service_progress();
    void finish_service_install();
    void release_service();

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
    void finish_error(const char *error);
    void update_progress();

    bool guard_device_idle_for_upgrade();
    bool device_idle_for_upgrade(const char **reason) const;
    bool lock(uint32_t timeout_ms) const;
    void unlock() const;

    RpcRequestPort *rpc_ = nullptr;
    As11DeviceService *device_ = nullptr;
    As11ServiceManager *service_ = nullptr;
    ResmedFirmwarePreparer *preparer_ = nullptr;
    StorageStreamPort *stream_port_ = nullptr;
    StoragePathPort *path_port_ = nullptr;
    StorageUploadPort *upload_port_ = nullptr;
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

    // Bootloader service protocol
    ServiceWaitingFor service_waiting_for_ = ServiceWaitingFor::None;
    uint32_t service_erase_offset_ = 0;
    size_t service_pending_bytes_ = 0;
    uint16_t service_sequence_ = 0;
    bool service_owned_ = false;
    bool service_write_lz4_supported_ = true;
    uint8_t service_reset_attempts_ = 0;
    uint8_t service_next_progress_percent_ = 0;
    uint32_t service_started_ms_ = 0;
    uint32_t service_reset_accepted_ms_ = 0;
    uint32_t service_reset_boot_revision_ = 0;

    // Firmware dump and bootloader recovery
    OperationTicket dump_path_ticket_;
    DumpPathCheck dump_path_check_ = DumpPathCheck::None;
    uint32_t dump_path_generation_ = 0;
    uint32_t dump_identity_revision_ = 0;
    uint32_t dump_identity_started_ms_ = 0;
    uint32_t dump_upload_id_ = 0;
    uint32_t dump_upload_generation_ = 0;
    std::atomic<bool> dump_upload_active_{false};
    uint32_t dump_flash_start_ = 0;
    uint32_t dump_read_offset_ = 0;
    size_t dump_read_bytes_ = 0;
    size_t dump_buffer_bytes_ = 0;
    uint32_t dump_chunk_offset_ = 0;
    bool dump_service_entered_ = false;
    bool dump_recovery_prepare_pending_ = false;
    bool dump_recovery_install_ = false;
    bool dump_error_reset_pending_ = false;
    uint32_t recovery_boot_revision_ = 0;
    uint32_t recovery_boot_started_ms_ = 0;

    // Prepared storage source
    size_t prepared_block_bytes_ = 0;
    size_t prepared_block_wanted_ = 0;
    bool prepared_transfer_ = false;
    bool apply_after_check_ = false;

    // Transient source cleanup
    size_t cleanup_count_ = 0;
    size_t cleanup_index_ = 0;
    OperationTicket cleanup_ticket_;
    uint32_t cleanup_generation_ = 0;
};

}  // namespace aircannect
