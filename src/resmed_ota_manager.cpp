#include "resmed_ota_manager.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <utility>

#include <ArduinoJson.h>

#include "as11_rpc.h"
#include "crc32.h"
#include "debug_log.h"

namespace aircannect {
namespace {

static constexpr size_t ABC_PRIMARY_SIZE = 0x58;
static constexpr size_t ABC_DESCRIPTOR_SIZE = 0x50;
static constexpr size_t ABC_PAYLOAD_OFFSET_0005 =
    ABC_PRIMARY_SIZE + ABC_DESCRIPTOR_SIZE;
static constexpr size_t ABC_SEGMENT_ENTRY_SIZE = 8;
static constexpr size_t AS11_FULL_FLASH_SIZE = 0x00200000;
static constexpr size_t AS11_CONF_SIZE = 0x00020000;
static constexpr size_t AS11_APPL_SIZE = 0x001C0000;
static constexpr size_t AS11_APCX_SIZE = 0x001E0000;
static constexpr uint32_t AS11_CONF_FLASH_START = 0x08020000;
static constexpr uint32_t AS11_APPL_FLASH_START = 0x08040000;
static constexpr const char *ABC_COMPONENT_0005 = "PacificFG";
static constexpr uint32_t STORAGE_PUBLISH_RETRY_MS = 50;

struct RawTargetSpec {
    const char *code = nullptr;
    uint32_t flash_start = 0;
    size_t payload_size = 0;
    size_t source_offset = 0;
    bool desc2_required = false;
    bool desc3_required = false;
};

struct DescriptorPreset {
    const char *version = nullptr;
    uint32_t desc2 = 0;
    uint32_t desc3 = 0;
};

const DescriptorPreset DESCRIPTOR_PRESETS[] = {
    {"14.8.3.0", 0x2D89E58Fu, 0xBEB37EE2u},
    {"15.8.4.0", 0xD785ABA6u, 0xBEB37EE2u},
    {"16.8.5.0", 0x7862CBA7u, 0xBEB37EE2u},
};

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
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

void put_le32(uint8_t *out, size_t offset, uint32_t value) {
    out[offset + 0] = static_cast<uint8_t>(value & 0xFFu);
    out[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    out[offset + 2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    out[offset + 3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

bool starts_with_abc_magic(const String &magic_hex) {
    String magic = magic_hex;
    magic.trim();
    magic.toUpperCase();
    return magic.startsWith("4F544121");
}

bool filename_ends_with_abc(String filename) {
    filename.trim();
    filename.toLowerCase();
    return filename.endsWith(".abc");
}

bool infer_raw_target(size_t input_size, RawTargetSpec &target) {
    if (input_size == AS11_FULL_FLASH_SIZE) {
        target = {"APCX", AS11_CONF_FLASH_START, AS11_APCX_SIZE,
                  AS11_CONF_SIZE, false, true};
        return true;
    }
    if (input_size == AS11_APCX_SIZE) {
        target = {"APCX", AS11_CONF_FLASH_START, AS11_APCX_SIZE,
                  0, false, true};
        return true;
    }
    if (input_size == AS11_APPL_SIZE) {
        target = {"APPL", AS11_APPL_FLASH_START, AS11_APPL_SIZE,
                  0, true, true};
        return true;
    }
    if (input_size == AS11_CONF_SIZE) {
        target = {"CONF", AS11_CONF_FLASH_START, AS11_CONF_SIZE,
                  0, true, false};
        return true;
    }
    return false;
}

bool extract_descriptor_version(const std::string &identifier,
                                char *out,
                                size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    const char *text = identifier.c_str();
    for (size_t i = 0; text[i]; ++i) {
        if (!isdigit(static_cast<unsigned char>(text[i]))) continue;
        int a = 0;
        int b = 0;
        int c = 0;
        int d = 0;
        int consumed = 0;
        if (sscanf(text + i, "%d.%d.%d.%d%n",
                   &a, &b, &c, &d, &consumed) == 4 &&
            a > 0 && a < 100 && b >= 0 && b < 100 &&
            c >= 0 && c < 100 && d >= 0 && d < 100) {
            snprintf(out, out_size, "%d.%d.%d.%d", a, b, c, d);
            return true;
        }
    }
    return false;
}

bool descriptor_preset_for_version(const char *version,
                                   uint32_t &desc2,
                                   uint32_t &desc3) {
    if (!version || !*version) return false;
    for (const DescriptorPreset &preset : DESCRIPTOR_PRESETS) {
        if (!strcmp(version, preset.version)) {
            desc2 = preset.desc2;
            desc3 = preset.desc3;
            return true;
        }
    }
    return false;
}

bool json_result_true(const std::string &json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) return false;
    return doc["result"].is<bool>() && doc["result"].as<bool>();
}

String sha_to_hex(const uint8_t hash[32]) {
    static const char *digits = "0123456789ABCDEF";
    String out;
    out.reserve(64);
    for (size_t i = 0; i < 32; ++i) {
        out += digits[(hash[i] >> 4) & 0x0F];
        out += digits[hash[i] & 0x0F];
    }
    return out;
}

bool update_sha_from_hex(mbedtls_sha256_context &ctx, const String &hex) {
    uint8_t bytes[64];
    size_t count = 0;
    for (size_t i = 0; i < hex.length(); i += 2) {
        const int hi = hex_nibble(hex[i]);
        const int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[count++] = static_cast<uint8_t>((hi << 4) | lo);
        if (count == sizeof(bytes)) {
            mbedtls_sha256_update(&ctx, bytes, count);
            count = 0;
        }
    }
    if (count) mbedtls_sha256_update(&ctx, bytes, count);
    return true;
}

}  // namespace

ResmedOtaManager::ScopedLock::ScopedLock(const ResmedOtaManager &manager,
                                         uint32_t timeout_ms)
    : manager_(manager) {
    locked_ = manager_.lock(timeout_ms);
}

ResmedOtaManager::ScopedLock::~ScopedLock() {
    if (locked_) manager_.unlock();
}

void ResmedOtaManager::begin(RpcArbiter &arbiter,
                             As11DeviceService &device,
                             StorageAtomicWritePort &storage_write_port) {
    arbiter_ = &arbiter;
    device_ = &device;
    storage_write_port_ = &storage_write_port;
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();
    mbedtls_sha256_init(&sha_ctx_);
}

void ResmedOtaManager::poll() {
    ScopedLock lock(*this, 0);
    if (!lock) return;
    if (!arbiter_) return;
    RpcEvent event;
    while (arbiter_->next_source_event(RpcSource::ResmedOta, event)) {
        handle_event(event);
    }

    poll_staged_publication();
    pump_staged_upload();
    if ((status_.phase == ResmedOtaPhase::Staging ||
         status_.phase == ResmedOtaPhase::Ready ||
         status_.phase == ResmedOtaPhase::Uploaded) &&
        last_activity_ms_ &&
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
    if (!arbiter_) {
        set_error("not_initialized");
        return false;
    }
    if (!guard_device_idle_for_upgrade()) return false;
    if (transport_active() || active()) {
        set_error("session_active");
        return false;
    }
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

    clear_session();
    status_.phase = ResmedOtaPhase::Initiating;
    status_.total_size = total_size;
    status_.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    status_.filename = filename;
    status_.expected_sha256 = expected;
    last_activity_ms_ = millis();
    mbedtls_sha256_starts(&sha_ctx_, 0);
    sha_started_ = true;

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

bool ResmedOtaManager::submit_block(size_t offset, const String &hex_data) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (!arbiter_) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (status_.phase != ResmedOtaPhase::Ready &&
        status_.phase != ResmedOtaPhase::Uploading) {
        set_error("not_ready");
        return false;
    }
    if (offset != status_.uploaded_bytes || offset >= status_.total_size) {
        set_error("bad_offset");
        return false;
    }

    String hex = hex_data;
    if (!normalize_hex(hex, status_.xfer_block_size)) {
        set_error("bad_hex_block");
        return false;
    }
    const size_t raw_len = hex.length() / 2;
    if (raw_len == 0 || offset + raw_len > status_.total_size) {
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

    pending_block_hex_ = hex;
    pending_block_offset_ = offset;
    pending_block_bytes_ = raw_len;
    status_.phase = ResmedOtaPhase::Uploading;
    last_activity_ms_ = millis();
    if (!queue_request("UpgradeDataBlock", params,
                       AC_RESMED_OTA_BLOCK_TIMEOUT_MS)) {
        pending_block_hex_ = "";
        pending_block_offset_ = 0;
        pending_block_bytes_ = 0;
        set_error("block_queue_failed");
        return false;
    }
    return true;
}

bool ResmedOtaManager::request_check() {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (status_.phase != ResmedOtaPhase::Uploaded ||
        status_.uploaded_bytes != status_.total_size) {
        set_error("upload_not_complete");
        return false;
    }
    if (!finish_hash()) return false;
    if (status_.expected_sha256.length() &&
        status_.expected_sha256 != status_.computed_sha256) {
        set_error("sha256_mismatch");
        return false;
    }

    std::string params = "{\"upgradeFileHash\":\"";
    params += status_.computed_sha256.c_str();
    params += "\"}";
    status_.phase = ResmedOtaPhase::Checking;
    last_activity_ms_ = millis();
    if (!queue_request("CheckUpgradeFile", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("check_queue_failed");
        return false;
    }
    Log::logf(CAT_OTA, LOG_INFO, "[RESMED] check sha256=%s\n",
              status_.computed_sha256.c_str());
    return true;
}

bool ResmedOtaManager::request_apply_plain(bool reset_settings,
                                           const String &confirm) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (status_.phase != ResmedOtaPhase::Verified ||
        !status_.computed_sha256.length()) {
        set_error("not_verified");
        return false;
    }
    if (confirm != AC_RESMED_OTA_CONFIRM) {
        set_error("confirmation_required");
        return false;
    }

    std::string params = "{\"upgradeFileHash\":\"";
    params += status_.computed_sha256.c_str();
    params += "\",\"resetSettingsToDefault\":";
    params += reset_settings ? "true" : "false";
    params += "}";
    status_.phase = ResmedOtaPhase::Applying;
    status_.apply_mode = "plain";
    last_activity_ms_ = millis();
    if (!queue_request("ApplyUpgrade", params,
                       AC_RESMED_OTA_VERIFY_TIMEOUT_MS)) {
        set_error("apply_queue_failed");
        return false;
    }
    Log::logf(CAT_OTA, LOG_WARN, "[RESMED] ApplyUpgrade queued\n");
    return true;
}

bool ResmedOtaManager::request_apply_authenticated(
    const String &authentication,
    const String &confirm) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (waiting_for_ != WaitingFor::None) {
        set_error("busy");
        return false;
    }
    if (status_.phase != ResmedOtaPhase::Verified ||
        !status_.computed_sha256.length()) {
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
    params += status_.computed_sha256.c_str();
    params += "\",\"authentication\":\"";
    params += tag.c_str();
    params += "\"}";
    status_.phase = ResmedOtaPhase::Applying;
    status_.apply_mode = "authenticated";
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

bool ResmedOtaManager::begin_staged_upload(size_t input_size,
                                           const String &filename,
                                           const String &magic_hex) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (!arbiter_) {
        set_error("not_initialized");
        return false;
    }
    if (!guard_device_idle_for_upgrade()) return false;
    if (active() || transport_active()) {
        set_error("session_active");
        return false;
    }
    if (input_size == 0 || input_size > AC_RESMED_OTA_MAX_FILE_BYTES) {
        set_error("bad_size");
        return false;
    }
    if (!storage_write_port_) {
        set_error("storage_not_initialized");
        return false;
    }

    clear_session();
    if (!configure_staged_input(input_size, filename, magic_hex)) return false;

    staging_buffer_ = LargeByteBuffer::allocate(staging_output_size_);
    if (!staging_buffer_) {
        set_error("stage_alloc_failed");
        return false;
    }
    if (!staging_passthrough_abc_ && !write_staging_header()) {
        set_error("stage_header_failed");
        return false;
    }

    staging_generation_++;
    if (staging_generation_ == 0) staging_generation_++;

    status_.phase = ResmedOtaPhase::Staging;
    status_.total_size = input_size;
    status_.uploaded_bytes = 0;
    status_.filename = filename;
    status_.staged_path = AC_RESMED_OTA_STAGED_PATH;
    status_.last_result = "staging";
    update_progress();
    last_activity_ms_ = millis();

    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] staging input=%s target=%s size=%u file=%s\n",
              status_.input_type.c_str(), status_.target.c_str(),
              static_cast<unsigned>(input_size), filename.c_str());
    return true;
}

bool ResmedOtaManager::write_staged_upload(size_t offset,
                                           const uint8_t *data,
                                           size_t len) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (status_.phase != ResmedOtaPhase::Staging || !staging_buffer_) {
        return false;
    }
    if (!data && len) return false;
    if (offset != staging_input_written_ ||
        offset > staging_input_size_ ||
        len > staging_input_size_ - offset) {
        set_error("stage_offset_mismatch");
        return false;
    }
    if (len == 0) return true;

    const bool ok = staging_passthrough_abc_
                        ? write_staging_bytes(offset, data, len)
                        : write_raw_payload_slice(offset, data, len);
    if (!ok) {
        set_error("stage_write_failed");
        return false;
    }
    staging_input_written_ += len;
    status_.uploaded_bytes = staging_input_written_;
    update_progress();
    last_activity_ms_ = millis();
    return true;
}

bool ResmedOtaManager::finish_staged_upload() {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (status_.phase != ResmedOtaPhase::Staging || !staging_buffer_) {
        set_error("stage_not_active");
        return false;
    }
    if (staging_input_written_ != staging_input_size_) {
        set_error("stage_incomplete");
        return false;
    }
    if (!staging_passthrough_abc_ && !finalize_raw_abc()) {
        set_error("stage_finalize_failed");
        return false;
    }

    staged_image_ = LargeByteBuffer::freeze(std::move(staging_buffer_));
    if (!staged_image_) {
        set_error("stage_freeze_failed");
        return false;
    }

    status_.phase = ResmedOtaPhase::Publishing;
    status_.total_size = staging_output_size_;
    status_.uploaded_bytes = 0;
    status_.progress_percent = 0;
    status_.staged_path = AC_RESMED_OTA_STAGED_PATH;
    status_.last_result = "publishing";
    last_activity_ms_ = millis();

    return request_staged_publication();
}

bool ResmedOtaManager::request_staged_publication() {
    if (staging_publish_ticket_.valid()) return true;
    if (!storage_write_port_ || !staged_image_) {
        set_error("stage_publish_state_invalid");
        return false;
    }

    StorageAtomicWriteCommand command;
    command.path = AC_RESMED_OTA_STAGED_PATH;
    command.bytes = staged_image_;
    command.free_reserve_bytes = AC_RESMED_OTA_STORAGE_MARGIN_BYTES;
    command.lane = StorageAtomicWriteLane::Foreground;
    command.generation = staging_generation_;

    const OperationSubmission submission =
        storage_write_port_->request_write(command);
    if (submission.accepted()) {
        staging_publish_ticket_ = submission.ticket;
        staging_publish_retry_at_ms_ = 0;
        return true;
    }
    if (submission.admission == OperationAdmission::Busy) {
        staging_publish_retry_at_ms_ = millis() + STORAGE_PUBLISH_RETRY_MS;
        return true;
    }

    set_error("stage_publish_rejected");
    return false;
}

void ResmedOtaManager::poll_staged_publication() {
    if (status_.phase != ResmedOtaPhase::Publishing) {
        if (storage_write_port_ && staging_publish_ticket_.valid() &&
            storage_write_port_->abandon(staging_publish_ticket_)) {
            staging_publish_ticket_ = {};
        }
        return;
    }

    if (!staging_publish_ticket_.valid()) {
        if (staging_publish_retry_at_ms_ != 0 &&
            static_cast<int32_t>(millis() - staging_publish_retry_at_ms_) < 0) {
            return;
        }

        (void)request_staged_publication();
        return;
    }

    StorageAtomicWriteCompletion completion;
    if (!storage_write_port_->take_completion(staging_publish_ticket_,
                                              completion)) {
        return;
    }
    staging_publish_ticket_ = {};

    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        set_error(completion.error[0] ? completion.error
                                      : "stage_publish_failed");
        return;
    }

    status_.phase = ResmedOtaPhase::Staged;
    status_.last_result = "staged";
    last_activity_ms_ = millis();

    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] staged %s size=%u path=%s\n",
              status_.target.c_str(),
              static_cast<unsigned>(staging_output_size_),
              status_.staged_path.c_str());

