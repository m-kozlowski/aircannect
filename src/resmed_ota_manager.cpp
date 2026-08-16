#include "resmed_ota_manager.h"

#include <algorithm>
#include <ctype.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "as11_device_service.h"
#include "as11_rpc.h"
#include "as11_service_lz4.h"
#include "as11_service_manager.h"
#include "as11_service_protocol.h"
#include "debug_log.h"
#include "large_byte_buffer.h"
#include "memory_manager.h"
#include "runtime_clock.h"
#include "storage_path_port.h"
#include "storage_stream_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t ServiceFgcbEraseBytes = 0x00020000;
static constexpr uint32_t ServiceFgcbProgramBytes = 32;
static constexpr uint32_t ServiceResetWaitMs = 10000;
static constexpr uint8_t ServiceResetMaxAttempts = 2;
static constexpr uint8_t ServiceProgressLogStep = 10;

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool normalize_hex(String &hex, size_t max_raw_bytes) {
    hex.trim();
    if (!hex.length() || (hex.length() & 1)) return false;
    if (hex.length() > max_raw_bytes * 2) return false;
    for (size_t i = 0; i < hex.length(); ++i) {
        if (hex_nibble(hex[i]) < 0) return false;
    }

    hex.toUpperCase();
    return true;
}

bool valid_sha256(String value) {
    value.trim();
    if (!value.length()) return true;
    if (value.length() != 64) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (hex_nibble(value[i]) < 0) return false;
    }
    return true;
}

bool json_result_true(RpcPayloadView json) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(
        doc, json.data() ? json.data() : "", json.size());
    return !error && doc["result"].is<bool>() &&
           doc["result"].as<bool>();
}

String sha_to_hex(const uint8_t hash[32]) {
    static constexpr char Digits[] = "0123456789ABCDEF";
    String out;
    out.reserve(64);
    for (size_t i = 0; i < 32; ++i) {
        out += Digits[(hash[i] >> 4) & 0x0F];
        out += Digits[hash[i] & 0x0F];
    }
    return out;
}

bool update_sha_from_hex(mbedtls_sha256_context &context,
                         const char *hex,
                         size_t raw_length) {
    if (!hex || strlen(hex) != raw_length * 2) return false;

    uint8_t bytes[64];
    size_t buffered = 0;
    for (size_t i = 0; i < raw_length * 2; i += 2) {
        const int high = hex_nibble(hex[i]);
        const int low = hex_nibble(hex[i + 1]);
        if (high < 0 || low < 0) return false;

        bytes[buffered++] = static_cast<uint8_t>((high << 4) | low);
        if (buffered == sizeof(bytes)) {
            mbedtls_sha256_update(&context, bytes, buffered);
            buffered = 0;
        }
    }
    if (buffered) mbedtls_sha256_update(&context, bytes, buffered);
    return true;
}

String bytes_to_hex(const uint8_t *bytes, size_t length) {
    static constexpr char Digits[] = "0123456789ABCDEF";
    String hex;
    hex.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        hex += Digits[(bytes[i] >> 4) & 0x0F];
        hex += Digits[bytes[i] & 0x0F];
    }
    return hex;
}

void put_le32(uint8_t *destination, uint32_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

void put_le16(uint8_t *destination, uint16_t value) {
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

}  // namespace

struct ResmedOtaManager::ColdState {
    ResmedOtaStatus status;
    char pending_block_hex[AC_RESMED_OTA_MAX_BLOCK_BYTES * 2 + 1] = {};

    ResmedPreparedFirmware prepared;
    std::shared_ptr<StorageByteStream> prepared_stream;
    uint8_t prepared_block[AS11_SERVICE_V2_WRITE_DATA_MAX_BYTES] = {};
    std::unique_ptr<LargeByteBuffer> service_lz4_state;

    char cleanup_paths[2][AC_STORAGE_PATH_MAX] = {};
};

ResmedOtaManager::ScopedLock::ScopedLock(
    const ResmedOtaManager &manager,
    uint32_t timeout_ms)
    : manager_(manager) {
    locked_ = manager_.lock(timeout_ms);
}

ResmedOtaManager::ScopedLock::~ScopedLock() {
    if (locked_) manager_.unlock();
}

bool ResmedOtaManager::begin(RpcRequestPort &rpc,
                             As11DeviceService &device,
                             As11ServiceManager &service,
                             StorageStreamPort &stream_port,
                             StoragePathPort &path_port) {
    if (cold_) return true;

    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();
    if (!mutex_) return false;

    void *memory = Memory::alloc_large(sizeof(ColdState), false);
    if (!memory) {
        Log::logf(CAT_OTA, LOG_ERROR,
                  "[RESMED] OTA state allocation failed\n");
        return false;
    }

    cold_ = new (memory) ColdState();
    rpc_ = &rpc;
    device_ = &device;
    service_ = &service;
    stream_port_ = &stream_port;
    path_port_ = &path_port;
    mbedtls_sha256_init(&sha_ctx_);
    return true;
}

void ResmedOtaManager::poll() {
    ScopedLock lock(*this, 0);
    if (!lock || !cold_ || !rpc_) return;

    poll_rpc_completion();
    poll_service_completion();
    poll_prepared_transfer();
    poll_cleanup();

    const bool idle_timeout_phase =
        cold_->status.phase == ResmedOtaPhase::Opening ||
        cold_->status.phase == ResmedOtaPhase::Ready ||
        cold_->status.phase == ResmedOtaPhase::Uploaded;
    if (idle_timeout_phase && last_activity_ms_ &&
        static_cast<int32_t>(millis() - last_activity_ms_) >
            static_cast<int32_t>(AC_RESMED_OTA_IDLE_TIMEOUT_MS)) {
        set_error("session_idle_timeout");
    }
}

bool ResmedOtaManager::begin_upload(size_t total_size,
                                    const String &expected_sha256,
                                    const String &filename) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (!cold_ || !rpc_) return false;
    if (!guard_device_idle_for_upgrade()) return false;
    if (active() || transport_active()) {
        set_error("session_active");
        return false;
    }

    clear_session();
    return begin_protocol(total_size, expected_sha256, filename);
}

