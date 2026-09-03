#include "ota_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <string.h>
#include <utility>

#include "arduino_ota_source.h"
#include "board.h"
#include "debug_log.h"
#include "firmware_installer.h"
#include "firmware_url_source.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "ota_status.h"
#include "resmed_firmware_preparer.h"
#include "resmed_ota_manager.h"
#include "update_checker.h"
#include "version.h"

namespace aircannect {
namespace {

bool required_size_arg(AsyncWebServerRequest *request,
                       const char *name,
                       size_t &out) {
    return http_size_arg(request, name, 0, AC_RESMED_OTA_MAX_FILE_BYTES, out) &&
           out > 0;
}

bool request_upload_args(AsyncWebServerRequest *request,
                         size_t &image_size,
                         OtaUploadEncoding &encoding,
                         size_t &wire_size) {
    image_size = 0;
    wire_size = 0;
    encoding = OtaUploadEncoding::Auto;

    if (!http_size_arg(request, "size", 0, AC_RESMED_OTA_MAX_FILE_BYTES,
                       image_size)) {
        return false;
    }

    if (request && request->hasArg("encoding")) {
        const String value = request->arg("encoding");
        if (!parse_ota_upload_encoding(value.c_str(), encoding)) return false;
    }

    if (request && request->hasArg("wire_size")) {
        if (!required_size_arg(request, "wire_size", wire_size)) return false;
    } else if (image_size) {
        wire_size = image_size;
    }

    if (!wire_size) return false;
    if (encoding == OtaUploadEncoding::Plain) {
        return image_size > 0 && image_size == wire_size;
    }
    return true;
}

bool request_url_args(AsyncWebServerRequest *request,
                      String &url,
                      size_t &image_size,
                      OtaUploadEncoding &encoding,
                      size_t &wire_size) {
    url = "";
    image_size = 0;
    wire_size = 0;
    encoding = OtaUploadEncoding::Auto;

    if (!request || !request->hasArg("url")) return false;
    url = request->arg("url");
    url.trim();
    if (!url.length() || url.length() > AC_OTA_URL_MAX_LENGTH) return false;

    if (request->hasArg("encoding")) {
        const String value = request->arg("encoding");
        if (!parse_ota_upload_encoding(value.c_str(), encoding)) return false;
    }

    if (!http_size_arg(request, "size", 0, AC_RESMED_OTA_MAX_FILE_BYTES,
                       image_size) ||
        !http_size_arg(request, "wire_size", 0,
                       AC_RESMED_OTA_MAX_FILE_BYTES, wire_size)) {
        return false;
    }

    if (encoding == OtaUploadEncoding::Plain) {
        return !image_size || !wire_size || image_size == wire_size;
    }
    return true;
}

bool ota_status_active(const OtaStatusSnapshot &status) {
    return status.arduino_active || status.http_prepare_pending ||
           status.http_prepared || status.http_active || status.http_ready ||
           status.url_active || status.update_check_pending ||
           status.update_check_active || status.reboot_pending;
}

bool same_ota_status(const OtaStatusSnapshot &lhs,
                     const OtaStatusSnapshot &rhs) {
    return lhs.arduino_started == rhs.arduino_started &&
           lhs.arduino_active == rhs.arduino_active &&
           lhs.http_prepare_pending == rhs.http_prepare_pending &&
           lhs.http_prepared == rhs.http_prepared &&
           lhs.http_active == rhs.http_active &&
           lhs.http_ready == rhs.http_ready &&
           lhs.url_active == rhs.url_active &&
           lhs.update_check_enabled == rhs.update_check_enabled &&
           lhs.update_check_pending == rhs.update_check_pending &&
           lhs.update_check_active == rhs.update_check_active &&
           lhs.update_check_attempted == rhs.update_check_attempted &&
           lhs.update_checked == rhs.update_checked &&
           lhs.update_available == rhs.update_available &&
           lhs.update_installable == rhs.update_installable &&
           lhs.reboot_pending == rhs.reboot_pending &&
           lhs.auth_enabled == rhs.auth_enabled &&
           lhs.arduino_port == rhs.arduino_port && lhs.bytes == rhs.bytes &&
           lhs.total_size == rhs.total_size &&
           lhs.wire_bytes == rhs.wire_bytes &&
           lhs.wire_total_size == rhs.wire_total_size &&
           lhs.progress_percent == rhs.progress_percent &&
           lhs.method == rhs.method && lhs.encoding == rhs.encoding &&
           lhs.partition == rhs.partition &&
           lhs.last_error == rhs.last_error &&
           lhs.update_version == rhs.update_version &&
           lhs.update_error == rhs.update_error;
}

template <typename JsonOut>
void build_ota_json(JsonOut &json, const OtaStatusSnapshot &ota) {
    json = "{";
    json_add_string(json, "version", aircannect_version(), false);
    json_add_string(json, "release_target", AC_OTA_RELEASE_TARGET);
    json += ",\"upload_encodings\":[\"auto\",\"plain\",\"zlib\"]";
    json_add_bool(json, "url_update", true);
    json_add_bool(json, "arduino_started", ota.arduino_started);
    json_add_bool(json, "arduino_active", ota.arduino_active);
    json_add_bool(json, "http_prepare_pending", ota.http_prepare_pending);
    json_add_bool(json, "http_prepared", ota.http_prepared);
    json_add_bool(json, "http_active", ota.http_active);
    json_add_bool(json, "http_ready", ota.http_ready);
    json_add_bool(json, "url_active", ota.url_active);
    json_add_bool(json, "update_check_enabled", ota.update_check_enabled);
    json_add_bool(json, "update_check_pending", ota.update_check_pending);
    json_add_bool(json, "update_check_active", ota.update_check_active);
    json_add_bool(json, "update_check_attempted", ota.update_check_attempted);
    json_add_bool(json, "update_checked", ota.update_checked);
    json_add_bool(json, "update_available", ota.update_available);
    json_add_bool(json, "update_installable", ota.update_installable);
    json_add_string(json, "update_version", ota.update_version.c_str());
    json_add_string(json, "update_error", ota.update_error.c_str());
    if (ota.update_check_attempted) {
        json_add_int(json, "update_last_check_age_ms",
                     static_cast<long>(ota.update_last_check_age_ms));
    } else {
        json += ",\"update_last_check_age_ms\":null";
    }
    json_add_bool(json, "reboot_pending", ota.reboot_pending);
    json_add_bool(json, "auth_enabled", ota.auth_enabled);
    json_add_int(json, "arduino_port", ota.arduino_port);
    json_add_int(json, "bytes", static_cast<long>(ota.bytes));
    json_add_int(json, "total_size", static_cast<long>(ota.total_size));
    json_add_int(json, "wire_bytes", static_cast<long>(ota.wire_bytes));
    json_add_int(json, "wire_total_size",
                 static_cast<long>(ota.wire_total_size));
    json_add_int(json, "progress", ota.progress_percent);
    json_add_string(json, "method", ota.method.c_str());
    json_add_string(json, "encoding", ota.encoding.c_str());
    json_add_string(json, "partition", ota.partition.c_str());
    json_add_string(json, "last_error", ota.last_error.c_str());
    json += '}';
}

template <typename JsonOut>
void build_resmed_ota_json(JsonOut &json,
                           const ResmedFirmwarePreparer &preparer,
                           const ResmedOtaManager &manager,
                           bool *active_out = nullptr) {
    const ResmedOtaStatus status = manager.status();
    const ResmedFirmwarePrepareStatus prepare = preparer.status();
    const bool active = manager.active() || preparer.active();
    if (active_out) *active_out = active;

    json = "{";
    json_add_string(json, "phase", resmed_ota_phase_name(status.phase), false);
    json_add_string(json, "operation",
                    resmed_ota_operation_name(status.operation));
    json_add_bool(json, "active", active);
    json_add_bool(json, "confirmation_required",
                  status.confirmation_required);
    json_add_bool(json, "recovery_available", status.recovery_available);
    json_add_bool(json, "can_available", status.can_available);
    json_add_string(json, "prepare_state",
                    resmed_firmware_prepare_state_name(prepare.state));
    json_add_bool(json, "prepare_active", prepare.active());
    json_add_int(json, "prepare_total_size",
                 static_cast<long>(prepare.total_bytes));
    json_add_int(json, "prepare_processed_bytes",
                 static_cast<long>(prepare.processed_bytes));
    json_add_int(json, "prepare_progress", prepare.progress_percent);
    json_add_string(json, "prepare_source_path", prepare.source_path);
    json_add_string(json, "prepare_filename", prepare.filename);
    json_add_string(json, "prepare_target", prepare.target);
    json_add_string(json, "prepare_error", prepare.error);
    json_add_bool(json, "waiting", status.waiting);
    json_add_int(json, "total_size", static_cast<long>(status.total_size));
    json_add_int(json, "uploaded_bytes",
                 static_cast<long>(status.uploaded_bytes));
    json_add_int(json, "xfer_block_size",
                 static_cast<long>(status.xfer_block_size));
    json_add_int(json, "progress", status.progress_percent);
    json_add_string(json, "filename", status.filename.c_str());
    json_add_string(json, "expected_sha256", status.expected_sha256.c_str());
    json_add_string(json, "computed_sha256", status.computed_sha256.c_str());
    json_add_string(json, "apply_mode", status.apply_mode.c_str());
    json_add_string(json, "input_type", status.input_type.c_str());
    json_add_string(json, "transport",
                    resmed_firmware_install_transport_name(status.transport));
    json_add_string(json, "target", status.target.c_str());
    json_add_string(json, "source_path", status.source_path.c_str());
    json_add_string(json, "output_path", status.output_path.c_str());
    json_add_string(json, "recovery_path", status.recovery_path.c_str());
    json_add_string(json, "last_result", status.last_result.c_str());
    json_add_string(json, "last_error", status.last_error.c_str());
    json += '}';
}

void send_esp_status(AsyncWebServerRequest *request,
                     const OtaStatusSnapshot &status,
                     int response_status) {
    String json;
    json.reserve(AC_WEB_OTA_JSON_RESERVE);
    build_ota_json(json, status);
    request->send(response_status, "application/json", json);
}

void send_resmed_status(AsyncWebServerRequest *request,
                        const ResmedFirmwarePreparer &preparer,
                        const ResmedOtaManager &manager,
                        int response_status) {
    String json;
    json.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
    build_resmed_ota_json(json, preparer, manager);
    request->send(response_status, "application/json", json);
}

}  // namespace

bool OtaHttpController::begin(FirmwareInstaller &installer,
                              FirmwareUrlSource &url_source,
                              ArduinoOtaSource &arduino_source,
                              UpdateChecker &update_checker,
                              ResmedFirmwarePreparer &resmed_preparer,
                              ResmedOtaManager &resmed_ota) {
    installer_ = &installer;
    url_source_ = &url_source;
    arduino_source_ = &arduino_source;
    update_checker_ = &update_checker;
    resmed_preparer_ = &resmed_preparer;
    resmed_ota_ = &resmed_ota;

    if (!snapshot_mutex_) {
        snapshot_mutex_ =
            xSemaphoreCreateMutexStatic(&snapshot_mutex_storage_);
    }
    if (!commands_.begin() || !snapshot_mutex_ ||
        !snapshot_json_.reserve(AC_WEB_OTA_JSON_RESERVE) ||
        !snapshot_build_json_.reserve(AC_WEB_OTA_JSON_RESERVE) ||
        !resmed_snapshot_json_.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE) ||
        !resmed_snapshot_build_json_.reserve(
            AC_WEB_RESMED_OTA_JSON_RESERVE)) {
        return false;
    }

