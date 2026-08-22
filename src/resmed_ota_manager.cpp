#include "resmed_ota_manager.h"

#include <algorithm>
#include <ctype.h>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>
#include <mbedtls/md.h>

#include "as11_device_service.h"
#include "as11_rpc.h"
#include "as11_service_lz4.h"
#include "as11_service_manager.h"
#include "as11_service_protocol.h"
#include "debug_log.h"
#include "hex_util.h"
#include "large_byte_buffer.h"
#include "little_endian.h"
#include "memory_manager.h"
#include "resmed_firmware_dump.h"
#include "runtime_clock.h"
#include "storage_path_port.h"
#include "storage_stream_port.h"
#include "storage_upload_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr uint32_t ServiceFgcbEraseBytes = 0x00020000;
static constexpr uint32_t ServiceFgcbProgramBytes = 32;
static constexpr uint32_t ServiceResetWaitMs = 10000;
static constexpr uint8_t ServiceResetMaxAttempts = 2;
static constexpr uint8_t ServiceProgressLogStep = 10;
static constexpr size_t FirmwareDumpChunkBytes = 64 * 1024;
static constexpr size_t FirmwareDumpReadBytes = 4080;
static constexpr uint64_t FirmwareDumpFreeReserveBytes = 1024 * 1024;
static constexpr uint32_t FirmwareDumpIdentityTimeoutMs = 30000;
static constexpr uint32_t FirmwareDumpRecoveryBootTimeoutMs = 30000;
static constexpr int32_t RpcMethodNotFound = -32601;
static constexpr size_t ResmedOtaKeyBytes = 32;

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
    return sha256_text_valid(value.c_str());
}

bool json_result_true(RpcPayloadView json) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(
        doc, json.data() ? json.data() : "", json.size());
    return !error && doc["result"].is<bool>() &&
           doc["result"].as<bool>();
}

String sha_to_hex(const uint8_t hash[32]) {
    char out[65] = {};
    if (!hex_encode(hash, 32, out, sizeof(out), HexCase::Upper)) return {};
    return String(out);
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
    String hex;
    hex.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        hex += hex_digit(bytes[i] >> 4, HexCase::Upper);
        hex += hex_digit(bytes[i], HexCase::Upper);
    }
    return hex;
}

bool build_apply_authentication(const String &key_hex,
                                const String &hash_hex,
                                String &authentication) {
    uint8_t key[ResmedOtaKeyBytes] = {};
    uint8_t hash[ResmedOtaKeyBytes] = {};
    uint8_t tag[ResmedOtaKeyBytes] = {};
    size_t key_length = 0;
    size_t hash_length = 0;

    if (!hex_decode(key_hex.c_str(), key_hex.length(), key, sizeof(key),
                    key_length) ||
        key_length != sizeof(key) ||
        !hex_decode(hash_hex.c_str(), hash_hex.length(), hash, sizeof(hash),
                    hash_length) ||
        hash_length != sizeof(hash)) {
        return false;
    }

    const mbedtls_md_info_t *sha256 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!sha256 ||
        mbedtls_md_hmac(sha256, key, sizeof(key), hash, sizeof(hash), tag) !=
            0) {
        return false;
    }

    char encoded[ResmedOtaKeyBytes * 2 + 1] = {};
    if (!hex_encode(tag, sizeof(tag), encoded, sizeof(encoded),
                    HexCase::Upper)) {
        return false;
    }

    authentication = encoded;
    return true;
}

using LittleEndian::put_le16;
using LittleEndian::put_le32;

}  // namespace

const char *resmed_ota_operation_name(ResmedOtaOperation operation) {
    switch (operation) {
        case ResmedOtaOperation::None: return "none";
        case ResmedOtaOperation::Install: return "install";
        case ResmedOtaOperation::Dump: return "dump";
    }
    return "none";
}