    (void)start_staged_transfer();
}

bool ResmedOtaManager::start_staged_transfer() {
    if (status_.phase != ResmedOtaPhase::Staged || !staged_image_) {
        set_error("not_staged");
        return false;
    }

    const String filename = status_.filename;
    const String input_type = status_.input_type;
    const String target = status_.target;
    const std::shared_ptr<const LargeByteBuffer> image = staged_image_;

    status_.phase = ResmedOtaPhase::Idle;
    if (!begin_upload(image->size(), "", filename)) return false;

    status_.input_type = input_type;
    status_.target = target;
    status_.staged_path = AC_RESMED_OTA_STAGED_PATH;
    staged_image_ = image;
    staged_transfer_active_ = true;
    staged_check_requested_ = false;
    staged_auto_apply_ = true;
    return true;
}

void ResmedOtaManager::abort(const char *reason) {
    ScopedLock lock(*this, 1000);
    if (!lock) return;
    const char *why = reason ? reason : "aborted";
    if (arbiter_) {
        arbiter_->cancel_requests_from_source(RpcSource::ResmedOta, why);
    }
    set_error(why);
}

bool ResmedOtaManager::active() const {
    ScopedLock lock(*this, 50);
    if (!lock) return true;
    return staging_publish_ticket_.valid() ||
           status_.phase == ResmedOtaPhase::Staging ||
           status_.phase == ResmedOtaPhase::Publishing ||
           status_.phase == ResmedOtaPhase::Staged ||
           status_.phase == ResmedOtaPhase::Initiating ||
           status_.phase == ResmedOtaPhase::Ready ||
           status_.phase == ResmedOtaPhase::Uploading ||
           status_.phase == ResmedOtaPhase::Uploaded ||
           status_.phase == ResmedOtaPhase::Checking ||
           status_.phase == ResmedOtaPhase::Verified ||
           status_.phase == ResmedOtaPhase::Applying;
}