    publish_snapshot_if_needed(true);
    publish_resmed_snapshot_if_needed(true);
    return snapshot_initialized_ && resmed_snapshot_initialized_;
}

void OtaHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/ota"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_esp_status(request,
                        collect_ota_status(*installer_, *url_source_,
                                           *arduino_source_, *update_checker_),
                        200);
    });

    server.on(AsyncURIMatcher::exact("/api/ota/check"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        const bool ok = update_checker_->request_check(installer_->active());
        request_snapshot();
        const OtaStatusSnapshot status = collect_ota_status(
            *installer_, *url_source_, *arduino_source_, *update_checker_);
        const int response_status =
            ok ? 202 : (status.update_error == "ota_busy" ? 409 : 400);
        send_esp_status(request, status, response_status);
    });

    server.on(AsyncURIMatcher::exact("/api/ota/install-update"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        if (resmed_ota_->transport_active()) {
            request->send(409, "application/json",
                          "{\"error\":\"resmed_ota_active\"}");
            return;
        }

        String error;
        const bool ok = request_selected_update(*update_checker_,
                                                *url_source_, error);
        request_snapshot();
        OtaStatusSnapshot status = collect_ota_status(
            *installer_, *url_source_, *arduino_source_, *update_checker_);
        if (!ok && error.length()) status.last_error = error;
        const int response_status =
            ok ? 202 : (status.last_error == "ota_busy" ? 409 : 400);
        send_esp_status(request, status, response_status);
    });

    server.on(AsyncURIMatcher::exact("/api/ota/url"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        if (resmed_ota_->transport_active()) {
            request->send(409, "application/json",
                          "{\"error\":\"resmed_ota_active\"}");
            return;
        }

        String url;
        size_t image_size = 0;
        size_t wire_size = 0;
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
        if (!request_url_args(request, url, image_size, encoding,
                              wire_size)) {
            request->send(400, "application/json",
                          "{\"error\":\"invalid_url_args\"}");
            return;
        }

        const bool ok = url_source_->request(url, encoding, image_size,
                                             wire_size);
        request_snapshot();
        const OtaStatusSnapshot status = collect_ota_status(
            *installer_, *url_source_, *arduino_source_, *update_checker_);
        const int response_status =
            ok ? 202 : (status.last_error == "ota_busy" ? 409 : 400);
        send_esp_status(request, status, response_status);
    });

    server.on(AsyncURIMatcher::exact("/api/ota/prepare"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        if (resmed_ota_->transport_active()) {
            request->send(409, "application/json",
                          "{\"error\":\"resmed_ota_active\"}");
            return;
        }

        size_t image_size = 0;
        size_t wire_size = 0;
        OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
        if (!request_upload_args(request, image_size, encoding, wire_size)) {
            request->send(400, "application/json",
                          "{\"error\":\"invalid_upload_args\"}");
            return;
        }

        const bool ok = installer_->request_prepare(
            image_size, encoding, wire_size,
            FirmwareInstallSource::HttpUpload);
        request_snapshot();
        const OtaStatusSnapshot status = collect_ota_status(
            *installer_, *url_source_, *arduino_source_, *update_checker_);
        const int response_status =
            ok ? 202 : (status.last_error == "ota_busy" ? 409 : 400);
        send_esp_status(request, status, response_status);
    });

    server.on(
        AsyncURIMatcher::exact("/api/ota/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (url_source_->active()) {
                request->send(409, "application/json",
                              "{\"error\":\"ota_busy\"}");
                return;
            }

            const bool ok = installer_->finish();
            request_snapshot();
            send_esp_status(
                request,
                collect_ota_status(*installer_, *url_source_,
                                   *arduino_source_, *update_checker_),
                ok ? 200 : 400);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t length, bool final) {
            if (index == 0) {
                if (url_source_->active()) {
                    if (request && request->client()) request->client()->close();
                    return;
                }

                size_t image_size = 0;
                size_t wire_size = 0;
                OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
                if (!request_upload_args(request, image_size, encoding,
                                         wire_size)) {
                    if (request && request->client()) request->client()->close();
                    return;
                }

                if (!installer_->begin_write(
                        filename, image_size, encoding, wire_size,
                        FirmwareInstallSource::HttpUpload)) {
                    return;
                }
            }

            if (!installer_->write(index, data, length)) {
                if (request && request->client()) request->client()->close();
                return;
            }

            (void)final;
        });

    server.on(AsyncURIMatcher::exact("/api/resmed-ota"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_resmed_status(request, *resmed_preparer_, *resmed_ota_, 200);
    });

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/install"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (resmed_ota_->active() || resmed_preparer_->active()) {
                request->send(
                    409, "application/json",
                    "{\"ok\":false,\"error\":\"resmed_ota_active\"}");
                return;
            }

            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            String path;
            String filename;
            String target_text;
            String transport_text;
            if (!json_variant_to_string(doc["path"], path)) {
                request->send(
                    400, "application/json",
                    "{\"ok\":false,\"error\":\"missing path\"}");
                return;
            }
            (void)json_variant_to_string(doc["filename"], filename);

            ResmedFirmwareTarget target =
                AC_RESMED_FIRMWARE_DEFAULT_TARGET;
            if (!doc["target"].isNull() &&
                (!json_variant_to_string(doc["target"], target_text) ||
                 !resmed_firmware_target_parse(target_text.c_str(), target))) {
                request->send(
                    400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid target\"}");
                return;
            }

            ResmedFirmwareInstallTransport transport =
                AC_RESMED_FIRMWARE_DEFAULT_TRANSPORT;
            if (!doc["transport"].isNull() &&
                (!json_variant_to_string(doc["transport"], transport_text) ||
                 !resmed_firmware_install_transport_parse(
                     transport_text.c_str(), transport))) {
                request->send(
                    400, "application/json",
                    "{\"ok\":false,\"error\":\"invalid transport\"}");
                return;
            }

            if (transport == ResmedFirmwareInstallTransport::Service &&
                !resmed_ota_->status().can_available) {
                request->send(
                    409, "application/json",
                    "{\"ok\":false,\"error\":\"can_transport_required\"}");
                return;
            }

            Command command;
            command.kind = CommandKind::ResmedInstall;
            command.path = path.c_str();
            command.filename = filename.c_str();
            command.flag = doc["transient"].is<bool>() &&
                           doc["transient"].as<bool>();
            command.resmed_target = target;
            command.resmed_transport = transport;
            send_queue_result(request, enqueue(std::move(command)));
        },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/resmed-ota/dump"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        if (resmed_ota_->active() || resmed_preparer_->active()) {
            request->send(
                409, "application/json",
                "{\"ok\":false,\"error\":\"resmed_ota_active\"}");
            return;
        }

        Command command;
        command.kind = CommandKind::ResmedDump;
        send_queue_result(request, enqueue(std::move(command)));
    });

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/dump/confirm"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            String confirm;
            if (!json_variant_to_string(doc["confirm"], confirm)) {
                request->send(
                    400, "application/json",
                    "{\"ok\":false,\"error\":\"missing confirm\"}");
                return;
            }

            Command command;
            command.kind = CommandKind::ResmedDumpConfirm;
            command.confirmation = confirm.c_str();
            send_queue_result(request, enqueue(std::move(command)));
        },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/init"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["size"].is<int>() || doc["size"].as<int>() <= 0) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing size\"}");
                return;
            }

            String sha256;
            String filename;
            (void)json_variant_to_string(doc["sha256"], sha256);
            (void)json_variant_to_string(doc["filename"], filename);

            Command command;
            command.kind = CommandKind::ResmedInit;
            command.number = static_cast<size_t>(doc["size"].as<int>());
            command.sha256 = sha256.c_str();
            command.filename = filename.c_str();
            send_queue_result(request, enqueue(std::move(command)));
        },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/block"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["offset"].is<int>() || doc["offset"].as<int>() < 0) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing offset\"}");
                return;
            }

            String data;
            if (!json_variant_to_string(doc["data"], data)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing data\"}");
                return;
            }

            Command command;
            command.kind = CommandKind::ResmedBlock;
            command.number = static_cast<size_t>(doc["offset"].as<int>());
            command.data = data.c_str();
            send_queue_result(request, enqueue(std::move(command)));
        },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/resmed-ota/check"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        Command command;
        command.kind = CommandKind::ResmedCheck;
        send_queue_result(request, enqueue(std::move(command)));
    });

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/apply"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!http_parse_json_body(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            String mode;
            String confirm;
            (void)json_variant_to_string(doc["mode"], mode);
            (void)json_variant_to_string(doc["confirm"], confirm);

            Command command;
            command.confirmation = confirm.c_str();
            if (mode == "plain") {
                command.kind = CommandKind::ResmedApplyPlain;
                command.flag = doc["reset"].is<bool>() &&
                               doc["reset"].as<bool>();
            } else if (mode == "authenticated") {
                String authentication;
                if (!json_variant_to_string(doc["authentication"],
                                            authentication)) {
                    request->send(
                        400, "application/json",
                        "{\"ok\":false,\"error\":\"missing authentication\"}");
                    return;
                }
                command.kind = CommandKind::ResmedApplyAuthenticated;
                command.authentication = authentication.c_str();
            } else {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"invalid mode\"}");
                return;
            }

            send_queue_result(request, enqueue(std::move(command)));
        },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/resmed-ota/abort"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        Command command;
        command.kind = CommandKind::ResmedAbort;
        send_queue_result(request, enqueue(std::move(command)));
    });
}