bool ResmedOtaManager::begin_prepared_install(
    const ResmedPreparedFirmware &firmware) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (!cold_ || !rpc_ || !stream_port_ || !path_port_) return false;
    const bool service_install =
        firmware.transport == ResmedFirmwareInstallTransport::Service;
    const uint64_t install_size = service_install
        ? firmware.image.service_payload_size
        : firmware.image.prepared_size;
    if (!firmware.valid() || install_size == 0 ||
        install_size > AC_RESMED_OTA_MAX_FILE_BYTES ||
        (service_install &&
         (!service_ || !firmware.image.service_payload_valid()))) {
        set_error("invalid_prepared_image");
        return false;
    }
    if (active() || transport_active()) {
        return false;
    }

    clear_session();
    cold_->prepared = firmware;
    if (!guard_device_idle_for_upgrade()) return false;
    if (service_install) {
        if (!service_->acquire(As11ServiceOwner::ResmedOta)) {
            set_error("service_busy");
            return false;
        }
        service_owned_ = true;
    }

    prepared_transfer_ = true;
    apply_after_check_ = true;
    cold_->status.phase = ResmedOtaPhase::Opening;
    cold_->status.total_size = static_cast<size_t>(install_size);
    cold_->status.filename = firmware.filename;
    cold_->status.input_type =
        resmed_firmware_image_kind_name(firmware.image.kind);
    cold_->status.transport = firmware.transport;
    cold_->status.target = firmware.image.target;
    cold_->status.source_path = firmware.path;
    cold_->status.last_result = "opening";
    last_activity_ms_ = millis();

    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] prepared install opening transport=%s target=%s "
              "size=%u path=%s\n",
              resmed_firmware_install_transport_name(firmware.transport),
              firmware.image.target, static_cast<unsigned>(install_size),
              firmware.path);
    return true;
}

bool ResmedOtaManager::discard_prepared_firmware(
    const ResmedPreparedFirmware &firmware) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_ || !firmware.valid() || active() ||
        transport_active()) {
        return false;
    }

    clear_session();
    cold_->prepared = firmware;
    schedule_prepared_cleanup();
    return true;
}

bool ResmedOtaManager::begin_protocol(size_t total_size,
                                      const String &expected_sha256,
                                      const String &filename) {
    if (total_size == 0 || total_size > AC_RESMED_OTA_MAX_FILE_BYTES) {
        set_error("bad_size");
        return false;
    }

    String expected = expected_sha256;
    expected.trim();
    expected.toUpperCase();
    if (!valid_sha256(expected)) {
        set_error("bad_sha256");
        return false;
    }

    cold_->status.phase = ResmedOtaPhase::Initiating;
    cold_->status.total_size = total_size;
    cold_->status.uploaded_bytes = 0;
    cold_->status.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    cold_->status.progress_percent = 0;
    cold_->status.filename = filename;
    cold_->status.expected_sha256 = expected;
    cold_->status.computed_sha256 = "";
    cold_->status.last_error = "";
    last_activity_ms_ = millis();
    mbedtls_sha256_starts(&sha_ctx_, 0);
    sha_started_ = true;
    sha_finished_ = false;

    std::string params = "{\"upgradeFileSize\":";
    params += std::to_string(total_size);
    params += "}";
    if (!queue_request("InitiateUpgrade", params,
                       AC_RESMED_OTA_BLOCK_TIMEOUT_MS)) {
        set_error("initiate_queue_failed");
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO, "[RESMED] initiate size=%u file=%s\n",
              static_cast<unsigned>(total_size), filename.c_str());
    return true;
}

bool ResmedOtaManager::submit_block(size_t offset,
                                    const String &hex_data) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_ || !rpc_) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (cold_->status.phase != ResmedOtaPhase::Ready &&
        cold_->status.phase != ResmedOtaPhase::Uploading) {
        set_error("not_ready");
        return false;
    }
    if (offset != cold_->status.uploaded_bytes || offset >= cold_->status.total_size) {
        set_error("bad_offset");
        return false;
    }

    String hex = hex_data;
    if (!normalize_hex(hex, cold_->status.xfer_block_size)) {
        set_error("bad_hex_block");
        return false;
    }
    const size_t raw_length = hex.length() / 2;
    if (raw_length == 0 || offset + raw_length > cold_->status.total_size) {
        set_error("bad_block_size");
        return false;
    }

    std::string params;
    params.reserve(hex.length() + 80);
    params += "{\"fileOffset\":";
    params += std::to_string(offset);
    params += ",\"encoding\":\"AsciiHex\",\"data\":\"";
    params += hex.c_str();
    params += "\"}";

    copy_cstr(cold_->pending_block_hex, sizeof(cold_->pending_block_hex),
              hex.c_str());
    pending_block_offset_ = offset;
    pending_block_bytes_ = raw_length;
    cold_->status.phase = ResmedOtaPhase::Uploading;
    last_activity_ms_ = millis();
    if (!queue_request("UpgradeDataBlock", params,
                       AC_RESMED_OTA_BLOCK_TIMEOUT_MS)) {
        cold_->pending_block_hex[0] = '\0';
        pending_block_offset_ = 0;
        pending_block_bytes_ = 0;
        set_error("block_queue_failed");
        return false;
    }
    return true;
}

bool ResmedOtaManager::request_check() {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (cold_->status.phase != ResmedOtaPhase::Uploaded ||
        cold_->status.uploaded_bytes != cold_->status.total_size) {
        set_error("upload_not_complete");
        return false;
    }
    if (!finish_hash()) return false;
    if (cold_->status.expected_sha256.length() &&
        cold_->status.expected_sha256 != cold_->status.computed_sha256) {
        set_error("sha256_mismatch");
        return false;
    }

    std::string params = "{\"upgradeFileHash\":\"";
    params += cold_->status.computed_sha256.c_str();
    params += "\"}";
    cold_->status.phase = ResmedOtaPhase::Checking;
    last_activity_ms_ = millis();
    if (!queue_request("CheckUpgradeFile", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("check_queue_failed");
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO, "[RESMED] check sha256=%s\n",
              cold_->status.computed_sha256.c_str());
    return true;
}

bool ResmedOtaManager::request_apply_plain(bool reset_settings,
                                           const String &confirm) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return false;
    if (confirm != AC_RESMED_OTA_CONFIRM) {
        set_error("confirmation_required");
        return false;
    }

    return queue_plain_apply(reset_settings);
}