bool ResmedOtaManager::transport_active() const {
    ScopedLock lock(*this, 50);
    if (!lock) return true;
    return waiting_for_ != WaitingFor::None ||
           staging_publish_ticket_.valid() ||
           status_.phase == ResmedOtaPhase::Staging ||
           status_.phase == ResmedOtaPhase::Publishing ||
           status_.phase == ResmedOtaPhase::Initiating ||
           status_.phase == ResmedOtaPhase::Ready ||
           status_.phase == ResmedOtaPhase::Uploading ||
           status_.phase == ResmedOtaPhase::Uploaded ||
           status_.phase == ResmedOtaPhase::Checking ||
           status_.phase == ResmedOtaPhase::Applying;
}

ResmedOtaStatus ResmedOtaManager::status() const {
    ScopedLock lock(*this, 50);
    if (!lock) return ResmedOtaStatus{};
    return status_;
}

const char *ResmedOtaManager::phase_name() const {
    ScopedLock lock(*this, 50);
    if (!lock) return "busy";
    switch (status_.phase) {
        case ResmedOtaPhase::Idle: return "idle";
        case ResmedOtaPhase::Staging: return "staging";
        case ResmedOtaPhase::Publishing: return "publishing";
        case ResmedOtaPhase::Staged: return "staged";
        case ResmedOtaPhase::Initiating: return "initiating";
        case ResmedOtaPhase::Ready: return "ready";
        case ResmedOtaPhase::Uploading: return "uploading";
        case ResmedOtaPhase::Uploaded: return "uploaded";
        case ResmedOtaPhase::Checking: return "checking";
        case ResmedOtaPhase::Verified: return "verified";
        case ResmedOtaPhase::Applying: return "applying";
        case ResmedOtaPhase::Complete: return "complete";
        case ResmedOtaPhase::Error: return "error";
        default: return "unknown";
    }
}