void OtaHttpController::poll() {
    if (!resmed_ota_) return;

    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;

        const bool publish_after =
            command.kind != CommandKind::ResmedBlock;
        execute(command);
        if (publish_after) request_resmed_snapshot();
    }

    publish_snapshot_if_needed();
    publish_resmed_snapshot_if_needed();
}

bool OtaHttpController::copy_snapshot(
    LargeTextBuffer &out, uint32_t &revision) const {
    if (!snapshot_mutex_ ||
        xSemaphoreTake(snapshot_mutex_, 0) != pdTRUE) {
        return false;
    }

    out.clear();
    const bool copied = snapshot_json_.length() &&
        out.append(snapshot_json_.c_str(), snapshot_json_.length());
    if (copied) {
        revision = snapshot_revision_;
    }
    xSemaphoreGive(snapshot_mutex_);
    return copied;
}

bool OtaHttpController::copy_resmed_snapshot(
    LargeTextBuffer &out, uint32_t &revision) const {
    if (!snapshot_mutex_ ||
        xSemaphoreTake(snapshot_mutex_, 0) != pdTRUE) {
        return false;
    }

    out.clear();
    const bool copied = resmed_snapshot_json_.length() &&
        out.append(resmed_snapshot_json_.c_str(),
                   resmed_snapshot_json_.length());
    if (copied) revision = resmed_snapshot_revision_;
    xSemaphoreGive(snapshot_mutex_);
    return copied;
}

