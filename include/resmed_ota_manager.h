#pragma once

#include <Arduino.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <string>

#include "board.h"
#include "rpc_arbiter.h"

namespace aircannect {

enum class ResmedOtaPhase {
    Idle,
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
    void abort(const char *reason);

    bool active() const;
    bool transport_active() const;
    const ResmedOtaStatus &status() const { return status_; }
    const char *phase_name() const;

private:
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
    bool finish_hash();
    void clear_session();
    void set_error(const char *error);
    void set_error(const String &error);
    void update_progress();

    RpcArbiter *arbiter_ = nullptr;
    ResmedOtaStatus status_;
    WaitingFor waiting_for_ = WaitingFor::None;
    String pending_block_hex_;
    size_t pending_block_offset_ = 0;
    size_t pending_block_bytes_ = 0;
    bool sha_started_ = false;
    bool sha_finished_ = false;
    uint32_t last_activity_ms_ = 0;
    mbedtls_sha256_context sha_ctx_;
};

}  // namespace aircannect