bool ResmedOtaManager::queue_request(const char *method,
                                     const std::string &params,
                                     uint32_t timeout_ms) {
    if (!arbiter_ || !method) return false;
    waiting_for_ = WaitingFor::None;
    if (!strcmp(method, "InitiateUpgrade")) {
        waiting_for_ = WaitingFor::Initiate;
    } else if (!strcmp(method, "UpgradeDataBlock")) {
        waiting_for_ = WaitingFor::Block;
    } else if (!strcmp(method, "CheckUpgradeFile")) {
        waiting_for_ = WaitingFor::Check;
    } else if (!strcmp(method, "ApplyUpgrade") ||
               !strcmp(method, "ApplyAuthenticatedUpgrade")) {
        waiting_for_ = WaitingFor::Apply;
    }
    status_.waiting = waiting_for_ != WaitingFor::None;
    const bool ok = arbiter_->send_request(method, params, RpcSource::ResmedOta,
                                           timeout_ms);
    if (!ok) {
        waiting_for_ = WaitingFor::None;
        status_.waiting = false;
    }
    return ok;
}

void ResmedOtaManager::handle_event(const RpcEvent &event) {
    if (event.kind != RpcEventKind::RpcResponse) {
        set_error(event.payload_c_str());
        return;
    }
    handle_response(event.payload_text());
}