const char *resmed_ota_phase_name(ResmedOtaPhase phase) {
    switch (phase) {
        case ResmedOtaPhase::Idle: return "idle";
        case ResmedOtaPhase::ReadingIdentity: return "reading_identity";
        case ResmedOtaPhase::CheckingStorage: return "checking_storage";
        case ResmedOtaPhase::Opening: return "opening";
        case ResmedOtaPhase::EnteringService: return "entering_service";
        case ResmedOtaPhase::Dumping: return "dumping";
        case ResmedOtaPhase::Publishing: return "publishing";
        case ResmedOtaPhase::BootloaderRequired:
            return "bootloader_required";
        case ResmedOtaPhase::PreparingBootloader:
            return "preparing_bootloader";
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

struct ResmedOtaManager::ColdState {
    ResmedOtaStatus status;
    char pending_block_hex[AC_RESMED_OTA_MAX_BLOCK_BYTES * 2 + 1] = {};

    ResmedPreparedFirmware prepared;
    std::shared_ptr<StorageByteStream> prepared_stream;
    uint8_t prepared_block[AS11_SERVICE_V2_WRITE_DATA_MAX_BYTES] = {};
    std::unique_ptr<LargeByteBuffer> service_lz4_state;

    ResmedFirmwareDumpIdentity dump_identity;
    std::unique_ptr<LargeByteBuffer> dump_buffer;
    std::shared_ptr<const LargeByteBuffer> dump_pending_chunk;
    char dump_pending_error[AC_STORAGE_ERROR_MAX] = {};

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
                             ResmedFirmwarePreparer &preparer,
                             StorageStreamPort &stream_port,
                             StoragePathPort &path_port,
                             StorageUploadPort &upload_port,
                             const String &ota_key) {
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
    preparer_ = &preparer;
    stream_port_ = &stream_port;
    path_port_ = &path_port;
    upload_port_ = &upload_port;
    ota_key_ = &ota_key;
    mbedtls_sha256_init(&sha_ctx_);
    return true;
}

void ResmedOtaManager::poll() {
    ScopedLock lock(*this, 0);
    if (!lock || !cold_ || !rpc_) return;

    poll_preparer_result();
    poll_rpc_completion();
    poll_service_completion();
    poll_firmware_dump();
    poll_recovery_boot();
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
    cold_->status.operation = ResmedOtaOperation::Install;
    return begin_protocol(total_size, expected_sha256, filename);
}

void ResmedOtaManager::set_can_available(bool available) {
    ScopedLock lock(*this, 1000);
    if (!lock) return;

    can_available_ = available;
    if (available || !cold_) return;

    const ResmedOtaPhase phase = cold_->status.phase;
    const bool terminal = phase == ResmedOtaPhase::Idle ||
                          phase == ResmedOtaPhase::Complete ||
                          phase == ResmedOtaPhase::Error;
    if (!terminal) finish_error("can_transport_changed");
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
    cold_->status.operation = ResmedOtaOperation::Install;
    if (service_install && !can_available_) {
        set_error("can_transport_required");
        return false;
    }
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

bool ResmedOtaManager::request_firmware_dump() {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_ || !rpc_ || !device_ || !service_ ||
        !path_port_ || !upload_port_) {
        return false;
    }
    if (!can_available_) {
        clear_session();
        set_error("can_transport_required");
        return false;
    }
    if (active() || transport_active() || (preparer_ && preparer_->active())) {
        return false;
    }

    clear_session();
    cold_->status.operation = ResmedOtaOperation::Dump;
    cold_->status.transport = ResmedFirmwareInstallTransport::Service;
    cold_->status.target = "FGCB";
    if (!guard_device_idle_for_upgrade()) return false;

    return begin_dump_identity_refresh(false);
}

bool ResmedOtaManager::confirm_dump_bootloader(const String &confirm) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_ || !preparer_ ||
        cold_->status.operation != ResmedOtaOperation::Dump ||
        cold_->status.phase != ResmedOtaPhase::BootloaderRequired) {
        return false;
    }
    if (confirm != AC_RESMED_DUMP_BOOTLOADER_CONFIRM) {
        cold_->status.last_error = "confirmation_required";
        return false;
    }
    if (!cold_->status.recovery_available ||
        !cold_->dump_identity.patched_bootloader_path[0]) {
        set_error("patched_bootloader_not_found");
        return false;
    }

    cold_->status.confirmation_required = false;
    cold_->status.phase = ResmedOtaPhase::PreparingBootloader;
    cold_->status.last_error = "";
    cold_->status.last_result = "preparing_patched_bootloader";
    dump_recovery_prepare_pending_ = true;
    if (!preparer_->request(cold_->dump_identity.patched_bootloader_path,
                            "patched.bin", false,
                            ResmedFirmwareTarget::Fgbl,
                            ResmedFirmwareInstallTransport::Rpc)) {
        dump_recovery_prepare_pending_ = false;
        set_error("bootloader_prepare_rejected");
        return false;
    }
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

    return queue_plain_apply(reset_settings, false);
}