bool ResmedOtaManager::queue_plain_apply(bool reset_settings) {
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (cold_->status.phase != ResmedOtaPhase::Verified ||
        !cold_->status.computed_sha256.length()) {
        set_error("not_verified");
        return false;
    }

    std::string params = "{\"upgradeFileHash\":\"";
    params += cold_->status.computed_sha256.c_str();
    params += "\",\"resetSettingsToDefault\":";
    params += reset_settings ? "true" : "false";
    params += "}";
    cold_->status.phase = ResmedOtaPhase::Applying;
    cold_->status.apply_mode = "plain";
    last_activity_ms_ = millis();
    if (!queue_request("ApplyUpgrade", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("apply_queue_failed");
        return false;
    }
    apply_after_check_ = false;

    Log::logf(CAT_OTA, LOG_WARN, "[RESMED] ApplyUpgrade queued\n");
    return true;
}

bool ResmedOtaManager::request_apply_authenticated(
    const String &authentication,
    const String &confirm) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (cold_->status.phase != ResmedOtaPhase::Verified ||
        !cold_->status.computed_sha256.length()) {
        set_error("not_verified");
        return false;
    }
    if (confirm != AC_RESMED_OTA_CONFIRM) {
        set_error("confirmation_required");
        return false;
    }

    String tag = authentication;
    tag.trim();
    tag.toUpperCase();
    if (tag.length() != 64 || !valid_sha256(tag)) {
        set_error("bad_authentication");
        return false;
    }

    std::string params = "{\"upgradeFileHash\":\"";
    params += cold_->status.computed_sha256.c_str();
    params += "\",\"authentication\":\"";
    params += tag.c_str();
    params += "\"}";
    cold_->status.phase = ResmedOtaPhase::Applying;
    cold_->status.apply_mode = "authenticated";
    last_activity_ms_ = millis();
    if (!queue_request("ApplyAuthenticatedUpgrade", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("apply_queue_failed");
        return false;
    }

    Log::logf(CAT_OTA, LOG_WARN,
              "[RESMED] ApplyAuthenticatedUpgrade queued\n");
    return true;
}

void ResmedOtaManager::abort(const char *reason) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return;

    const char *result = reason ? reason : "aborted";
    clear_session();

    Log::logf(CAT_OTA, LOG_INFO, "[RESMED] %s\n", result);
}

bool ResmedOtaManager::reset_terminal_state() {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return false;

    if (active() || transport_active()) return false;

    if (cold_->status.phase == ResmedOtaPhase::Error ||
        cold_->status.phase == ResmedOtaPhase::Complete) {
        clear_session();
    }
    return cold_->status.phase == ResmedOtaPhase::Idle;
}

bool ResmedOtaManager::active() const {
    ScopedLock lock(*this, 50);
    if (!lock) return true;
    if (!cold_) return false;

    return cleanup_count_ != 0 ||
           cold_->status.phase == ResmedOtaPhase::Opening ||
           cold_->status.phase == ResmedOtaPhase::EnteringService ||
           cold_->status.phase == ResmedOtaPhase::Erasing ||
           cold_->status.phase == ResmedOtaPhase::Initiating ||
           cold_->status.phase == ResmedOtaPhase::Ready ||
           cold_->status.phase == ResmedOtaPhase::Uploading ||
           cold_->status.phase == ResmedOtaPhase::Uploaded ||
           cold_->status.phase == ResmedOtaPhase::Checking ||
           cold_->status.phase == ResmedOtaPhase::Verified ||
           cold_->status.phase == ResmedOtaPhase::Applying ||
           cold_->status.phase == ResmedOtaPhase::Resetting;
}

bool ResmedOtaManager::transport_active() const {
    ScopedLock lock(*this, 50);
    if (!lock) return true;
    if (!cold_) return false;

    return waiting_for_ != WaitingFor::None || cleanup_count_ != 0 ||
           service_waiting_for_ != ServiceWaitingFor::None || service_owned_ ||
           cold_->status.phase == ResmedOtaPhase::Opening ||
           cold_->status.phase == ResmedOtaPhase::EnteringService ||
           cold_->status.phase == ResmedOtaPhase::Erasing ||
           cold_->status.phase == ResmedOtaPhase::Initiating ||
           cold_->status.phase == ResmedOtaPhase::Ready ||
           cold_->status.phase == ResmedOtaPhase::Uploading ||
           cold_->status.phase == ResmedOtaPhase::Uploaded ||
           cold_->status.phase == ResmedOtaPhase::Checking ||
           cold_->status.phase == ResmedOtaPhase::Applying ||
           cold_->status.phase == ResmedOtaPhase::Resetting;
}

ResmedOtaStatus ResmedOtaManager::status() const {
    ScopedLock lock(*this, 50);
    return lock && cold_ ? cold_->status : ResmedOtaStatus{};
}

const char *ResmedOtaManager::phase_name() const {
    ScopedLock lock(*this, 50);
    if (!lock) return "busy";
    if (!cold_) return "unavailable";

    switch (cold_->status.phase) {
        case ResmedOtaPhase::Idle: return "idle";
        case ResmedOtaPhase::Opening: return "opening";
        case ResmedOtaPhase::EnteringService: return "entering_service";
        case ResmedOtaPhase::Erasing: return "erasing";
        case ResmedOtaPhase::Initiating: return "initiating";
        case ResmedOtaPhase::Ready: return "ready";
        case ResmedOtaPhase::Uploading: return "uploading";
        case ResmedOtaPhase::Uploaded: return "uploaded";
        case ResmedOtaPhase::Checking: return "checking";
        case ResmedOtaPhase::Verified: return "verified";
        case ResmedOtaPhase::Applying: return "applying";
        case ResmedOtaPhase::Resetting: return "resetting";
        case ResmedOtaPhase::Complete: return "complete";
        case ResmedOtaPhase::Error: return "error";
    }
    return "unknown";
}

bool ResmedOtaManager::queue_request(const char *method,
                                     const std::string &params,
                                     uint32_t timeout_ms) {
    if (!rpc_ || !method || rpc_ticket_.valid()) return false;

    WaitingFor waiting = WaitingFor::None;
    if (!strcmp(method, "InitiateUpgrade")) {
        waiting = WaitingFor::Initiate;
    } else if (!strcmp(method, "UpgradeDataBlock")) {
        waiting = WaitingFor::Block;
    } else if (!strcmp(method, "CheckUpgradeFile")) {
        waiting = WaitingFor::Check;
    } else if (!strcmp(method, "ApplyUpgrade") ||
               !strcmp(method, "ApplyAuthenticatedUpgrade")) {
        waiting = WaitingFor::Apply;
    }
    if (waiting == WaitingFor::None) return false;

    rpc_generation_++;
    if (rpc_generation_ == 0) rpc_generation_++;

    RpcRequestCommand command;
    command.method = method;
    command.params_json = params;
    command.source = RpcSource::ResmedOta;
    command.timeout_ms = timeout_ms;
    command.generation = rpc_generation_;
    const OperationSubmission submission = rpc_->request(command);
    if (!submission.accepted()) return false;

    rpc_ticket_ = submission.ticket;
    waiting_for_ = waiting;
    cold_->status.waiting = true;
    return true;
}