void OtaHttpController::request_snapshot() {
    snapshot_requested_.store(true, std::memory_order_release);
}

void OtaHttpController::request_resmed_snapshot() {
    resmed_snapshot_requested_.store(true, std::memory_order_release);
}

void OtaHttpController::publish_snapshot_if_needed(bool force) {
    if (!installer_ || !url_source_ || !arduino_source_ || !update_checker_) {
        return;
    }

    const uint32_t now = millis();
    const bool requested =
        snapshot_requested_.exchange(false, std::memory_order_acq_rel);
    if (!force && !requested &&
        static_cast<int32_t>(now - next_snapshot_ms_) < 0) {
        return;
    }

    OtaStatusSnapshot status = collect_ota_status(
        *installer_, *url_source_, *arduino_source_, *update_checker_);
    next_snapshot_ms_ = now +
        (ota_status_active(status) ? SnapshotActiveIntervalMs
                                   : SnapshotIdleIntervalMs);
    if (!force && !requested && snapshot_initialized_ &&
        same_ota_status(published_status_, status)) {
        return;
    }

    snapshot_build_json_.clear();
    build_ota_json(snapshot_build_json_, status);
    if (snapshot_build_json_.overflowed()) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "ESP OTA status snapshot allocation failed\n");
        return;
    }
    if (xSemaphoreTake(snapshot_mutex_, 0) != pdTRUE) {
        snapshot_requested_.store(true, std::memory_order_release);
        return;
    }

    snapshot_json_.swap(snapshot_build_json_);
    published_status_ = std::move(status);
    snapshot_initialized_ = true;
    snapshot_revision_++;
    if (snapshot_revision_ == 0) snapshot_revision_++;
    xSemaphoreGive(snapshot_mutex_);
}

