#include "resmed_ota_manager.h"

#include <algorithm>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "as11_device_service.h"
#include "as11_rpc.h"
#include "debug_log.h"
#include "storage_path_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

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

bool json_result_true(const std::string &json) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, json);
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

}  // namespace

ResmedOtaManager::ScopedLock::ScopedLock(
    const ResmedOtaManager &manager,
    uint32_t timeout_ms)
    : manager_(manager) {
    locked_ = manager_.lock(timeout_ms);
}

ResmedOtaManager::ScopedLock::~ScopedLock() {
    if (locked_) manager_.unlock();
}

void ResmedOtaManager::begin(RpcRequestPort &rpc,
                             As11DeviceService &device,
                             StorageStreamPort &stream_port,
                             StoragePathPort &path_port) {
    rpc_ = &rpc;
    device_ = &device;
    stream_port_ = &stream_port;
    path_port_ = &path_port;
    if (!mutex_) mutex_ = xSemaphoreCreateRecursiveMutex();
    mbedtls_sha256_init(&sha_ctx_);
}

void ResmedOtaManager::poll() {
    ScopedLock lock(*this, 0);
    if (!lock || !rpc_) return;

    poll_rpc_completion();
    poll_prepared_transfer();
    poll_cleanup();

    const bool idle_timeout_phase =
        status_.phase == ResmedOtaPhase::Opening ||
        status_.phase == ResmedOtaPhase::Ready ||
        status_.phase == ResmedOtaPhase::Uploaded;
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
    if (!rpc_) {
        set_error("not_initialized");
        return false;
    }
    if (!guard_device_idle_for_upgrade()) return false;
    if (active() || transport_active()) {
        set_error("session_active");
        return false;
    }

    clear_session();
    return begin_protocol(total_size, expected_sha256, filename);
}

bool ResmedOtaManager::begin_prepared_upload(
    const ResmedPreparedFirmware &firmware) {
    ScopedLock lock(*this, 1000);
    if (!lock) return false;
    if (!rpc_ || !stream_port_ || !path_port_) {
        set_error("not_initialized");
        return false;
    }
    if (!firmware.valid() ||
        firmware.image.prepared_size > AC_RESMED_OTA_MAX_FILE_BYTES) {
        set_error("invalid_prepared_image");
        return false;
    }
    if (active() || transport_active()) {
        return false;
    }

    clear_session();
    prepared_ = firmware;
    if (!guard_device_idle_for_upgrade()) return false;

    prepared_transfer_ = true;
    prepared_check_requested_ = false;
    status_.phase = ResmedOtaPhase::Opening;
    status_.total_size = static_cast<size_t>(firmware.image.prepared_size);
    status_.filename = firmware.filename;
    status_.input_type =
        resmed_firmware_image_kind_name(firmware.image.kind);
    status_.target = firmware.image.target;
    status_.source_path = firmware.path;
    status_.last_result = "opening";
    last_activity_ms_ = millis();

    StorageStreamCommand command;
    command.path = firmware.path;
    command.lane = StorageStreamLane::Foreground;
    command.expected_size = firmware.image.prepared_size;
    command.verification = StorageStreamVerification::Size;

    char error[AC_STORAGE_ERROR_MAX] = {};
    if (!stream_port_->request_stream(command, prepared_stream_, error,
                                      sizeof(error))) {
        set_error(error[0] ? error : "prepared_stream_rejected");
        return false;
    }

    Log::logf(CAT_OTA, LOG_INFO,
              "[RESMED] prepared upload opening target=%s size=%u path=%s\n",
              firmware.image.target,
              static_cast<unsigned>(firmware.image.prepared_size),
              firmware.path);
    return true;
}