void ResmedOtaManager::poll_rpc_completion() {
    if (!rpc_ticket_.valid()) return;

    RpcRequestCompletion completion;
    if (!rpc_->take_completion(rpc_ticket_, completion)) return;

    rpc_ticket_ = {};
    if (completion.cause == RpcCompletionCause::Response) {
        handle_response(rpc_payload_view(completion.payload));
        return;
    }

    set_error(completion.reason.empty()
                  ? "rpc_request_failed"
                  : completion.reason.c_str());
}

void ResmedOtaManager::cancel_rpc_request() {
    if (!rpc_ || !rpc_ticket_.valid()) return;

    (void)rpc_->cancel(rpc_ticket_);
    RpcRequestCompletion completion;
    (void)rpc_->take_completion(rpc_ticket_, completion);
    rpc_ticket_ = {};
}

void ResmedOtaManager::handle_response(RpcPayloadView payload) {
    if (waiting_for_ == WaitingFor::None) return;

    cold_->status.last_result = "";
    cold_->status.last_result.reserve(payload.size());
    if (!payload.empty()) {
        cold_->status.last_result.concat(
            payload.data(), static_cast<unsigned int>(payload.size()));
    }
    cold_->status.waiting = false;
    last_activity_ms_ = millis();
    if (json_member_present(payload.data(), payload.size(), "error")) {
        waiting_for_ = WaitingFor::None;
        set_error("rpc_error");
        return;
    }

    const WaitingFor completed = waiting_for_;
    waiting_for_ = WaitingFor::None;
    switch (completed) {
        case WaitingFor::Initiate: {
            uint32_t block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
            json_extract_uint_member(payload.data(), payload.size(),
                                     "xferBlockSize", block_size);
            if (block_size == 0 ||
                block_size > AC_RESMED_OTA_MAX_BLOCK_BYTES) {
                set_error("bad_xfer_block_size");
                return;
            }
            cold_->status.xfer_block_size = block_size;
            cold_->status.phase = ResmedOtaPhase::Ready;
            break;
        }

        case WaitingFor::Block:
            if (!json_result_true(payload)) {
                set_error("block_rejected");
                return;
            }
            finish_pending_block();
            break;

        case WaitingFor::Check:
            if (!json_result_true(payload)) {
                set_error("check_rejected");
                return;
            }
            cold_->status.phase = ResmedOtaPhase::Verified;
            schedule_prepared_cleanup();
            if (apply_after_check_) (void)queue_plain_apply(false);
            break;

        case WaitingFor::Apply:
            cold_->status.phase = ResmedOtaPhase::Complete;
            break;

        case WaitingFor::None:
            break;
    }
}

bool ResmedOtaManager::begin_service_install() {
    if (!service_ || !cold_->prepared.image.service_payload_valid() ||
        cold_->prepared.image.flash_start % ServiceFgcbEraseBytes != 0 ||
        cold_->prepared.image.service_payload_size % ServiceFgcbEraseBytes != 0) {
        set_error("service_range_invalid");
        return false;
    }
    if (!service_owned_ &&
        !service_->acquire(As11ServiceOwner::ResmedOta)) {
        set_error("service_busy");
        return false;
    }

    service_owned_ = true;
    service_erase_offset_ = cold_->prepared.image.flash_start;
    service_started_ms_ = millis();
    service_reset_attempts_ = 0;
    service_reset_accepted_ms_ = 0;
    service_reset_boot_revision_ = 0;
    service_next_progress_percent_ = ServiceProgressLogStep;
    service_write_lz4_supported_ = true;
    cold_->service_lz4_state.reset();
    cold_->status.xfer_block_size = AS11_SERVICE_V2_WRITE_DATA_MAX_BYTES;
    cold_->status.phase = ResmedOtaPhase::EnteringService;

    Log::logf(
        CAT_OTA, LOG_INFO,
        "[RESMED] service install started target=%s offset=0x%08lx "
        "bytes=%u sectors=%u\n",
        cold_->status.target.c_str(),
        static_cast<unsigned long>(cold_->prepared.image.flash_start),
        static_cast<unsigned>(cold_->status.total_size),
        static_cast<unsigned>(cold_->status.total_size /
                              ServiceFgcbEraseBytes));

    return submit_service_request(AS11_SERVICE_COMMAND_ENTER, nullptr, 0,
                                  ServiceWaitingFor::Enter);
}

void ResmedOtaManager::poll_service_completion() {
    if (!service_owned_ || !service_) return;

    As11ServiceTransactionError error;
    if (service_->take_error(As11ServiceOwner::ResmedOta, error)) {
        char reason[64];
        snprintf(reason, sizeof(reason), "service_%s",
                 as11_service_transaction_error_name(error));
        set_error(reason);
        return;
    }

    std::shared_ptr<const LargeByteBuffer> response;
    bool close_after_send = false;
    if (!service_->take_response(As11ServiceOwner::ResmedOta, response,
                                 close_after_send)) {
        return;
    }

    (void)close_after_send;
    handle_service_response(response);
}

void ResmedOtaManager::poll_service_transfer() {
    if (service_waiting_for_ != ServiceWaitingFor::None) return;

    if (cold_->status.phase == ResmedOtaPhase::Resetting) {
        poll_service_reset();
        return;
    }
    if (cold_->status.phase == ResmedOtaPhase::Erasing) {
        (void)submit_service_erase();
        return;
    }
    if (cold_->status.phase != ResmedOtaPhase::Uploading) return;

    if (cold_->status.uploaded_bytes >= cold_->status.total_size) {
        close_prepared_stream(true);
        (void)submit_service_reset();
        return;
    }

    (void)submit_service_write();
}