void OtaHttpController::publish_resmed_snapshot_if_needed(bool force) {
    if (!resmed_preparer_ || !resmed_ota_) return;

    const uint32_t now = millis();
    const bool requested =
        resmed_snapshot_requested_.exchange(false, std::memory_order_acq_rel);
    if (!force && !requested &&
        static_cast<int32_t>(now - next_resmed_snapshot_ms_) < 0) {
        return;
    }

    bool active = false;
    resmed_snapshot_build_json_.clear();
    build_resmed_ota_json(resmed_snapshot_build_json_, *resmed_preparer_,
                          *resmed_ota_, &active);
    next_resmed_snapshot_ms_ = now +
        (active ? ResmedSnapshotActiveIntervalMs
                : ResmedSnapshotIdleIntervalMs);
    if (resmed_snapshot_build_json_.overflowed()) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "ResMed OTA status snapshot allocation failed\n");
        return;
    }
    if (xSemaphoreTake(snapshot_mutex_, 0) != pdTRUE) {
        request_resmed_snapshot();
        return;
    }

    const bool changed =
        resmed_snapshot_json_.length() !=
            resmed_snapshot_build_json_.length() ||
        memcmp(resmed_snapshot_json_.c_str(),
               resmed_snapshot_build_json_.c_str(),
               resmed_snapshot_json_.length()) != 0;
    if (force || requested || !resmed_snapshot_initialized_ || changed) {
        resmed_snapshot_json_.swap(resmed_snapshot_build_json_);
        resmed_snapshot_initialized_ = true;
        resmed_snapshot_revision_++;
        if (resmed_snapshot_revision_ == 0) resmed_snapshot_revision_++;
    }
    xSemaphoreGive(snapshot_mutex_);
}