bool ResmedOtaManager::queue_plain_apply(bool reset_settings,
                                         bool allow_auth_fallback) {
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
    apply_auth_fallback_pending_ = allow_auth_fallback;
    if (!queue_request("ApplyUpgrade", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        apply_auth_fallback_pending_ = false;
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

    return queue_authenticated_apply(tag);
}

bool ResmedOtaManager::queue_authenticated_apply(
    const String &authentication) {
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
    params += "\",\"authentication\":\"";
    params += authentication.c_str();
    params += "\"}";
    cold_->status.phase = ResmedOtaPhase::Applying;
    cold_->status.apply_mode = "authenticated";
    last_activity_ms_ = millis();
    apply_auth_fallback_pending_ = false;
    if (!queue_request("ApplyAuthenticatedUpgrade", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("apply_queue_failed");
        return false;
    }

    Log::logf(CAT_OTA, LOG_WARN,
              "[RESMED] ApplyAuthenticatedUpgrade queued\n");
    return true;
}

bool ResmedOtaManager::queue_configured_authenticated_apply() {
    if (!ota_key_ || !ota_key_->length()) {
        set_error("ota_key_required");
        return false;
    }

    String authentication;
    if (!build_apply_authentication(*ota_key_,
                                    cold_->status.computed_sha256,
                                    authentication)) {
        set_error("ota_key_invalid");
        return false;
    }

    return queue_authenticated_apply(authentication);
}

void ResmedOtaManager::abort(const char *reason) {
    ScopedLock lock(*this, 1000);
    if (!lock || !cold_) return;

    const char *result = reason ? reason : "aborted";
    if (cold_->status.operation == ResmedOtaOperation::Dump &&
        dump_service_entered_) {
        set_error(result);
    } else {
        clear_session();
    }

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
           cold_->status.phase == ResmedOtaPhase::ReadingIdentity ||
           cold_->status.phase == ResmedOtaPhase::CheckingStorage ||
           cold_->status.phase == ResmedOtaPhase::Opening ||
           cold_->status.phase == ResmedOtaPhase::EnteringService ||
           cold_->status.phase == ResmedOtaPhase::Dumping ||
           cold_->status.phase == ResmedOtaPhase::Publishing ||
           cold_->status.phase == ResmedOtaPhase::BootloaderRequired ||
           cold_->status.phase == ResmedOtaPhase::PreparingBootloader ||
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
           cold_->status.phase == ResmedOtaPhase::ReadingIdentity ||
           cold_->status.phase == ResmedOtaPhase::CheckingStorage ||
           cold_->status.phase == ResmedOtaPhase::Opening ||
           cold_->status.phase == ResmedOtaPhase::EnteringService ||
           cold_->status.phase == ResmedOtaPhase::Dumping ||
           cold_->status.phase == ResmedOtaPhase::Publishing ||
           cold_->status.phase == ResmedOtaPhase::Erasing ||
           cold_->status.phase == ResmedOtaPhase::Initiating ||
           cold_->status.phase == ResmedOtaPhase::Ready ||
           cold_->status.phase == ResmedOtaPhase::Uploading ||
           cold_->status.phase == ResmedOtaPhase::Uploaded ||
           cold_->status.phase == ResmedOtaPhase::Checking ||
           cold_->status.phase == ResmedOtaPhase::Applying ||
           cold_->status.phase == ResmedOtaPhase::Resetting;
}

bool ResmedOtaManager::storage_upload_active() const {
    return dump_upload_active_.load(std::memory_order_acquire);
}

ResmedOtaStatus ResmedOtaManager::status() const {
    ScopedLock lock(*this, 50);
    if (!lock) return {};

    ResmedOtaStatus status = cold_ ? cold_->status : ResmedOtaStatus{};
    status.can_available = can_available_;
    return status;
}

const char *ResmedOtaManager::phase_name() const {
    ScopedLock lock(*this, 50);
    if (!lock) return "busy";
    if (!cold_) return "unavailable";
    return resmed_ota_phase_name(cold_->status.phase);
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
        int32_t error_code = 0;
        const bool method_unavailable =
            waiting_for_ == WaitingFor::Apply &&
            apply_auth_fallback_pending_ &&
            cold_->status.apply_mode == "plain" &&
            json_extract_rpc_error_code(payload.data(), payload.size(),
                                        error_code) &&
            error_code == RpcMethodNotFound;

        waiting_for_ = WaitingFor::None;
        apply_auth_fallback_pending_ = false;
        if (method_unavailable) {
            cold_->status.phase = ResmedOtaPhase::Verified;
            (void)queue_configured_authenticated_apply();
            return;
        }

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
            if (apply_after_check_) {
                (void)queue_plain_apply(false, true);
            }
            break;

        case WaitingFor::Apply:
            apply_auth_fallback_pending_ = false;
            if (dump_recovery_install_) {
                cold_->status.phase = ResmedOtaPhase::Resetting;
                cold_->status.last_result = "waiting_for_application";
                recovery_boot_started_ms_ = millis();
            } else {
                cold_->status.phase = ResmedOtaPhase::Complete;
            }
            break;

        case WaitingFor::None:
            break;
    }
}

void ResmedOtaManager::poll_preparer_result() {
    if (!preparer_) return;

    if (dump_recovery_prepare_pending_) {
        const ResmedFirmwarePrepareStatus status = preparer_->status();
        if (status.state == ResmedFirmwarePrepareState::Error) {
            dump_recovery_prepare_pending_ = false;
            set_error(status.error[0] ? status.error
                                      : "bootloader_prepare_failed");
            return;
        }
        if (status.state == ResmedFirmwarePrepareState::Idle &&
            !preparer_->active()) {
            dump_recovery_prepare_pending_ = false;
            set_error("bootloader_prepare_cancelled");
            return;
        }
    }

    if (!dump_recovery_prepare_pending_ &&
        (active() || transport_active())) {
        return;
    }

    ResmedPreparedFirmware firmware;
    bool cancelled = false;
    if (!preparer_->take_result(firmware, cancelled)) return;

    if (dump_recovery_prepare_pending_) {
        dump_recovery_prepare_pending_ = false;
        if (cancelled || !firmware.valid()) {
            (void)discard_prepared_firmware(firmware);
            set_error(cancelled ? "bootloader_prepare_cancelled"
                                : "bootloader_prepare_failed");
            return;
        }
        if (!begin_recovery_install(firmware)) {
            (void)discard_prepared_firmware(firmware);
            if (cold_->status.phase != ResmedOtaPhase::Error) {
                set_error("bootloader_install_rejected");
            }
        }
        return;
    }

    const bool accepted = cancelled
        ? discard_prepared_firmware(firmware)
        : begin_prepared_install(firmware);
    if (!accepted) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "[RESMED] prepared firmware handoff rejected\n");
    }
}