bool ResmedOtaManager::submit_service_request(
    uint8_t command,
    const uint8_t *payload,
    size_t payload_size,
    ServiceWaitingFor waiting_for) {
    if (!service_ || !service_owned_ ||
        service_waiting_for_ != ServiceWaitingFor::None) {
        set_error("service_not_ready");
        return false;
    }

    const size_t packet_capacity = AS11_SERVICE_PACKET_HEADER_BYTES +
                                   payload_size +
                                   AS11_SERVICE_V2_CRC_BYTES;
    std::unique_ptr<LargeByteBuffer> packet =
        LargeByteBuffer::allocate(packet_capacity);
    if (!packet) {
        set_error("service_packet_alloc_failed");
        return false;
    }

    service_sequence_++;
    if (service_sequence_ == 0) service_sequence_++;

    size_t packet_size = 0;
    if (!as11_service_encode_packet(
            AS11_SERVICE_PROTOCOL_VERSION_V2, command,
            AS11_SERVICE_STATUS_OK, service_sequence_, payload, payload_size,
            packet->data(), packet->size(), packet_size) ||
        !packet->truncate(packet_size)) {
        set_error("service_packet_encode_failed");
        return false;
    }

    const bool enter_allowed = command == AS11_SERVICE_COMMAND_ENTER;
    if (!service_->submit_packet(As11ServiceOwner::ResmedOta,
                                 std::move(packet), enter_allowed, millis())) {
        char reason[64];
        snprintf(reason, sizeof(reason), "service_%s",
                 as11_service_transaction_error_name(service_->last_error()));
        set_error(reason);
        return false;
    }

    service_waiting_for_ = waiting_for;
    cold_->status.waiting = true;
    last_activity_ms_ = millis();
    return true;
}

void ResmedOtaManager::handle_service_response(
    const std::shared_ptr<const LargeByteBuffer> &response) {
    if (!response || service_waiting_for_ == ServiceWaitingFor::None) {
        set_error("service_response_missing");
        return;
    }

    As11ServicePacketHeader header;
    const As11ServicePacketError packet_error = as11_service_validate_packet(
        response->data(), response->size(), header);
    const ServiceWaitingFor completed = service_waiting_for_;
    service_waiting_for_ = ServiceWaitingFor::None;
    cold_->status.waiting = false;
    last_activity_ms_ = millis();

    uint8_t expected_command = 0;
    switch (completed) {
        case ServiceWaitingFor::Enter:
            expected_command = AS11_SERVICE_COMMAND_ENTER;
            break;
        case ServiceWaitingFor::Erase:
            expected_command = AS11_SERVICE_COMMAND_ERASE;
            break;
        case ServiceWaitingFor::Write:
            expected_command = AS11_SERVICE_COMMAND_WRITE;
            break;
        case ServiceWaitingFor::WriteLz4:
            expected_command = AS11_SERVICE_COMMAND_WRITE_LZ4;
            break;
        case ServiceWaitingFor::Reset:
            expected_command = AS11_SERVICE_COMMAND_RESET;
            break;
        case ServiceWaitingFor::None:
            break;
    }

    if (packet_error != As11ServicePacketError::None ||
        header.command != expected_command ||
        header.sequence != service_sequence_) {
        set_error("service_response_invalid");
        return;
    }
    if (completed == ServiceWaitingFor::WriteLz4 &&
        header.status == AS11_SERVICE_STATUS_BAD_COMMAND) {
        service_write_lz4_supported_ = false;
        cold_->service_lz4_state.reset();
        Log::logf(CAT_OTA, LOG_DEBUG,
                  "[RESMED] service LZ4 unavailable; using raw writes\n");
        return;
    }

    if (header.status != AS11_SERVICE_STATUS_OK) {
        char reason[40];
        snprintf(reason, sizeof(reason), "service_status_%u",
                 static_cast<unsigned>(header.status));
        set_error(reason);
        return;
    }
    if (completed != ServiceWaitingFor::Enter && header.payload_length != 0) {
        set_error("service_response_payload");
        return;
    }

    cold_->status.last_result = "ok";
    switch (completed) {
        case ServiceWaitingFor::Enter:
            cold_->status.phase = ResmedOtaPhase::Erasing;
            Log::logf(CAT_OTA, LOG_INFO,
                      "[RESMED] service entered target=%s\n",
                      cold_->status.target.c_str());
            break;

        case ServiceWaitingFor::Erase:
            cold_->status.phase = ResmedOtaPhase::Uploading;
            break;

        case ServiceWaitingFor::Write:
        case ServiceWaitingFor::WriteLz4:
            cold_->status.uploaded_bytes += service_pending_bytes_;
            service_pending_bytes_ = 0;
            prepared_block_bytes_ = 0;
            prepared_block_wanted_ = 0;
            update_progress();
            log_service_progress();

            if (cold_->status.uploaded_bytes < cold_->status.total_size &&
                cold_->prepared.image.flash_start +
                        cold_->status.uploaded_bytes ==
                    service_erase_offset_ + ServiceFgcbEraseBytes) {
                service_erase_offset_ += ServiceFgcbEraseBytes;
                cold_->status.phase = ResmedOtaPhase::Erasing;
            }
            break;

        case ServiceWaitingFor::Reset:
            service_reset_accepted_ms_ = millis();
            Log::logf(
                CAT_OTA, LOG_INFO,
                "[RESMED] service reset accepted target=%s attempt=%u; "
                "waiting for application boot\n",
                cold_->status.target.c_str(),
                static_cast<unsigned>(service_reset_attempts_));
            break;

        case ServiceWaitingFor::None:
            break;
    }
}

bool ResmedOtaManager::submit_service_erase() {
    const uint64_t region_end =
        cold_->prepared.image.flash_start + cold_->status.total_size;
    if (service_erase_offset_ >= region_end) {
        cold_->status.phase = ResmedOtaPhase::Uploading;
        return true;
    }

    uint8_t payload[9] = {AS11_SERVICE_TARGET_FGCB};
    put_le32(payload + 1, service_erase_offset_);
    put_le32(payload + 5, ServiceFgcbEraseBytes);
    if (!submit_service_request(AS11_SERVICE_COMMAND_ERASE, payload,
                                sizeof(payload),
                                ServiceWaitingFor::Erase)) {
        return false;
    }

    const uint32_t region_start = cold_->prepared.image.flash_start;
    const size_t sector_index =
        (service_erase_offset_ - region_start) / ServiceFgcbEraseBytes + 1;
    const size_t sector_count =
        cold_->status.total_size / ServiceFgcbEraseBytes;
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] service erase target=%s sector=%u/%u "
              "offset=0x%08lx\n",
              cold_->status.target.c_str(),
              static_cast<unsigned>(sector_index),
              static_cast<unsigned>(sector_count),
              static_cast<unsigned long>(service_erase_offset_));
    return true;
}