bool OtaHttpController::enqueue(Command &&command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_OTA, LOG_WARN, "HTTP OTA command queue full\n");
    }
    return queued;
}

void OtaHttpController::execute(Command &command) {
    switch (command.kind) {
        case CommandKind::ResmedInit:
            (void)resmed_ota_->begin_upload(
                command.number,
                String(command.sha256.c_str()),
                String(command.filename.c_str()));
            break;

        case CommandKind::ResmedBlock:
            (void)resmed_ota_->submit_block(
                command.number, String(command.data.c_str()));
            break;

        case CommandKind::ResmedInstall:
            if ((command.resmed_transport ==
                     ResmedFirmwareInstallTransport::Service &&
                 !resmed_ota_->status().can_available) ||
                !resmed_ota_->reset_terminal_state() ||
                !resmed_preparer_->request(
                    command.path.c_str(), command.filename.c_str(),
                    command.flag, command.resmed_target,
                    command.resmed_transport)) {
                Log::logf(CAT_OTA, LOG_WARN,
                          "[RESMED] firmware preparation rejected\n");
            }
            break;

        case CommandKind::ResmedDump:
            if (!resmed_ota_->request_firmware_dump()) {
                Log::logf(CAT_OTA, LOG_WARN,
                          "[RESMED] firmware dump rejected\n");
            }
            break;

        case CommandKind::ResmedDumpConfirm:
            if (!resmed_ota_->confirm_dump_bootloader(
                    String(command.confirmation.c_str()))) {
                Log::logf(CAT_OTA, LOG_WARN,
                          "[RESMED] patched bootloader confirmation "
                          "rejected\n");
            }
            break;

        case CommandKind::ResmedCheck:
            (void)resmed_ota_->request_check();
            break;

        case CommandKind::ResmedApplyPlain:
            (void)resmed_ota_->request_apply_plain(
                command.flag, String(command.confirmation.c_str()));
            break;

        case CommandKind::ResmedApplyAuthenticated:
            (void)resmed_ota_->request_apply_authenticated(
                String(command.authentication.c_str()),
                String(command.confirmation.c_str()));
            break;

        case CommandKind::ResmedAbort:
            resmed_preparer_->cancel();
            resmed_ota_->abort("aborted");
            break;
    }
}

void OtaHttpController::send_queue_result(AsyncWebServerRequest *request,
                                          bool queued) const {
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

}  // namespace aircannect