bool ResmedOtaManager::begin_recovery_install(
    const ResmedPreparedFirmware &firmware) {
    if (!firmware.valid() || firmware.transport !=
                                 ResmedFirmwareInstallTransport::Rpc ||
        strcmp(firmware.image.target, "FGBL") != 0 ||
        cold_->status.operation != ResmedOtaOperation::Dump ||
        cold_->status.phase != ResmedOtaPhase::PreparingBootloader) {
        return false;
    }
    if (!can_available_) {
        set_error("can_transport_required");
        return false;
    }
    if (!guard_device_idle_for_upgrade()) return false;

    cold_->prepared = firmware;
    prepared_transfer_ = true;
    apply_after_check_ = true;
    dump_recovery_install_ = true;
    recovery_boot_revision_ = device_ ? device_->boot_revision() : 0;
    recovery_boot_started_ms_ = 0;

    cold_->status.phase = ResmedOtaPhase::Opening;
    cold_->status.total_size =
        static_cast<size_t>(firmware.image.prepared_size);
    cold_->status.uploaded_bytes = 0;
    cold_->status.progress_percent = 0;
    cold_->status.filename = firmware.filename;
    cold_->status.input_type =
        resmed_firmware_image_kind_name(firmware.image.kind);
    cold_->status.transport = ResmedFirmwareInstallTransport::Rpc;
    cold_->status.target = firmware.image.target;
    cold_->status.source_path = firmware.path;
    cold_->status.last_result = "installing_patched_bootloader";
    last_activity_ms_ = millis();

    Log::logf(CAT_OTA, LOG_WARN,
              "[RESMED] installing confirmed patched bootloader "
              "version=%s path=%s\n",
              cold_->dump_identity.bootloader_version, firmware.path);
    return true;
}

void ResmedOtaManager::poll_recovery_boot() {
    if (!dump_recovery_install_ || !device_ ||
        cold_->status.phase != ResmedOtaPhase::Resetting ||
        recovery_boot_started_ms_ == 0) {
        return;
    }

    if (device_->boot_revision() != recovery_boot_revision_) {
        dump_recovery_install_ = false;
        close_prepared_stream(true);
        schedule_prepared_cleanup();
        (void)begin_dump_identity_refresh(true);
        return;
    }

    if (millis_elapsed_at_least(millis(), recovery_boot_started_ms_,
                                FirmwareDumpRecoveryBootTimeoutMs)) {
        set_error("bootloader_application_not_observed");
    }
}

void ResmedOtaManager::poll_firmware_dump() {
    if (cold_->status.operation != ResmedOtaOperation::Dump) return;

    switch (cold_->status.phase) {
        case ResmedOtaPhase::ReadingIdentity:
            if (device_->identity_revision() != dump_identity_revision_) {
                if (!capture_dump_identity()) return;

                cold_->status.phase = ResmedOtaPhase::CheckingStorage;
                dump_path_check_ = DumpPathCheck::Output;
                (void)request_dump_path_check(dump_path_check_);
                return;
            }
            if (millis_elapsed_at_least(millis(), dump_identity_started_ms_,
                                        FirmwareDumpIdentityTimeoutMs)) {
                set_error("identity_refresh_timeout");
            }
            return;

        case ResmedOtaPhase::CheckingStorage:
            poll_dump_path_check();
            return;

        case ResmedOtaPhase::Dumping:
        case ResmedOtaPhase::Publishing:
            poll_dump_upload();
            return;

        case ResmedOtaPhase::Resetting:
            if (!dump_recovery_install_) poll_service_reset();
            return;

        case ResmedOtaPhase::Idle:
        case ResmedOtaPhase::Opening:
        case ResmedOtaPhase::EnteringService:
        case ResmedOtaPhase::BootloaderRequired:
        case ResmedOtaPhase::PreparingBootloader:
        case ResmedOtaPhase::Erasing:
        case ResmedOtaPhase::Initiating:
        case ResmedOtaPhase::Ready:
        case ResmedOtaPhase::Uploading:
        case ResmedOtaPhase::Uploaded:
        case ResmedOtaPhase::Checking:
        case ResmedOtaPhase::Verified:
        case ResmedOtaPhase::Applying:
        case ResmedOtaPhase::Complete:
        case ResmedOtaPhase::Error:
            return;
    }
}