bool ResmedOtaManager::submit_service_write() {
    if (!cold_->prepared_stream || !stream_port_) {
        set_error("prepared_stream_missing");
        return false;
    }

    if (prepared_block_wanted_ == 0) {
        const size_t remaining =
            cold_->status.total_size - cold_->status.uploaded_bytes;
        const uint32_t write_offset =
            cold_->prepared.image.flash_start +
            cold_->status.uploaded_bytes;
        const uint32_t sector_end =
            service_erase_offset_ + ServiceFgcbEraseBytes;
        if (write_offset < service_erase_offset_ ||
            write_offset >= sector_end) {
            set_error("service_sector_state_invalid");
            return false;
        }

        const size_t sector_remaining =
            sector_end - write_offset;
        prepared_block_wanted_ = std::min({
            static_cast<size_t>(AS11_SERVICE_V2_WRITE_DATA_MAX_BYTES),
            remaining,
            sector_remaining,
        });
        if (prepared_block_wanted_ == 0 ||
            prepared_block_wanted_ % ServiceFgcbProgramBytes != 0) {
            set_error("service_write_alignment");
            return false;
        }
        prepared_block_bytes_ = 0;
    }

    if (prepared_block_bytes_ < prepared_block_wanted_) {
        const uint64_t source_offset =
            cold_->status.uploaded_bytes + prepared_block_bytes_;
        const StorageStreamRead read = stream_port_->read(
            *cold_->prepared_stream,
            cold_->prepared_block + prepared_block_bytes_,
            prepared_block_wanted_ - prepared_block_bytes_, source_offset);
        if (read.state == StorageStreamReadState::Retry) return false;
        if (read.state != StorageStreamReadState::Data || read.bytes == 0) {
            set_error(read.state == StorageStreamReadState::End
                          ? "prepared_stream_short"
                          : "prepared_stream_read_failed");
            return false;
        }

        prepared_block_bytes_ += read.bytes;
    }

    if (prepared_block_bytes_ != prepared_block_wanted_) return true;

    if (service_write_lz4_supported_) {
        bool submitted = false;
        if (!submit_service_write_lz4(submitted)) return false;
        if (submitted) return true;
    }

    const size_t payload_size = AS11_SERVICE_STORAGE_ADDRESS_BYTES +
                                prepared_block_bytes_;
    std::unique_ptr<LargeByteBuffer> payload =
        LargeByteBuffer::allocate(payload_size);
    if (!payload) {
        set_error("service_write_alloc_failed");
        return false;
    }

    payload->data()[0] = AS11_SERVICE_TARGET_FGCB;
    put_le32(payload->data() + 1,
             cold_->prepared.image.flash_start +
                 cold_->status.uploaded_bytes);
    memcpy(payload->data() + AS11_SERVICE_STORAGE_ADDRESS_BYTES,
           cold_->prepared_block, prepared_block_bytes_);
    service_pending_bytes_ = prepared_block_bytes_;
    return submit_service_request(AS11_SERVICE_COMMAND_WRITE,
                                  payload->data(), payload->size(),
                                  ServiceWaitingFor::Write);
}

bool ResmedOtaManager::submit_service_write_lz4(bool &submitted) {
    submitted = false;
    if (prepared_block_bytes_ == 0 ||
        prepared_block_bytes_ > AS11_SERVICE_LZ4_RAW_MAX_BYTES) {
        return true;
    }

    const size_t state_bytes = as11_service_lz4_state_bytes();
    if (!cold_->service_lz4_state) {
        cold_->service_lz4_state = LargeByteBuffer::allocate(state_bytes);
        if (!cold_->service_lz4_state) {
            service_write_lz4_supported_ = false;
            return true;
        }
    }

    const size_t payload_capacity = AS11_SERVICE_WRITE_LZ4_METADATA_BYTES +
                                    prepared_block_bytes_;
    std::unique_ptr<LargeByteBuffer> payload =
        LargeByteBuffer::allocate(payload_capacity);
    if (!payload) {
        set_error("service_write_alloc_failed");
        return false;
    }

    size_t compressed_size = 0;
    if (!as11_service_compress_lz4_block(
            cold_->prepared_block,
            prepared_block_bytes_,
            payload->data() + AS11_SERVICE_WRITE_LZ4_METADATA_BYTES,
            payload->size() - AS11_SERVICE_WRITE_LZ4_METADATA_BYTES,
            cold_->service_lz4_state->data(),
            cold_->service_lz4_state->size(),
            compressed_size)) {
        set_error("service_lz4_compress_failed");
        return false;
    }

    if (compressed_size == 0) return true;

    payload->data()[0] = AS11_SERVICE_TARGET_FGCB;
    put_le32(payload->data() + 1,
             cold_->prepared.image.flash_start +
                 cold_->status.uploaded_bytes);
    put_le16(payload->data() + 5,
             static_cast<uint16_t>(prepared_block_bytes_));
    if (!payload->truncate(AS11_SERVICE_WRITE_LZ4_METADATA_BYTES +
                           compressed_size)) {
        set_error("service_lz4_payload_failed");
        return false;
    }

    service_pending_bytes_ = prepared_block_bytes_;
    submitted = submit_service_request(AS11_SERVICE_COMMAND_WRITE_LZ4,
                                       payload->data(), payload->size(),
                                       ServiceWaitingFor::WriteLz4);
    return submitted;
}

bool ResmedOtaManager::submit_service_reset() {
    cold_->status.phase = ResmedOtaPhase::Resetting;
    service_reset_accepted_ms_ = 0;
    service_reset_boot_revision_ = device_ ? device_->boot_revision() : 0;
    const uint8_t attempt = service_reset_attempts_ + 1;
    if (!submit_service_request(AS11_SERVICE_COMMAND_RESET, nullptr, 0,
                                ServiceWaitingFor::Reset)) {
        return false;
    }

    service_reset_attempts_ = attempt;
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] service reset requested target=%s attempt=%u\n",
              cold_->status.target.c_str(),
              static_cast<unsigned>(service_reset_attempts_));
    return true;
}

void ResmedOtaManager::poll_service_reset() {
    if (!device_ || service_reset_accepted_ms_ == 0) return;

    if (device_->boot_revision() != service_reset_boot_revision_) {
        finish_service_install();
        return;
    }

    const uint32_t now_ms = millis();
    if (!millis_elapsed_at_least(now_ms, service_reset_accepted_ms_,
                                 ServiceResetWaitMs)) {
        return;
    }

    if (service_reset_attempts_ < ServiceResetMaxAttempts) {
        Log::logf(
            CAT_OTA, LOG_WARN,
            "[RESMED] application boot not observed after service reset; "
            "retrying target=%s\n",
            cold_->status.target.c_str());
        (void)submit_service_reset();
        return;
    }

    set_error("service_reset_not_observed");
}