void ResmedOtaManager::handle_response(const std::string &payload) {
    if (waiting_for_ == WaitingFor::None) return;
    status_.last_result = payload.c_str();
    status_.waiting = false;
    last_activity_ms_ = millis();

    if (json_member_present(payload, "error")) {
        WaitingFor failed = waiting_for_;
        waiting_for_ = WaitingFor::None;
        (void)failed;
        set_error("rpc_error");
        return;
    }

    WaitingFor completed = waiting_for_;
    waiting_for_ = WaitingFor::None;
    switch (completed) {
        case WaitingFor::Initiate: {
            uint32_t block = AC_RESMED_OTA_MAX_BLOCK_BYTES;
            json_extract_uint_member(payload, "xferBlockSize", block);
            if (block == 0 || block > AC_RESMED_OTA_MAX_BLOCK_BYTES) {
                set_error("bad_xfer_block_size");
                return;
            }
            status_.xfer_block_size = block;
            status_.phase = ResmedOtaPhase::Ready;
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
            status_.phase = ResmedOtaPhase::Verified;
            if (staged_auto_apply_) {
                Log::logf(CAT_OTA, LOG_WARN,
                          "[RESMED] verified; auto-applying upgrade\n");
                request_apply_plain(false, AC_RESMED_OTA_CONFIRM);
            }
            break;
        case WaitingFor::Apply:
            status_.phase = ResmedOtaPhase::Complete;
            break;
        case WaitingFor::None:
        default:
            break;
    }
}

void ResmedOtaManager::finish_pending_block() {
    if (!sha_started_ || sha_finished_ ||
        !update_sha_from_hex(sha_ctx_, pending_block_hex_)) {
        set_error("sha_update_failed");
        return;
    }

    status_.uploaded_bytes = pending_block_offset_ + pending_block_bytes_;
    pending_block_hex_ = "";
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    update_progress();
    status_.phase = status_.uploaded_bytes >= status_.total_size
                        ? ResmedOtaPhase::Uploaded
                        : ResmedOtaPhase::Ready;
}

void ResmedOtaManager::pump_staged_upload() {
    if (!staged_transfer_active_) return;
    if (waiting_for_ != WaitingFor::None) return;

    if (status_.phase == ResmedOtaPhase::Verified ||
        status_.phase == ResmedOtaPhase::Complete ||
        status_.phase == ResmedOtaPhase::Error) {
        staged_image_.reset();
        staged_transfer_active_ = false;
        return;
    }

    if (status_.phase == ResmedOtaPhase::Uploaded) {
        if (!staged_check_requested_) {
            staged_check_requested_ = true;
            staged_image_.reset();
            request_check();
        }
        return;
    }

    if (status_.phase != ResmedOtaPhase::Ready &&
        status_.phase != ResmedOtaPhase::Uploading) {
        return;
    }
    if (!staged_image_) {
        set_error("stage_buffer_missing");
        return;
    }

    const size_t offset = status_.uploaded_bytes;
    const size_t remaining =
        status_.total_size > offset ? status_.total_size - offset : 0;
    if (remaining == 0) return;

    const size_t want = std::min(status_.xfer_block_size, remaining);
    if (offset > staged_image_->size() ||
        want > staged_image_->size() - offset) {
        set_error("stage_buffer_range_invalid");
        return;
    }
    const uint8_t *bytes = staged_image_->data() + offset;

    static const char *digits = "0123456789ABCDEF";
    String hex;
    hex.reserve(want * 2);
    for (size_t i = 0; i < want; ++i) {
        hex += digits[(bytes[i] >> 4) & 0x0F];
        hex += digits[bytes[i] & 0x0F];
    }
    submit_block(offset, hex);
}

bool ResmedOtaManager::finish_hash() {
    if (!sha_started_) {
        set_error("sha_not_started");
        return false;
    }
    if (sha_finished_) return true;
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha_ctx_, hash);
    status_.computed_sha256 = sha_to_hex(hash);
    sha_finished_ = true;
    return true;
}