bool ResmedOtaManager::begin_dump_identity_refresh(bool after_recovery) {
    if (!device_ || !rpc_) return false;

    dump_identity_revision_ = device_->identity_revision();
    dump_identity_started_ms_ = millis();
    cold_->status.phase = ResmedOtaPhase::ReadingIdentity;
    cold_->status.operation = ResmedOtaOperation::Dump;
    cold_->status.transport = ResmedFirmwareInstallTransport::Service;
    cold_->status.target = "FGCB";
    cold_->status.filename = "";
    cold_->status.source_path = "";
    cold_->status.last_error = "";
    cold_->status.last_result = after_recovery
        ? "refreshing_identity_after_bootloader_install"
        : "refreshing_identity";
    cold_->status.confirmation_required = false;
    cold_->status.recovery_available = false;
    last_activity_ms_ = millis();

    if (!device_->request_identity_refresh(*rpc_, RpcSource::ResmedOta,
                                           dump_identity_started_ms_)) {
        set_error("identity_refresh_rejected");
        return false;
    }
    return true;
}

bool ResmedOtaManager::capture_dump_identity() {
    const As11DeviceState &state = device_->state();
    if (!state.variant_id_valid() ||
        !resmed_firmware_dump_identity(
            state.product_name().c_str(),
            state.software_identifier().c_str(),
            state.bootloader_identifier().c_str(), state.variant_id(),
            cold_->dump_identity)) {
        set_error("firmware_identity_invalid");
        return false;
    }

    uint32_t flash_start = 0;
    uint64_t payload_size = 0;
    if (!resmed_firmware_target_range(ResmedFirmwareTarget::Fgcb,
                                      flash_start, payload_size) ||
        payload_size > SIZE_MAX) {
        set_error("firmware_dump_range_invalid");
        return false;
    }

    dump_flash_start_ = flash_start;
    cold_->status.total_size = static_cast<size_t>(payload_size);
    cold_->status.uploaded_bytes = 0;
    cold_->status.progress_percent = 0;
    cold_->status.filename = cold_->dump_identity.filename;
    cold_->status.output_path = cold_->dump_identity.output_path;
    cold_->status.recovery_path =
        cold_->dump_identity.patched_bootloader_path;
    cold_->status.last_result = "identity_ready";
    return true;
}

bool ResmedOtaManager::request_dump_path_check(DumpPathCheck check) {
    if (!path_port_ || dump_path_ticket_.valid() ||
        check == DumpPathCheck::None) {
        return false;
    }

    const char *path = cold_->dump_identity.patched_bootloader_path;
    if (check == DumpPathCheck::Output) {
        path = cold_->dump_identity.output_path;
    }
    dump_path_generation_++;
    if (dump_path_generation_ == 0) dump_path_generation_++;

    StoragePathCommand command;
    command.operation = StoragePathOperation::Stat;
    command.source = path;
    command.generation = dump_path_generation_;
    const OperationSubmission submission = path_port_->request(command);
    if (submission.accepted()) {
        dump_path_check_ = check;
        dump_path_ticket_ = submission.ticket;
        cold_->status.waiting = true;
        return true;
    }
    if (submission.admission == OperationAdmission::Rejected) {
        set_error("firmware_path_check_rejected");
    }
    return false;
}

void ResmedOtaManager::poll_dump_path_check() {
    if (dump_path_check_ == DumpPathCheck::None) {
        set_error("firmware_path_check_missing");
        return;
    }
    if (!dump_path_ticket_.valid()) {
        (void)request_dump_path_check(dump_path_check_);
        return;
    }

    StoragePathCompletion completion;
    if (!path_port_->take_completion(dump_path_ticket_, completion)) return;

    dump_path_ticket_ = {};
    cold_->status.waiting = false;
    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        set_error(completion.error[0] ? completion.error
                                     : "firmware_path_check_failed");
        return;
    }

    const DumpPathCheck completed = dump_path_check_;
    dump_path_check_ = DumpPathCheck::None;
    if (completed == DumpPathCheck::Output) {
        if (completion.exists) {
            set_error("firmware_dump_exists");
            return;
        }
        (void)begin_dump_service();
        return;
    }

    if (!completion.exists || completion.directory || completion.size == 0) {
        set_error("patched_bootloader_not_found");
        return;
    }

    cold_->status.phase = ResmedOtaPhase::BootloaderRequired;
    cold_->status.confirmation_required = true;
    cold_->status.recovery_available = true;
    cold_->status.last_result = "patched_bootloader_available";
    cold_->status.last_error = "service_entry_timeout";
}

