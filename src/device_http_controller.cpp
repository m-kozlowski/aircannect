#include "device_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <string>
#include <string.h>
#include <utility>

#include "as11_device_service.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "rpc_request_port.h"
#include "time_sync_service.h"

namespace aircannect {

bool DeviceHttpController::begin(RpcRequestPort &rpc,
                                 As11DeviceService &device,
                                 TimeSyncService &time_sync) {
    rpc_ = &rpc;
    device_ = &device;
    time_sync_ = &time_sync;
    return commands_.begin();
}

void DeviceHttpController::register_routes(AsyncWebServer &server) {
    server.on(
        AsyncURIMatcher::exact("/api/time"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_time_action(request); },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/therapy"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_therapy_action(request);
        },
        nullptr, http_request_body_handler);
}

void DeviceHttpController::poll() {
    if (!rpc_ || !device_ || !time_sync_) return;

    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;
        execute(command);
    }
}

bool DeviceHttpController::enqueue(Command command) {
    const bool queued = commands_.push(std::move(command));
    if (!queued) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "HTTP device command queue full\n");
    }
    return queued;
}

void DeviceHttpController::execute(Command command) {
    switch (command.kind) {
        case CommandKind::TimeNtp:
            time_sync_->force_ntp_sync();
            break;
        case CommandKind::TimePush:
            time_sync_->request_push_esp_to_resmed(RpcSource::HttpApi);
            break;
        case CommandKind::TimePull:
            time_sync_->request_pull_resmed_to_esp(RpcSource::HttpApi);
            break;
        case CommandKind::TimeReset:
            time_sync_->reset_resmed_push();
            break;
        case CommandKind::TherapyStart:
            (void)device_->request_therapy(
                *rpc_, As11TherapyTarget::Running, RpcSource::HttpApi,
                millis());
            break;
        case CommandKind::TherapyStop:
            (void)device_->request_therapy(
                *rpc_, As11TherapyTarget::Standby, RpcSource::HttpApi,
                millis());
            break;
    }
}

void DeviceHttpController::send_time_action(
    AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char *action = doc["action"] | "";
    Command command;
    if (strcmp(action, "ntp_sync") == 0) {
        command.kind = CommandKind::TimeNtp;
    } else if (strcmp(action, "sync_to_resmed") == 0) {
        command.kind = CommandKind::TimePush;
    } else if (strcmp(action, "sync_from_resmed") == 0) {
        command.kind = CommandKind::TimePull;
    } else if (strcmp(action, "retry_resmed_push") == 0) {
        command.kind = CommandKind::TimeReset;
    } else {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_action\"}");
        return;
    }

    const bool queued = enqueue(command);
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

void DeviceHttpController::send_therapy_action(
    AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!http_parse_json_body(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_json\"}");
        return;
    }

    const char *action = doc["action"] | "";
    Command command;
    if (strcmp(action, "start") == 0) {
        command.kind = CommandKind::TherapyStart;
    } else if (strcmp(action, "stop") == 0 ||
               strcmp(action, "standby") == 0) {
        command.kind = CommandKind::TherapyStop;
    } else {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_action\"}");
        return;
    }

    const bool queued = enqueue(command);
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

}  // namespace aircannect
