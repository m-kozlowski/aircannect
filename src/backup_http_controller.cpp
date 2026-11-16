#include "backup_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <string.h>
#include <time.h>

#include "http_request_utils.h"
#include "http_response_utils.h"
#include "http_route_registry.h"
#include "json_util.h"
#include "large_allocator.h"
#include "large_text_buffer.h"

namespace aircannect {

bool BackupHttpController::begin(SettingsBackupService &service) {
    service_ = &service;
    return commands_.begin();
}

void BackupHttpController::register_routes(HttpRouteRegistry &server) {
    server.on(AsyncURIMatcher::exact("/api/backup"), HTTP_POST,
              [this](AsyncWebServerRequest *request) { submit(request); },
              nullptr, http_request_body_handler);
}

void BackupHttpController::submit(AsyncWebServerRequest *request) {
    JsonDocument document;
    std::string body;
    if (!http_parse_json_body(request, document, body)) {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_request\"}");
        return;
    }

    Command command;
    const char *kind = document["kind"] | "";
    const char *action = document["action"] | "";
    if (strcmp(kind, "config") == 0) command.operation.kind = SettingsBackupKind::Config;
    else if (strcmp(kind, "clinical") == 0) command.operation.kind = SettingsBackupKind::Clinical;
    else action = "";

    if (strcmp(action, "create") == 0) command.operation.action = SettingsBackupAction::Create;
    else if (strcmp(action, "preview") == 0) command.operation.action = SettingsBackupAction::Preview;
    else if (strcmp(action, "restore") == 0) command.operation.action = SettingsBackupAction::Restore;
    else if (strcmp(action, "cancel") == 0) command.operation.action = SettingsBackupAction::Cancel;
    else if (strcmp(action, "directory") == 0) command.operation.action = SettingsBackupAction::Directory;
    else {
        request->send(400, "application/json", "{\"ok\":false,\"error\":\"bad_request\"}");
        return;
    }
    command.operation.options.secrets = document["secrets"] | false;
    command.operation.options.internal = document["internal"] | false;
    command.operation.path = document["path"] | "";
    command.operation.plan = document["plan"] | 0u;

    try {
        command.response = std::allocate_shared<AsyncDeferredResponse::State>(
            LargeAllocator<AsyncDeferredResponse::State>());
        auto response = std::unique_ptr<AsyncDeferredResponse>(
            new AsyncDeferredResponse(command.response));
        if (!commands_.push(std::move(command))) {
            request->send(503, "application/json", "{\"ok\":false,\"error\":\"backup_busy\"}");
            return;
        }
        request->send(response.release());
    } catch (const std::bad_alloc &) {
        request->send(503, "application/json", "{\"ok\":false,\"error\":\"response_alloc\"}");
    }
}

void BackupHttpController::poll() {
    if (!service_) return;
    if (response_ && response_->cancelled()) {
        service_->cancel();
        response_.reset();
    }

    service_->poll(millis());
    if (response_ && service_->result().ready) publish_result();

    Command command;
    if (!commands_.pop(command) || command.response->cancelled()) return;
    if (response_ || !service_->start(command.operation, millis(), time(nullptr))) {
        command.response->publish(http_json_error_response(409, "backup_busy"));
        return;
    }
    response_ = std::move(command.response);
    if (service_->result().ready) publish_result();
}

void BackupHttpController::publish_result() {
    const auto &result = service_->result();
    LargeTextBuffer json;
    json = result.error.empty() ? "{\"ok\":true" : "{\"ok\":false";
    if (!result.error.empty()) json_add_string(json, "error", result.error.c_str());
    json_add_string(json, "path", result.path.c_str());
    json_add_int(json, "plan", result.plan);
    if (result.plan) {
        const auto &plan = service_->clinical_plan();
        json_add_string(json, "serial", plan.source_serial.c_str());
        json_add_string(json, "created", plan.created.c_str());
        json_add_int(json, "unchanged", plan.unchanged);
        json += ",\"entries\":[";
        bool first = true;
        for (const auto &entry : plan.entries) {
            if (!first) json += ',';
            first = false;
            json += '{';
            json_add_string(json, "key", entry.key.c_str(), false);
            json_add_string(json, "before", entry.before.c_str());
            json_add_string(json, "value", entry.value.c_str());
            if (entry.reason) json_add_string(json, "reason", entry.reason);
            json += '}';
        }
        json += ']';
    } else {
        json_add_int(json, "applied", result.config.applied);
        json_add_int(json, "kept", result.config.kept);
        json_add_int(json, "rejected", result.config.rejected);
        json += ",\"issues\":[";
        for (size_t i = 0; i < result.config.issues.size(); ++i) {
            if (i) json += ',';
            json += '{';
            const auto &issue = result.config.issues[i];
            json_add_string(json, "key", issue.key.c_str(), false);
            json_add_string(json, "reason", issue.reason);
            json += '}';
        }
        json += ']';
    }
    json += '}';

    AsyncWebServerResponse *response = nullptr;
    if (!http_prepare_json_response(std::move(json), response)) {
        response_->publish(http_json_error_response(503, "response_alloc"));
    } else {
        response->setCode(result.error.empty() ? 200 : 409);
        response->addHeader("Cache-Control", "no-store");
        response_->publish(std::unique_ptr<AsyncWebServerResponse>(response));
    }
    response_.reset();
}

}  // namespace aircannect