bool ResmedOtaManager::begin_dump_service() {
    if (!service_ || !service_->acquire(As11ServiceOwner::ResmedOta)) {
        set_error("service_busy");
        return false;
    }

    service_owned_ = true;
    service_started_ms_ = millis();
    service_reset_attempts_ = 0;
    service_reset_accepted_ms_ = 0;
    service_reset_boot_revision_ = 0;
    dump_service_entered_ = false;
    cold_->status.phase = ResmedOtaPhase::EnteringService;
    cold_->status.last_result = "entering_service";
    return submit_service_request(AS11_SERVICE_COMMAND_ENTER, nullptr, 0,
                                  ServiceWaitingFor::Enter);
}

bool ResmedOtaManager::begin_dump_upload() {
    if (!upload_port_ || dump_upload_id_ != 0) {
        set_error("firmware_dump_upload_unavailable");
        return false;
    }

    dump_upload_generation_++;
    if (dump_upload_generation_ == 0) dump_upload_generation_++;

    StorageUploadStartCommand command;
    command.path = cold_->dump_identity.output_path;
    command.total_size = cold_->status.total_size;
    command.free_reserve_bytes = FirmwareDumpFreeReserveBytes;
    command.conflict = StorageUploadConflict::Fail;
    command.generation = dump_upload_generation_;
    const StorageUploadStartResult started = upload_port_->start(command);
    if (!started.accepted()) {
        set_error(started.error[0] ? started.error
                                   : "firmware_dump_upload_rejected");
        return false;
    }

    dump_upload_id_ = started.id;
    dump_upload_active_.store(true, std::memory_order_release);
    cold_->status.phase = ResmedOtaPhase::Publishing;
    cold_->status.last_result = "opening_dump";
    return true;
}

void ResmedOtaManager::poll_dump_upload() {
    if (!upload_port_ || dump_upload_id_ == 0) {
        set_error("firmware_dump_upload_missing");
        return;
    }

    StorageUploadStatus upload;
    const StorageUploadStatusRead read =
        upload_port_->status(dump_upload_id_, upload);
    if (read == StorageUploadStatusRead::Busy) return;
    if (read == StorageUploadStatusRead::NotFound) {
        set_error("firmware_dump_upload_not_found");
        return;
    }

    cold_->status.uploaded_bytes =
        static_cast<size_t>(upload.committed_bytes);
    update_progress();
    if (cold_->dump_pending_chunk &&
        upload.committed_bytes >=
            dump_chunk_offset_ + cold_->dump_pending_chunk->size()) {
        cold_->dump_pending_chunk.reset();
    }

    switch (upload.state) {
        case StorageUploadState::Preparing:
        case StorageUploadState::Writing:
        case StorageUploadState::Paused:
        case StorageUploadState::Publishing:
            return;

        case StorageUploadState::Ready:
            cold_->status.phase = ResmedOtaPhase::Dumping;
            if (cold_->dump_pending_chunk) return;
            if (dump_buffer_bytes_ != 0) {
                (void)submit_dump_chunk();
                return;
            }
            if (dump_read_offset_ < cold_->status.total_size) {
                (void)submit_dump_read();
            }
            return;

        case StorageUploadState::Done:
            dump_upload_id_ = 0;
            dump_upload_active_.store(false, std::memory_order_release);
            cold_->dump_pending_chunk.reset();
            cold_->dump_buffer.reset();
            dump_buffer_bytes_ = 0;
            cold_->status.uploaded_bytes = cold_->status.total_size;
            update_progress();
            cold_->status.last_result = "dump_published";
            (void)submit_service_reset();
            return;

        case StorageUploadState::Cancelled:
            dump_upload_id_ = 0;
            dump_upload_active_.store(false, std::memory_order_release);
            set_error("firmware_dump_upload_cancelled");
            return;

        case StorageUploadState::Error:
            dump_upload_id_ = 0;
            dump_upload_active_.store(false, std::memory_order_release);
            set_error(upload.error[0] ? upload.error
                                      : "firmware_dump_upload_failed");
            return;

        case StorageUploadState::Idle:
            set_error("firmware_dump_upload_idle");
            return;
    }
}

