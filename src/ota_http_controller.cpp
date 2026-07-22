#include "ota_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <utility>

#include "arduino_ota_source.h"
#include "board.h"
#include "debug_log.h"
#include "firmware_installer.h"
#include "firmware_url_source.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "ota_status.h"
#include "resmed_ota_manager.h"
#include "update_checker.h"
#include "version.h"

namespace aircannect {
namespace {

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
    return true;
}

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

template <typename JsonOut>
void build_ota_json(JsonOut &json, const OtaStatusSnapshot &ota) {
    json = "{";
    json_add_string(json, "version", aircannect_version(), false);
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

const char *resmed_phase_name(ResmedOtaPhase phase) {
    switch (phase) {
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
    }
    return "unknown";
}

template <typename JsonOut>
void build_resmed_ota_json(JsonOut &json,
                           const ResmedOtaManager &manager) {
    const ResmedOtaStatus status = manager.status();
    json = "{";
    json_add_string(json, "phase", resmed_phase_name(status.phase), false);
    json_add_bool(json, "active", manager.active());
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
    json_add_string(json, "target", status.target.c_str());
    json_add_string(json, "staged_path", status.staged_path.c_str());
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
                        const ResmedOtaManager &manager,
                        int response_status) {
    String json;
    json.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
    build_resmed_ota_json(json, manager);
    request->send(response_status, "application/json", json);
}

}  // namespace

bool OtaHttpController::begin(FirmwareInstaller &installer,
                              FirmwareUrlSource &url_source,
                              ArduinoOtaSource &arduino_source,
                              UpdateChecker &update_checker,
                              ResmedOtaManager &resmed_ota) {
    installer_ = &installer;
    url_source_ = &url_source;
    arduino_source_ = &arduino_source;
    update_checker_ = &update_checker;
    resmed_ota_ = &resmed_ota;

    return commands_.begin();
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
        send_resmed_status(request, *resmed_ota_, 200);
    });

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            const ResmedOtaStatus status = resmed_ota_->status();
            const bool ok = status.phase == ResmedOtaPhase::Staging &&
                            resmed_ota_->finish_staged_upload();
            send_resmed_status(request, *resmed_ota_, ok ? 200 : 400);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t length, bool) {
            if (index == 0) {
                size_t input_size = 0;
                if (!required_size_arg(request, "size", input_size)) {
                    resmed_ota_->abort("missing_size");
                    return;
                }

                const String magic =
                    request->hasArg("magic") ? request->arg("magic") : "";
                if (!resmed_ota_->begin_staged_upload(
                        input_size, filename, magic)) {
                    return;
                }
            }

            (void)resmed_ota_->write_staged_upload(index, data, length);
        });

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
            (void)json_get_string(doc, "sha256", sha256);
            (void)json_get_string(doc, "filename", filename);

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
            if (!json_get_string(doc, "data", data)) {
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
            (void)json_get_string(doc, "mode", mode);
            (void)json_get_string(doc, "confirm", confirm);

            Command command;
            command.confirmation = confirm.c_str();
            if (mode == "plain") {
                command.kind = CommandKind::ResmedApplyPlain;
                command.flag = doc["reset"].is<bool>() &&
                               doc["reset"].as<bool>();
            } else if (mode == "authenticated") {
                String authentication;
                if (!json_get_string(doc, "authentication",
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

        execute(command);
    }
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