void ResmedOtaManager::log_service_progress() {
    if (cold_->status.progress_percent < service_next_progress_percent_ &&
        cold_->status.uploaded_bytes < cold_->status.total_size) {
        return;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] service write target=%s bytes=%u/%u progress=%u%%\n",
              cold_->status.target.c_str(),
              static_cast<unsigned>(cold_->status.uploaded_bytes),
              static_cast<unsigned>(cold_->status.total_size),
              static_cast<unsigned>(cold_->status.progress_percent));

    while (service_next_progress_percent_ <=
               cold_->status.progress_percent &&
           service_next_progress_percent_ <= 100 - ServiceProgressLogStep) {
        service_next_progress_percent_ += ServiceProgressLogStep;
    }
}

void ResmedOtaManager::finish_service_install() {
    const uint32_t elapsed_ms = millis() - service_started_ms_;
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] service install complete target=%s bytes=%u "
              "elapsed_ms=%lu\n",
              cold_->status.target.c_str(),
              static_cast<unsigned>(cold_->status.uploaded_bytes),
              static_cast<unsigned long>(elapsed_ms));

    prepared_transfer_ = false;
    cold_->status.phase = ResmedOtaPhase::Complete;
    cold_->status.last_result = "application_ready";
    schedule_prepared_cleanup();
    release_service();
}

void ResmedOtaManager::release_service() {
    if (service_ && service_owned_) {
        service_->release(As11ServiceOwner::ResmedOta);
    }
    service_owned_ = false;
    service_waiting_for_ = ServiceWaitingFor::None;
    service_pending_bytes_ = 0;
    if (cold_) cold_->service_lz4_state.reset();
}

void ResmedOtaManager::poll_prepared_transfer() {
    if (!prepared_transfer_) return;

    if (cold_->status.phase == ResmedOtaPhase::Opening) {
        (void)open_prepared_stream();
        return;
    }
    if (cold_->status.transport == ResmedFirmwareInstallTransport::Service) {
        poll_service_transfer();
        return;
    }
    if (waiting_for_ != WaitingFor::None) return;

    if (cold_->status.phase == ResmedOtaPhase::Uploaded) {
        close_prepared_stream(true);
        prepared_transfer_ = false;
        (void)request_check();
        return;
    }
    if (cold_->status.phase == ResmedOtaPhase::Ready ||
        cold_->status.phase == ResmedOtaPhase::Uploading) {
        (void)fill_prepared_block();
    }
}

bool ResmedOtaManager::open_prepared_stream() {
    if (!stream_port_) {
        set_error("prepared_stream_unavailable");
        return false;
    }

    if (!cold_->prepared_stream) {
        StorageStreamCommand command;
        command.path = cold_->prepared.path;
        command.lane = StorageStreamLane::Foreground;
        command.expected_size =
            cold_->status.transport == ResmedFirmwareInstallTransport::Service
                ? cold_->prepared.image.input_size
                : cold_->prepared.image.prepared_size;
        if (cold_->status.transport ==
            ResmedFirmwareInstallTransport::Service) {
            command.source_offset =
                cold_->prepared.image.service_source_offset;
            command.source_length =
                cold_->prepared.image.service_payload_size;
        }
        command.verification = StorageStreamVerification::Size;

        char error[AC_STORAGE_ERROR_MAX] = {};
        if (!stream_port_->request_stream(
                command, cold_->prepared_stream, error, sizeof(error))) {
            if (!strcmp(error, "stream_busy") ||
                !strcmp(error, "stream_slots_full")) {
                return false;
            }

            set_error(error[0] ? error : "prepared_stream_rejected");
            return false;
        }
    }

    StorageStreamStatus stream_status;
    if (!stream_port_->status(*cold_->prepared_stream, stream_status)) return false;
    if (stream_status.state == StorageStreamState::Error ||
        stream_status.state == StorageStreamState::Cancelled) {
        set_error(stream_status.error[0]
                      ? stream_status.error
                      : "prepared_stream_failed");
        return false;
    }
    if (stream_status.state != StorageStreamState::Ready) return false;
    if (!stream_port_->attach(*cold_->prepared_stream)) return false;

    if (cold_->status.transport == ResmedFirmwareInstallTransport::Service) {
        return begin_service_install();
    }

    return begin_protocol(
        static_cast<size_t>(cold_->prepared.image.prepared_size), "",
        cold_->prepared.filename);
}

bool ResmedOtaManager::fill_prepared_block() {
    if (!cold_->prepared_stream || !stream_port_) {
        set_error("prepared_stream_missing");
        return false;
    }

    if (prepared_block_wanted_ == 0) {
        const size_t remaining =
            cold_->status.total_size - cold_->status.uploaded_bytes;
        if (remaining == 0) return true;
        prepared_block_wanted_ =
            std::min(cold_->status.xfer_block_size, remaining);
        prepared_block_bytes_ = 0;
    }

    const size_t offset = cold_->status.uploaded_bytes + prepared_block_bytes_;
    const StorageStreamRead read = stream_port_->read(
        *cold_->prepared_stream, cold_->prepared_block + prepared_block_bytes_,
        prepared_block_wanted_ - prepared_block_bytes_, offset);
    if (read.state == StorageStreamReadState::Retry) return false;
    if (read.state != StorageStreamReadState::Data || read.bytes == 0) {
        set_error(read.state == StorageStreamReadState::End
                      ? "prepared_stream_short"
                      : "prepared_stream_read_failed");
        return false;
    }

    prepared_block_bytes_ += read.bytes;
    if (prepared_block_bytes_ != prepared_block_wanted_) return true;

    const String hex = bytes_to_hex(cold_->prepared_block, prepared_block_bytes_);
    return submit_block(cold_->status.uploaded_bytes, hex);
}