bool ResmedOtaManager::submit_dump_read() {
    if (service_waiting_for_ != ServiceWaitingFor::None ||
        dump_read_offset_ >= cold_->status.total_size) {
        return false;
    }

    if (!cold_->dump_buffer) {
        const size_t capacity = std::min(
            FirmwareDumpChunkBytes,
            cold_->status.total_size -
                static_cast<size_t>(dump_read_offset_));
        cold_->dump_buffer = LargeByteBuffer::allocate(capacity);
        if (!cold_->dump_buffer) {
            set_error("firmware_dump_buffer_alloc_failed");
            return false;
        }
        dump_buffer_bytes_ = 0;
    }

    const size_t remaining_file =
        cold_->status.total_size - static_cast<size_t>(dump_read_offset_);
    const size_t remaining_buffer =
        cold_->dump_buffer->size() - dump_buffer_bytes_;
    dump_read_bytes_ = std::min({FirmwareDumpReadBytes, remaining_file,
                                 remaining_buffer});
    if (dump_read_bytes_ == 0) {
        return submit_dump_chunk();
    }

    uint8_t payload[7] = {AS11_SERVICE_TARGET_FGCB};
    put_le32(payload + 1, dump_flash_start_ + dump_read_offset_);
    put_le16(payload + 5, static_cast<uint16_t>(dump_read_bytes_));
    return submit_service_request(AS11_SERVICE_COMMAND_READ, payload,
                                  sizeof(payload),
                                  ServiceWaitingFor::Read);
}

bool ResmedOtaManager::submit_dump_chunk() {
    if (!upload_port_ || dump_upload_id_ == 0) return false;

    if (!cold_->dump_pending_chunk) {
        if (!cold_->dump_buffer || dump_buffer_bytes_ == 0 ||
            !cold_->dump_buffer->truncate(dump_buffer_bytes_)) {
            set_error("firmware_dump_chunk_invalid");
            return false;
        }

        dump_chunk_offset_ = dump_read_offset_ - dump_buffer_bytes_;
        cold_->dump_pending_chunk =
            LargeByteBuffer::freeze(std::move(cold_->dump_buffer));
        dump_buffer_bytes_ = 0;
    }

    StorageUploadChunkCommand command;
    command.id = dump_upload_id_;
    command.offset = dump_chunk_offset_;
    command.bytes = cold_->dump_pending_chunk;
    const StorageUploadChunkResult submitted = upload_port_->submit(command);
    if (submitted.accepted() ||
        submitted.admission == OperationAdmission::Busy) {
        return submitted.accepted();
    }

    set_error(submitted.error[0] ? submitted.error
                                 : "firmware_dump_chunk_rejected");
    return false;
}

void ResmedOtaManager::finish_dump_read(
    const std::shared_ptr<const LargeByteBuffer> &response,
    const As11ServicePacketHeader &header) {
    if (!cold_->dump_buffer || dump_read_bytes_ == 0 ||
        header.payload_length != dump_read_bytes_ ||
        dump_buffer_bytes_ + dump_read_bytes_ > cold_->dump_buffer->size()) {
        set_error("firmware_dump_read_invalid");
        return;
    }

    memcpy(cold_->dump_buffer->data() + dump_buffer_bytes_,
           response->data() + AS11_SERVICE_PACKET_HEADER_BYTES,
           dump_read_bytes_);
    dump_buffer_bytes_ += dump_read_bytes_;
    dump_read_offset_ += dump_read_bytes_;
    dump_read_bytes_ = 0;

    if (dump_buffer_bytes_ == cold_->dump_buffer->size() ||
        dump_read_offset_ == cold_->status.total_size) {
        (void)submit_dump_chunk();
    }
}

void ResmedOtaManager::require_dump_bootloader() {
    release_service();
    dump_service_entered_ = false;
    cold_->status.phase = ResmedOtaPhase::CheckingStorage;
    cold_->status.waiting = false;
    cold_->status.last_result = "checking_patched_bootloader";
    cold_->status.last_error = "service_entry_timeout";
    dump_path_check_ = DumpPathCheck::Bootloader;
    (void)request_dump_path_check(dump_path_check_);
}

void ResmedOtaManager::finish_firmware_dump() {
    const uint32_t elapsed_ms = millis() - service_started_ms_;
    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] firmware dump complete bytes=%u path=%s "
              "elapsed_ms=%lu\n",
              static_cast<unsigned>(cold_->status.uploaded_bytes),
              cold_->dump_identity.output_path,
              static_cast<unsigned long>(elapsed_ms));

    cold_->status.phase = ResmedOtaPhase::Complete;
    cold_->status.last_result = "firmware_dump_ready";
    cold_->status.waiting = false;
    dump_service_entered_ = false;
    release_service();
}

