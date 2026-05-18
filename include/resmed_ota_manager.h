#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "rpc_arbiter.h"

namespace aircannect {

enum class ResmedOtaPhase {
    Idle,
    Staging,
    Staged,
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
    String staged_path;
    String last_result;
    String last_error;
};

class ResmedOtaManager {
public:
    void begin(RpcArbiter &arbiter);
    void poll();

    bool begin_upload(size_t total_size,
                      const String &expected_sha256,
                      const String &filename);
    bool submit_block(size_t offset, const String &hex_data);
    bool request_check();
    bool request_apply_plain(bool reset_settings, const String &confirm);
    bool request_apply_authenticated(const String &authentication,
                                     const String &confirm);
    bool begin_staged_upload(size_t input_size,
                             const String &filename,
                             const String &magic_hex);
    bool write_staged_upload(size_t offset,
                             const uint8_t *data,
                             size_t len);
    bool finish_staged_upload();
    bool start_staged_upload();
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

    bool queue_request(const char *method,
                       const std::string &params,
                       uint32_t timeout_ms);
    void handle_event(const RpcEvent &event);
    void handle_response(const std::string &payload);

    void finish_pending_block();
    void pump_staged_upload();
    bool finish_hash();
    void clear_session();
    void close_staging_files();
    void set_error(const char *error);
    void set_error(const String &error);
    void update_progress();

    bool configure_staged_input(size_t input_size,
                                const String &filename,
                                const String &magic_hex);
    bool write_staging_header();
    bool write_staging_bytes(const uint8_t *data, size_t len);
    bool write_raw_payload_slice(size_t input_offset,
                                 const uint8_t *data,
                                 size_t len);
    bool finalize_raw_abc();
    bool enough_storage(size_t output_size) const;
    bool guard_device_idle_for_upgrade();
    bool device_idle_for_upgrade(const char **reason) const;
    bool lock(uint32_t timeout_ms) const;
    void unlock() const;

    RpcArbiter *arbiter_ = nullptr;
    mutable SemaphoreHandle_t mutex_ = nullptr;
    ResmedOtaStatus status_;
    WaitingFor waiting_for_ = WaitingFor::None;
    String pending_block_hex_;
    size_t pending_block_offset_ = 0;
    size_t pending_block_bytes_ = 0;
    bool sha_started_ = false;
    bool sha_finished_ = false;
    uint32_t last_activity_ms_ = 0;
    mbedtls_sha256_context sha_ctx_;

    File staging_file_;
    File staged_read_file_;
    bool staging_passthrough_abc_ = false;
    bool staged_transfer_active_ = false;
    bool staged_check_requested_ = false;
    bool staged_auto_apply_ = false;
    size_t staging_input_size_ = 0;
    size_t staging_input_written_ = 0;
    size_t staging_output_size_ = 0;
    size_t staging_payload_size_ = 0;
    size_t staging_payload_written_ = 0;
    size_t staging_source_offset_ = 0;
    uint32_t staging_flash_start_ = 0;
    uint32_t staging_rest_crc_ = 0xFFFFFFFFu;
    uint32_t staging_desc2_ = 0;
    uint32_t staging_desc3_ = 0;
    char staging_target_code_[5] = {};
    char staging_descriptor_version_[16] = {};
};

}  // namespace aircannect