void ResmedOtaManager::clear_session() {
    release_staged_resources();
    pending_block_hex_ = "";
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    waiting_for_ = WaitingFor::None;
    status_ = {};
    status_.phase = ResmedOtaPhase::Idle;
    status_.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    sha_started_ = false;
    sha_finished_ = false;
    last_activity_ms_ = 0;
    staging_passthrough_abc_ = false;
    staging_input_size_ = 0;
    staging_input_written_ = 0;
    staging_output_size_ = 0;
    staging_payload_size_ = 0;
    staging_payload_written_ = 0;
    staging_source_offset_ = 0;
    staging_flash_start_ = 0;
    staging_rest_crc_ = crc32_ieee_initial_state();
    staging_desc2_ = 0;
    staging_desc3_ = 0;
    staging_target_code_[0] = 0;
    staging_descriptor_version_[0] = 0;
}

void ResmedOtaManager::release_staged_resources() {
    if (storage_write_port_ && staging_publish_ticket_.valid()) {
        if (storage_write_port_->abandon(staging_publish_ticket_)) {
            staging_publish_ticket_ = {};
        }
    }

    staging_publish_retry_at_ms_ = 0;
    staging_buffer_.reset();
    staged_image_.reset();
    staged_transfer_active_ = false;
    staged_check_requested_ = false;
    staged_auto_apply_ = false;
}