bool ResmedOtaManager::discard_prepared_firmware(
    const ResmedPreparedFirmware &firmware) {
    ScopedLock lock(*this, 1000);
    if (!lock || !firmware.valid() || active() || transport_active()) {
        return false;
    }

    clear_session();
    prepared_ = firmware;
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

    status_.phase = ResmedOtaPhase::Initiating;
    status_.total_size = total_size;
    status_.uploaded_bytes = 0;
    status_.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    status_.progress_percent = 0;
    status_.filename = filename;
    status_.expected_sha256 = expected;
    status_.computed_sha256 = "";
    status_.last_error = "";
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
    if (!lock || !rpc_) return false;
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
    const size_t raw_length = hex.length() / 2;
    if (raw_length == 0 || offset + raw_length > status_.total_size) {
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

    copy_cstr(pending_block_hex_, sizeof(pending_block_hex_),
              hex.c_str());
    pending_block_offset_ = offset;
    pending_block_bytes_ = raw_length;
    status_.phase = ResmedOtaPhase::Uploading;
    last_activity_ms_ = millis();
    if (!queue_request("UpgradeDataBlock", params,
                       AC_RESMED_OTA_BLOCK_TIMEOUT_MS)) {
        pending_block_hex_[0] = '\0';
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

void ResmedOtaManager::abort(const char *reason) {
    ScopedLock lock(*this, 1000);
    if (!lock) return;

    cancel_rpc_request();
    set_error(reason ? reason : "aborted");
}

bool ResmedOtaManager::active() const {
    ScopedLock lock(*this, 50);
    if (!lock) return true;

    return cleanup_count_ != 0 ||
           status_.phase == ResmedOtaPhase::Opening ||
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

    return waiting_for_ != WaitingFor::None || cleanup_count_ != 0 ||
           status_.phase == ResmedOtaPhase::Opening ||
           status_.phase == ResmedOtaPhase::Initiating ||
           status_.phase == ResmedOtaPhase::Ready ||
           status_.phase == ResmedOtaPhase::Uploading ||
           status_.phase == ResmedOtaPhase::Uploaded ||
           status_.phase == ResmedOtaPhase::Checking ||
           status_.phase == ResmedOtaPhase::Applying;
}

ResmedOtaStatus ResmedOtaManager::status() const {
    ScopedLock lock(*this, 50);
    return lock ? status_ : ResmedOtaStatus{};
}

const char *ResmedOtaManager::phase_name() const {
    ScopedLock lock(*this, 50);
    if (!lock) return "busy";

    switch (status_.phase) {
        case ResmedOtaPhase::Idle: return "idle";
        case ResmedOtaPhase::Opening: return "opening";
        case ResmedOtaPhase::Initiating: return "initiating";
        case ResmedOtaPhase::Ready: return "ready";
        case ResmedOtaPhase::Uploading: return "uploading";
        case ResmedOtaPhase::Uploaded: return "uploaded";
        case ResmedOtaPhase::Checking: return "checking";
        case ResmedOtaPhase::Verified: return "verified";
        case ResmedOtaPhase::Applying: return "applying";
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
    status_.waiting = true;
    return true;
}

void ResmedOtaManager::poll_rpc_completion() {
    if (!rpc_ticket_.valid()) return;

    RpcRequestCompletion completion;
    if (!rpc_->take_completion(rpc_ticket_, completion)) return;

    rpc_ticket_ = {};
    if (completion.cause == RpcCompletionCause::Response) {
        handle_response(completion.payload);
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

void ResmedOtaManager::handle_response(const std::string &payload) {
    if (waiting_for_ == WaitingFor::None) return;

    status_.last_result = payload.c_str();
    status_.waiting = false;
    last_activity_ms_ = millis();
    if (json_member_present(payload, "error")) {
        waiting_for_ = WaitingFor::None;
        set_error("rpc_error");
        return;
    }

    const WaitingFor completed = waiting_for_;
    waiting_for_ = WaitingFor::None;
    switch (completed) {
        case WaitingFor::Initiate: {
            uint32_t block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
            json_extract_uint_member(payload, "xferBlockSize", block_size);
            if (block_size == 0 ||
                block_size > AC_RESMED_OTA_MAX_BLOCK_BYTES) {
                set_error("bad_xfer_block_size");
                return;
            }
            status_.xfer_block_size = block_size;
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
            schedule_prepared_cleanup();
            break;

        case WaitingFor::Apply:
            status_.phase = ResmedOtaPhase::Complete;
            break;

        case WaitingFor::None:
            break;
    }
}

void ResmedOtaManager::poll_prepared_transfer() {
    if (!prepared_transfer_) return;

    if (status_.phase == ResmedOtaPhase::Opening) {
        (void)open_prepared_stream();
        return;
    }
    if (waiting_for_ != WaitingFor::None) return;

    if (status_.phase == ResmedOtaPhase::Uploaded) {
        if (prepared_check_requested_) return;

        close_prepared_stream(true);
        prepared_check_requested_ = true;
        prepared_transfer_ = false;
        (void)request_check();
        return;
    }
    if (status_.phase == ResmedOtaPhase::Ready ||
        status_.phase == ResmedOtaPhase::Uploading) {
        (void)fill_prepared_block();
    }
}

bool ResmedOtaManager::open_prepared_stream() {
    if (!prepared_stream_ || !stream_port_) {
        set_error("prepared_stream_missing");
        return false;
    }

    StorageStreamStatus stream_status;
    if (!stream_port_->status(*prepared_stream_, stream_status)) {
        set_error("prepared_stream_status_failed");
        return false;
    }
    if (stream_status.state == StorageStreamState::Error ||
        stream_status.state == StorageStreamState::Cancelled) {
        set_error(stream_status.error[0]
                      ? stream_status.error
                      : "prepared_stream_failed");
        return false;
    }
    if (stream_status.state != StorageStreamState::Ready) return false;
    if (!stream_port_->attach(*prepared_stream_)) return false;

    return begin_protocol(static_cast<size_t>(prepared_.image.prepared_size),
                          "", prepared_.filename);
}

bool ResmedOtaManager::fill_prepared_block() {
    if (!prepared_stream_ || !stream_port_) {
        set_error("prepared_stream_missing");
        return false;
    }

    if (prepared_block_wanted_ == 0) {
        const size_t remaining = status_.total_size - status_.uploaded_bytes;
        if (remaining == 0) return true;
        prepared_block_wanted_ =
            std::min(status_.xfer_block_size, remaining);
        prepared_block_bytes_ = 0;
    }

    const size_t offset = status_.uploaded_bytes + prepared_block_bytes_;
    const StorageStreamRead read = stream_port_->read(
        *prepared_stream_, prepared_block_ + prepared_block_bytes_,
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

    const String hex = bytes_to_hex(prepared_block_, prepared_block_bytes_);
    return submit_block(status_.uploaded_bytes, hex);
}

void ResmedOtaManager::finish_pending_block() {
    if (!sha_started_ || sha_finished_ || pending_block_bytes_ == 0 ||
        !update_sha_from_hex(sha_ctx_, pending_block_hex_,
                             pending_block_bytes_)) {
        set_error("sha_update_failed");
        return;
    }

    status_.uploaded_bytes = pending_block_offset_ + pending_block_bytes_;
    pending_block_hex_[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    prepared_block_bytes_ = 0;
    prepared_block_wanted_ = 0;
    update_progress();
    status_.phase = status_.uploaded_bytes >= status_.total_size
                        ? ResmedOtaPhase::Uploaded
                        : ResmedOtaPhase::Ready;
}

void ResmedOtaManager::close_prepared_stream(bool complete) {
    if (stream_port_ && prepared_stream_) {
        stream_port_->finish(*prepared_stream_, complete);
    }
    prepared_stream_.reset();
    prepared_block_bytes_ = 0;
    prepared_block_wanted_ = 0;
}

void ResmedOtaManager::schedule_prepared_cleanup() {
    if (!prepared_.valid() || cleanup_count_ != 0) return;

    if (prepared_.cleanup_source) {
        copy_cstr(cleanup_paths_[cleanup_count_], AC_STORAGE_PATH_MAX,
                  prepared_.source_path);
        cleanup_count_++;
    }
    if (prepared_.cleanup_prepared &&
        (!prepared_.cleanup_source ||
         strcmp(prepared_.path, prepared_.source_path) != 0) &&
        cleanup_count_ < 2) {
        copy_cstr(cleanup_paths_[cleanup_count_], AC_STORAGE_PATH_MAX,
                  prepared_.path);
        cleanup_count_++;
    }
    prepared_ = {};
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
                      cleanup_paths_[cleanup_index_],
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
    command.source = cleanup_paths_[cleanup_index_];
    command.generation = cleanup_generation_;
    const OperationSubmission submission = path_port_->request(command);
    if (submission.accepted()) {
        cleanup_ticket_ = submission.ticket;
    } else if (submission.admission == OperationAdmission::Rejected) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "[RESMED] transient cleanup rejected path=%s\n",
                  cleanup_paths_[cleanup_index_]);
        cleanup_index_++;
    }
}

void ResmedOtaManager::clear_cleanup() {
    memset(cleanup_paths_, 0, sizeof(cleanup_paths_));
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
    status_.computed_sha256 = sha_to_hex(hash);
    sha_finished_ = true;
    return true;
}

void ResmedOtaManager::clear_session() {
    cancel_rpc_request();
    close_prepared_stream(false);
    schedule_prepared_cleanup();

    pending_block_hex_[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    waiting_for_ = WaitingFor::None;
    status_ = {};
    status_.phase = ResmedOtaPhase::Idle;
    status_.xfer_block_size = AC_RESMED_OTA_MAX_BLOCK_BYTES;
    sha_started_ = false;
    sha_finished_ = false;
    last_activity_ms_ = 0;
    prepared_ = {};
    prepared_transfer_ = false;
    prepared_check_requested_ = false;
}

void ResmedOtaManager::set_error(const char *error) {
    close_prepared_stream(false);
    schedule_prepared_cleanup();
    prepared_transfer_ = false;
    prepared_check_requested_ = false;

    status_.phase = ResmedOtaPhase::Error;
    status_.waiting = false;
    status_.last_error = error ? error : "error";
    waiting_for_ = WaitingFor::None;
    pending_block_hex_[0] = '\0';
    pending_block_offset_ = 0;
    pending_block_bytes_ = 0;
    last_activity_ms_ = millis();
    Log::logf(CAT_OTA, LOG_ERROR, "[RESMED] %s\n",
              status_.last_error.c_str());
}

void ResmedOtaManager::update_progress() {
    status_.progress_percent = status_.total_size == 0
        ? 0
        : static_cast<uint8_t>((status_.uploaded_bytes * 100ULL) /
                               status_.total_size);
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
    if (!mutex_) return true;
    return xSemaphoreTakeRecursive(
               mutex_, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void ResmedOtaManager::unlock() const {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

}  // namespace aircannect
