#include "resmed_firmware_http_controller.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <string.h>
#include <string>

#include "board_net.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "large_text_buffer.h"
#include "resmed_firmware_catalog.h"
#include "resmed_firmware_repository.h"

namespace aircannect {
namespace {

static constexpr size_t CatalogDefaultLimit = 64;
static constexpr size_t CatalogMaxLimit = 128;

template <typename JsonOut>
void append_repository_status(JsonOut &json,
                              const ResmedFirmwareRepositoryStatus &status) {
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state",
                    resmed_firmware_repository_state_name(status.state));
    json_add_string(json, "directory",
                    AC_RESMED_FIRMWARE_REPOSITORY_PATH);
    json_add_int(json, "revision", static_cast<long>(status.revision));
    json_add_bool(json, "truncated", status.truncated);
    json_add_bool(json, "refresh_pending", status.refresh_pending);
    json_add_string(json, "error", status.error);
}

template <typename JsonOut>
void build_repository_status_json(
    JsonOut &json, const ResmedFirmwareRepositoryStatus &status) {
    json = "{";
    append_repository_status(json, status);
    json_add_int(json, "total", static_cast<long>(status.entries));
    json += '}';
}

bool send_json(AsyncWebServerRequest *request,
               int status,
               const LargeTextBuffer &json) {
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"catalog_alloc\"}");
        return false;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return false;
    }

    response->setCode(status);
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
    return true;
}

}  // namespace

bool ResmedFirmwareHttpController::begin(
    ResmedFirmwareRepository &repository) {
    repository_ = &repository;
    if (!status_snapshot_.begin(
            AC_WEB_RESMED_REPOSITORY_JSON_RESERVE) ||
        !status_build_json_.reserve(
            AC_WEB_RESMED_REPOSITORY_JSON_RESERVE)) {
        return false;
    }

    return publish_status_snapshot(true);
}

void ResmedFirmwareHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/resmed-ota/repository"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_catalog(request);
    });

    server.on(AsyncURIMatcher::exact("/api/resmed-ota/repository/refresh"),
              HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        request_refresh(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/resmed-ota/repository/remove"),
        HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            request_remove(request);
        },
        nullptr,
        http_request_body_handler);

}

void ResmedFirmwareHttpController::send_catalog(
    AsyncWebServerRequest *request) const {
    if (!repository_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"repository_unavailable\"}");
        return;
    }

    size_t offset = 0;
    size_t limit = CatalogDefaultLimit;
    if (!http_size_arg(request, "offset", 0, 65535, offset) ||
        !http_size_arg(request, "limit", CatalogDefaultLimit,
                       CatalogMaxLimit, limit) ||
        limit == 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_range\"}");
        return;
    }

    const bool refresh = http_bool_arg(request, "refresh", false);
    if (refresh) (void)repository_->request_refresh(true);

    const ResmedFirmwareRepositoryStatus status = repository_->status();
    const std::shared_ptr<const ResmedFirmwareCatalogSnapshot> snapshot =
        repository_->snapshot();
    const size_t total = snapshot ? snapshot->size() : 0;
    const size_t returned = offset < total
        ? std::min(limit, total - offset)
        : 0;

    LargeTextBuffer json;
    json.reserve(512 + returned * 256);
    json = "{";
    append_repository_status(json, status);
    json += ",\"entries\":[";

    bool first = true;
    for (size_t i = 0; i < returned; ++i) {
        ResmedFirmwareEntryView entry;
        if (!snapshot->entry(offset + i, entry)) {
            request->send(
                503, "application/json",
                "{\"ok\":false,\"error\":\"bad_catalog_snapshot\"}");
            return;
        }

        if (!first) json += ',';
        first = false;
        json += '{';
        json_add_string(json, "path", entry.path, false);
        json_add_string(json, "name", entry.filename);
        json_add_string(json, "kind_hint",
                        resmed_firmware_name_hint_name(entry.name_hint));
        json_add_uint64(json, "size", entry.size);
        json_add_uint64(json, "modified", entry.modified);
        json += '}';
    }

    json += ']';
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "count", static_cast<long>(returned));
    json_add_int(json, "total", static_cast<long>(total));
    json_add_bool(json, "more", offset + returned < total);
    json += '}';

    const bool preparing = !snapshot &&
        status.state != ResmedFirmwareRepositoryState::Error;
    (void)send_json(request, preparing ? 202 : 200, json);
}

void ResmedFirmwareHttpController::poll() {
    (void)publish_status_snapshot();
}

bool ResmedFirmwareHttpController::publish_status_snapshot(bool force) {
    if (!repository_) return false;

    const uint32_t generation = repository_->status_generation();
    if (!generation ||
        (!force && generation == observed_repository_generation_)) {
        return generation != 0;
    }

    const ResmedFirmwareRepositoryStatus status = repository_->status();
    if (!status.generation) return false;

    status_build_json_.clear();
    build_repository_status_json(status_build_json_, status);
    if (status_build_json_.overflowed()) {
        Log::logf(CAT_OTA, LOG_WARN,
                  "[RESMED] repository status snapshot allocation failed\n");
        return false;
    }
    if (!status_snapshot_.replace(status_build_json_)) return false;

    observed_repository_generation_ = status.generation;
    return true;
}

void ResmedFirmwareHttpController::request_refresh(
    AsyncWebServerRequest *request) const {
    if (!repository_ || !repository_->request_refresh(true)) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"repository_busy\"}");
        return;
    }

    request->send(202, "application/json",
                  "{\"ok\":true,\"state\":\"queued\"}");
}

void ResmedFirmwareHttpController::request_remove(
    AsyncWebServerRequest *request) const {
    JsonDocument document;
    std::string body;
    if (!http_parse_json_body(request, document, body) ||
        !document["path"].is<const char *>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_path\"}");
        return;
    }

    const char *path = document["path"].as<const char *>();
    if (!repository_ || !repository_->request_remove(path)) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"remove_rejected\"}");
        return;
    }

    request->send(202, "application/json",
                  "{\"ok\":true,\"state\":\"queued\"}");
}

}  // namespace aircannect