void ResmedOtaManager::set_error(const char *error) {
    release_staged_resources();
    status_.phase = ResmedOtaPhase::Error;
    status_.waiting = false;
    status_.last_error = error ? error : "error";
    waiting_for_ = WaitingFor::None;
    pending_block_hex_ = "";
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    last_activity_ms_ = millis();
    Log::logf(CAT_OTA, LOG_ERROR, "[RESMED] %s\n",
              status_.last_error.c_str());
}

void ResmedOtaManager::set_error(const String &error) {
    set_error(error.c_str());
}

void ResmedOtaManager::update_progress() {
    if (status_.total_size == 0) {
        status_.progress_percent = 0;
        return;
    }
    status_.progress_percent =
        static_cast<uint8_t>((status_.uploaded_bytes * 100ULL) /
                             status_.total_size);
}

bool ResmedOtaManager::configure_staged_input(size_t input_size,
                                              const String &filename,
                                              const String &magic_hex) {
    staging_input_size_ = input_size;
    staging_input_written_ = 0;
    staging_output_size_ = input_size;
    staging_payload_size_ = 0;
    staging_payload_written_ = 0;
    staging_source_offset_ = 0;
    staging_rest_crc_ = crc32_ieee_initial_state();
    staging_passthrough_abc_ =
        starts_with_abc_magic(magic_hex) || filename_ends_with_abc(filename);

    if (staging_passthrough_abc_) {
        status_.input_type = "abc";
        status_.target = "ABC";
        return true;
    }

    RawTargetSpec target;
    if (!infer_raw_target(input_size, target)) {
        set_error("unsupported_raw_image_size");
        return false;
    }

    uint32_t preset_desc2 = 0;
    uint32_t preset_desc3 = 0;
    bool have_preset = false;
    if (extract_descriptor_version(
            device_->state().software_identifier(),
            staging_descriptor_version_,
            sizeof(staging_descriptor_version_))) {
        have_preset =
            descriptor_preset_for_version(staging_descriptor_version_,
                                          preset_desc2, preset_desc3);
    }
    if (!have_preset &&
        extract_descriptor_version(std::string(filename.c_str()),
                                   staging_descriptor_version_,
                                   sizeof(staging_descriptor_version_))) {
        have_preset =
            descriptor_preset_for_version(staging_descriptor_version_,
                                          preset_desc2, preset_desc3);
    }
    if (!have_preset) {
        set_error("unsupported_descriptor_preset");
        return false;
    }
    staging_desc2_ = target.desc2_required ? preset_desc2 : 0;
    staging_desc3_ = target.desc3_required ? preset_desc3 : 0;
    if ((target.desc2_required && staging_desc2_ == 0) ||
        (target.desc3_required && staging_desc3_ == 0)) {
        set_error("missing_descriptor_word");
        return false;
    }

    snprintf(staging_target_code_, sizeof(staging_target_code_), "%s",
             target.code);
    staging_payload_size_ = target.payload_size;
    staging_source_offset_ = target.source_offset;
    staging_flash_start_ = target.flash_start;
    staging_output_size_ = ABC_PAYLOAD_OFFSET_0005 +
                           ABC_SEGMENT_ENTRY_SIZE + target.payload_size;
    status_.input_type = "raw";
    status_.target = target.code;
    return true;
}

bool ResmedOtaManager::write_staging_header() {
    uint8_t primary[ABC_PRIMARY_SIZE] = {};
    memcpy(primary + 0x00, "OTA!", 4);
    memcpy(primary + 0x04, "0005", 4);
    memcpy(primary + 0x48, ABC_COMPONENT_0005,
           strlen(ABC_COMPONENT_0005));

    uint8_t descriptor[ABC_DESCRIPTOR_SIZE] = {};
    uint8_t segment[ABC_SEGMENT_ENTRY_SIZE] = {};
    put_le32(segment, 0, static_cast<uint32_t>(staging_payload_size_));
    put_le32(segment, 4, staging_flash_start_);

    if (!write_staging_bytes(0, primary, sizeof(primary))) {
        return false;
    }
    if (!write_staging_bytes(ABC_PRIMARY_SIZE, descriptor,
                             sizeof(descriptor))) {
        return false;
    }
    if (!write_staging_bytes(ABC_PAYLOAD_OFFSET_0005, segment,
                             sizeof(segment))) {
        return false;
    }

    staging_rest_crc_ =
        crc32_ieee_update_state(staging_rest_crc_, segment, sizeof(segment));
    return true;
}

