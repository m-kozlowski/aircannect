#include "resmed_ota_manager.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "as11_rpc.h"
#include "debug_log.h"

namespace aircannect {
namespace {

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

void ResmedOtaManager::begin(RpcArbiter &arbiter) {
    arbiter_ = &arbiter;
    mbedtls_sha256_init(&sha_ctx_);
}

void ResmedOtaManager::poll() {
    if (!arbiter_) return;
    RpcEvent event;
    while (arbiter_->next_resmed_ota_event(event)) {
        handle_event(event);
    }
    if ((status_.phase == ResmedOtaPhase::Ready ||
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
    if (!arbiter_) {
        set_error("not_initialized");
        return false;
    }
    if (transport_active()) {
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
    Log::logf(CAT_OTA, LOG_INFO, "[RESMED OTA] initiate size=%u file=%s\n",
              static_cast<unsigned>(total_size), filename.c_str());
    return true;
}

bool ResmedOtaManager::submit_block(size_t offset, const String &hex_data) {
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
    Log::logf(CAT_OTA, LOG_INFO, "[RESMED OTA] check sha256=%s\n",
              status_.computed_sha256.c_str());
    return true;
}

bool ResmedOtaManager::request_apply_plain(bool reset_settings,
                                           const String &confirm) {
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
    Log::logf(CAT_OTA, LOG_WARN, "[RESMED OTA] ApplyUpgrade queued\n");
    return true;
}

bool ResmedOtaManager::request_apply_authenticated(
    const String &authentication,
    const String &confirm) {
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
              "[RESMED OTA] ApplyAuthenticatedUpgrade queued\n");
    return true;
}

void ResmedOtaManager::abort(const char *reason) {
    set_error(reason ? reason : "aborted");
}

bool ResmedOtaManager::active() const {
    return status_.phase == ResmedOtaPhase::Initiating ||
           status_.phase == ResmedOtaPhase::Ready ||
           status_.phase == ResmedOtaPhase::Uploading ||
           status_.phase == ResmedOtaPhase::Uploaded ||
           status_.phase == ResmedOtaPhase::Checking ||
           status_.phase == ResmedOtaPhase::Verified ||
           status_.phase == ResmedOtaPhase::Applying;
}

bool ResmedOtaManager::transport_active() const {
    return waiting_for_ != WaitingFor::None ||
           status_.phase == ResmedOtaPhase::Initiating ||
           status_.phase == ResmedOtaPhase::Ready ||
           status_.phase == ResmedOtaPhase::Uploading ||
           status_.phase == ResmedOtaPhase::Uploaded ||
           status_.phase == ResmedOtaPhase::Checking ||
           status_.phase == ResmedOtaPhase::Applying;
}

const char *ResmedOtaManager::phase_name() const {
    switch (status_.phase) {
        case ResmedOtaPhase::Idle: return "idle";
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
        set_error(event.payload.c_str());
        return;
    }
    handle_response(event.payload);
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
}

void ResmedOtaManager::set_error(const char *error) {
    status_.phase = ResmedOtaPhase::Error;
    status_.waiting = false;
    status_.last_error = error ? error : "error";
    waiting_for_ = WaitingFor::None;
    pending_block_hex_ = "";
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    last_activity_ms_ = millis();
    Log::logf(CAT_OTA, LOG_ERROR, "[RESMED OTA] %s\n",
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

}  // namespace aircannect