void ResmedOtaManager::cancel_dump_upload() {
    if (upload_port_ && dump_upload_id_ != 0) {
        (void)upload_port_->cancel(dump_upload_id_);
    }
    dump_upload_id_ = 0;
    dump_upload_active_.store(false, std::memory_order_release);
    cold_->dump_buffer.reset();
    cold_->dump_pending_chunk.reset();
    dump_buffer_bytes_ = 0;
    dump_read_bytes_ = 0;
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
        case ServiceWaitingFor::Read:
            expected_command = AS11_SERVICE_COMMAND_READ;
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

    if (completed == ServiceWaitingFor::Enter &&
        cold_->status.operation == ResmedOtaOperation::Dump &&
        header.status == AS11_SERVICE_STATUS_ENTRY_TIMEOUT) {
        require_dump_bootloader();
        return;
    }

    if (header.status != AS11_SERVICE_STATUS_OK) {
        char reason[40];
        snprintf(reason, sizeof(reason), "service_status_%u",
                 static_cast<unsigned>(header.status));
        set_error(reason);
        return;
    }
    if (completed == ServiceWaitingFor::Read) {
        finish_dump_read(response, header);
        return;
    }
    if (completed != ServiceWaitingFor::Enter && header.payload_length != 0) {
        set_error("service_response_payload");
        return;
    }

    cold_->status.last_result = "ok";
    switch (completed) {
        case ServiceWaitingFor::Enter:
            if (cold_->status.operation == ResmedOtaOperation::Dump) {
                dump_service_entered_ = true;
                Log::logf(CAT_OTA, LOG_INFO,
                          "[RESMED] service entered for firmware dump\n");
                (void)begin_dump_upload();
            } else {
                cold_->status.phase = ResmedOtaPhase::Erasing;
                Log::logf(CAT_OTA, LOG_INFO,
                          "[RESMED] service entered target=%s\n",
                          cold_->status.target.c_str());
            }
            break;

        case ServiceWaitingFor::Read:
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
        if (cold_->status.operation == ResmedOtaOperation::Dump) {
            if (dump_error_reset_pending_) {
                char error[AC_STORAGE_ERROR_MAX] = {};
                copy_cstr(error, sizeof(error), cold_->dump_pending_error);
                dump_error_reset_pending_ = false;
                cold_->dump_pending_error[0] = '\0';
                finish_error(error[0] ? error : "firmware_dump_failed");
            } else {
                finish_firmware_dump();
            }
        } else {
            finish_service_install();
        }
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

    if (dump_error_reset_pending_) {
        char error[AC_STORAGE_ERROR_MAX] = {};
        copy_cstr(error, sizeof(error), cold_->dump_pending_error);
        dump_error_reset_pending_ = false;
        cold_->dump_pending_error[0] = '\0';
        finish_error(error[0] ? error : "service_reset_not_observed");
    } else {
        set_error("service_reset_not_observed");
    }
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
    if (path_port_ && dump_path_ticket_.valid()) {
        (void)path_port_->abandon(dump_path_ticket_);
    }
    dump_path_ticket_ = {};
    dump_path_check_ = DumpPathCheck::None;
    cancel_dump_upload();
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
    apply_auth_fallback_pending_ = false;
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
    cold_->dump_identity = {};
    cold_->dump_pending_error[0] = '\0';
    dump_identity_revision_ = 0;
    dump_identity_started_ms_ = 0;
    dump_flash_start_ = 0;
    dump_read_offset_ = 0;
    dump_read_bytes_ = 0;
    dump_buffer_bytes_ = 0;
    dump_chunk_offset_ = 0;
    dump_service_entered_ = false;
    dump_recovery_prepare_pending_ = false;
    dump_recovery_install_ = false;
    dump_error_reset_pending_ = false;
    recovery_boot_revision_ = 0;
    recovery_boot_started_ms_ = 0;
}

void ResmedOtaManager::set_error(const char *error) {
    if (dump_error_reset_pending_) {
        char original_error[AC_STORAGE_ERROR_MAX] = {};
        copy_cstr(original_error, sizeof(original_error),
                  cold_->dump_pending_error);
        dump_error_reset_pending_ = false;
        cold_->dump_pending_error[0] = '\0';
        finish_error(original_error[0] ? original_error : error);
        return;
    }

    if (cold_->status.operation == ResmedOtaOperation::Dump &&
        dump_service_entered_ && service_) {
        copy_cstr(cold_->dump_pending_error,
                  sizeof(cold_->dump_pending_error),
                  error ? error : "firmware_dump_failed");
        cancel_dump_upload();
        close_prepared_stream(false);
        prepared_transfer_ = false;
        apply_after_check_ = false;

        if (service_owned_) {
            service_->release(As11ServiceOwner::ResmedOta);
        }
        service_owned_ = service_->acquire(As11ServiceOwner::ResmedOta);
        service_waiting_for_ = ServiceWaitingFor::None;
        if (service_owned_) {
            dump_error_reset_pending_ = true;
            if (submit_service_reset()) return;
            return;
        }
        dump_error_reset_pending_ = false;
    }

    finish_error(error);
}

void ResmedOtaManager::finish_error(const char *error) {
    release_service();
    cancel_dump_upload();
    close_prepared_stream(false);
    schedule_prepared_cleanup();
    prepared_transfer_ = false;
    apply_after_check_ = false;
    apply_auth_fallback_pending_ = false;

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