bool ResmedOtaManager::write_staging_bytes(size_t output_offset,
                                           const uint8_t *data,
                                           size_t len) {
    if (!len) return true;
    if (!staging_buffer_ || !data || output_offset > staging_buffer_->size() ||
        len > staging_buffer_->size() - output_offset) {
        return false;
    }

    memcpy(staging_buffer_->data() + output_offset, data, len);
    return true;
}

bool ResmedOtaManager::write_raw_payload_slice(size_t input_offset,
                                               const uint8_t *data,
                                               size_t len) {
    if (!len) return true;
    const size_t source_start = staging_source_offset_;
    const size_t source_end = staging_source_offset_ + staging_payload_size_;
    const size_t chunk_start = input_offset;
    const size_t chunk_end = input_offset + len;
    if (chunk_end <= source_start || chunk_start >= source_end) return true;

    const size_t copy_start = std::max(chunk_start, source_start);
    const size_t copy_end = std::min(chunk_end, source_end);
    const size_t data_offset = copy_start - chunk_start;
    const size_t copy_len = copy_end - copy_start;
    const uint8_t *payload = data + data_offset;
    const size_t output_offset = ABC_PAYLOAD_OFFSET_0005 +
                                 ABC_SEGMENT_ENTRY_SIZE +
                                 (copy_start - source_start);

    if (!write_staging_bytes(output_offset, payload, copy_len)) return false;

    staging_rest_crc_ =
        crc32_ieee_update_state(staging_rest_crc_, payload, copy_len);
    staging_payload_written_ += copy_len;
    return true;
}

bool ResmedOtaManager::finalize_raw_abc() {
    if (staging_payload_written_ != staging_payload_size_) return false;

    uint8_t primary[ABC_PRIMARY_SIZE] = {};
    memcpy(primary + 0x00, "OTA!", 4);
    memcpy(primary + 0x04, "0005", 4);
    memcpy(primary + 0x48, ABC_COMPONENT_0005,
           strlen(ABC_COMPONENT_0005));

    uint8_t descriptor[ABC_DESCRIPTOR_SIZE] = {};
    put_le32(descriptor, 0x00, 1);
    memcpy(descriptor + 0x04, staging_target_code_, 4);
    put_le32(descriptor, 0x08, staging_desc2_);
    put_le32(descriptor, 0x0C, staging_desc3_);
    put_le32(descriptor, 0x10, 0);
    put_le32(descriptor, 0x40,
             static_cast<uint32_t>(ABC_SEGMENT_ENTRY_SIZE +
                                   staging_payload_size_));
    put_le32(descriptor, 0x44, crc32_ieee_finish_state(staging_rest_crc_));
    put_le32(descriptor, 0x48, 1);

    uint8_t crc_input[ABC_PRIMARY_SIZE + 0x4C] = {};
    memcpy(crc_input, primary, sizeof(primary));
    memcpy(crc_input + sizeof(primary), descriptor, 0x4C);
    put_le32(descriptor, 0x4C,
             crc32_ieee(crc_input, sizeof(crc_input)));

    return write_staging_bytes(ABC_PRIMARY_SIZE, descriptor,
                               sizeof(descriptor));
}

bool ResmedOtaManager::guard_device_idle_for_upgrade() {
    const char *reason = nullptr;
    if (device_idle_for_upgrade(&reason)) return true;
    set_error(reason);
    return false;
}

bool ResmedOtaManager::device_idle_for_upgrade(const char **reason) const {
    if (reason) *reason = "therapy_state_unknown";
    if (!arbiter_ || !device_) return false;

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
        default:
            device_->request_healthcheck(
                *arbiter_, RpcSource::ResmedOta, millis());
            if (reason) {
                *reason = "therapy_state_refreshing";
            }
            return false;
    }
}

bool ResmedOtaManager::lock(uint32_t timeout_ms) const {
    if (!mutex_) return true;
    return xSemaphoreTakeRecursive(
               mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ResmedOtaManager::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