void ResmedOtaManager::finish_pending_block() {
    if (!sha_started_ || sha_finished_ || pending_block_bytes_ == 0 ||
        !update_sha_from_hex(sha_ctx_, cold_->pending_block_hex,
                             pending_block_bytes_)) {
        set_error("sha_update_failed");
        return;
    }

    cold_->status.uploaded_bytes = pending_block_offset_ + pending_block_bytes_;
    cold_->pending_block_hex[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    prepared_block_bytes_ = 0;
    prepared_block_wanted_ = 0;
    update_progress();
    cold_->status.phase = cold_->status.uploaded_bytes >= cold_->status.total_size
                        ? ResmedOtaPhase::Uploaded
                        : ResmedOtaPhase::Ready;
}

void ResmedOtaManager::close_prepared_stream(bool complete) {
    if (stream_port_ && cold_->prepared_stream) {
        stream_port_->finish(*cold_->prepared_stream, complete);
    }
    cold_->prepared_stream.reset();
    prepared_block_bytes_ = 0;
    prepared_block_wanted_ = 0;
}

void ResmedOtaManager::schedule_prepared_cleanup() {
    if (!cold_->prepared.valid() || cleanup_count_ != 0) return;

    if (cold_->prepared.cleanup_source) {
        copy_cstr(cold_->cleanup_paths[cleanup_count_], AC_STORAGE_PATH_MAX,
                  cold_->prepared.source_path);
        cleanup_count_++;
    }
    if (cold_->prepared.cleanup_prepared &&
        (!cold_->prepared.cleanup_source ||
         strcmp(cold_->prepared.path, cold_->prepared.source_path) != 0) &&
        cleanup_count_ < 2) {
        copy_cstr(cold_->cleanup_paths[cleanup_count_], AC_STORAGE_PATH_MAX,
                  cold_->prepared.path);
        cleanup_count_++;
    }
    cold_->prepared = {};
}

void ResmedOtaManager::poll_cleanup() {
    if (!path_port_ || cleanup_count_ == 0) return;

    if (cleanup_ticket_.valid()) {
        StoragePathCompletion completion;
        if (!path_port_->take_completion(cleanup_ticket_, completion)) return;

        if (completion.outcome.disposition !=
            OperationDisposition::Succeeded) {
            Log::logf(CAT_OTA, LOG_WARN,
                      "[RESMED] transient cleanup failed path=%s error=%s\n",
                      cold_->cleanup_paths[cleanup_index_],
                      completion.error[0] ? completion.error : "remove_failed");
        }
        cleanup_ticket_ = {};
        cleanup_index_++;
    }

    if (cleanup_index_ >= cleanup_count_) {
        clear_cleanup();
        return;
    }

    cleanup_generation_++;
    if (cleanup_generation_ == 0) cleanup_generation_++;

    StoragePathCommand command;
    command.operation = StoragePathOperation::Remove;
    command.source = cold_->cleanup_paths[cleanup_index_];
    command.generation = cleanup_generation_;
    const OperationSubmission submission = path_port_->request(command);
    if (submission.accepted()) {
        cleanup_ticket_ = submission.ticket;
    } else if (submission.admission == OperationAdmission::Rejected) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "[RESMED] transient cleanup rejected path=%s\n",
                  cold_->cleanup_paths[cleanup_index_]);
        cleanup_index_++;
    }
}

void ResmedOtaManager::clear_cleanup() {
    memset(cold_->cleanup_paths, 0, sizeof(cold_->cleanup_paths));
    cleanup_count_ = 0;
    cleanup_index_ = 0;
    cleanup_ticket_ = {};
}

bool ResmedOtaManager::finish_hash() {
    if (!sha_started_) {
        set_error("sha_not_started");
        return false;
    }
    if (sha_finished_) return true;

    uint8_t hash[32] = {};
    mbedtls_sha256_finish(&sha_ctx_, hash);
    cold_->status.computed_sha256 = sha_to_hex(hash);
    sha_finished_ = true;
    return true;
}

void ResmedOtaManager::clear_session() {
    cancel_rpc_request();
    release_service();
    close_prepared_stream(false);
    schedule_prepared_cleanup();

    cold_->pending_block_hex[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    waiting_for_ = WaitingFor::None;
    cold_->status = {};
    cold_->status.phase = ResmedOtaPhase::Idle;
    cold_->status.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    cold_->status.transport = AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT;
    sha_started_ = false;
    sha_finished_ = false;
    last_activity_ms_ = 0;
    cold_->prepared = {};
    prepared_transfer_ = false;
    apply_after_check_ = false;
    service_erase_offset_ = 0;
    service_sequence_ = 0;
    service_write_lz4_supported_ = true;
    service_reset_attempts_ = 0;
    service_next_progress_percent_ = 0;
    service_started_ms_ = 0;
    service_reset_accepted_ms_ = 0;
    service_reset_boot_revision_ = 0;
}

void ResmedOtaManager::set_error(const char *error) {
    release_service();
    close_prepared_stream(false);
    schedule_prepared_cleanup();
    prepared_transfer_ = false;
    apply_after_check_ = false;

    cold_->status.phase = ResmedOtaPhase::Error;
    cold_->status.waiting = false;
    cold_->status.last_error = error ? error : "error";
    waiting_for_ = WaitingFor::None;
    cold_->pending_block_hex[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    last_activity_ms_ = millis();
    Log::logf(CAT_OTA, LOG_ERROR, "[RESMED] %s\n",
              cold_->status.last_error.c_str());
}

void ResmedOtaManager::update_progress() {
    cold_->status.progress_percent = cold_->status.total_size == 0
        ? 0
        : static_cast<uint8_t>((cold_->status.uploaded_bytes * 100ULL) /
                               cold_->status.total_size);
}

bool ResmedOtaManager::guard_device_idle_for_upgrade() {
    const char *reason = nullptr;
    if (device_idle_for_upgrade(&reason)) return true;

    set_error(reason);
    return false;
}

bool ResmedOtaManager::device_idle_for_upgrade(const char **reason) const {
    if (reason) *reason = "therapy_state_unknown";
    if (!rpc_ || !device_) return false;

    const As11DeviceState &as11 = device_->state();
    if (as11.therapy_command_pending()) {
        if (reason) *reason = "therapy_transition_pending";
        return false;
    }

    switch (as11.therapy_state()) {
        case As11TherapyState::Standby:
            return true;
        case As11TherapyState::Running:
            if (reason) *reason = "therapy_active";
            return false;
        case As11TherapyState::Other:
            if (reason) *reason = "device_active";
            return false;
        case As11TherapyState::Unknown:
            device_->request_healthcheck(*rpc_, RpcSource::ResmedOta,
                                         millis());
            if (reason) *reason = "therapy_state_refreshing";
            return false;
    }
    return false;
}

bool ResmedOtaManager::lock(uint32_t timeout_ms) const {
    if (!mutex_) return false;
    return xSemaphoreTakeRecursive(
               mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ResmedOtaManager::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
