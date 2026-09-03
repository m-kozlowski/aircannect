#include "device_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <string>
#include <string.h>
#include <utility>

#include "as11_device_service.h"
#include "as11_ble_rpc_link.h"
#include "board_net.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "large_text_buffer.h"
#include "rpc_request_port.h"
#include "time_sync_service.h"

namespace aircannect {
namespace {

bool build_ble_pairing_json(LargeTextBuffer &json,
                            const As11BlePairingStatus &pairing,
                            const As11BleLinkStatus &link) {
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_uint64(json, "revision", pairing.revision);
    json_add_bool(json, "enabled", link.enabled);
    json_add_bool(json, "paired", pairing.paired);
    json_add_string(json, "state",
                    as11_ble_pairing_state_name(pairing.state));
    json_add_bool(json, "active", pairing.active);
    json_add_bool(json, "passkey_required", pairing.passkey_required);
    json_add_string(json, "selected_address", pairing.selected_address);
    json_add_string(json, "selected_name", pairing.selected_name);
    json_add_string(json, "error", pairing.error);
    json += ",\"devices\":[";
    for (size_t i = 0; i < pairing.device_count; ++i) {
        if (i) json += ',';
        json += '{';
        json_add_string(json, "address", pairing.devices[i].address, false);
        json_add_string(json, "name", pairing.devices[i].name);
        json_add_int(json, "rssi", pairing.devices[i].rssi);
        json += '}';
    }
    json += "]}";
    return !json.overflowed();
}

}  // namespace

bool DeviceHttpController::begin(RpcRequestPort &rpc,
                                 As11DeviceService &device,
                                 TimeSyncService &time_sync,
                                 As11BleRpcLink &ble_link) {
    rpc_ = &rpc;
    device_ = &device;
    time_sync_ = &time_sync;
    ble_link_ = &ble_link;
    if (!commands_.begin()) return false;

    if (!ble_pairing_snapshot_.begin(
            AC_WEB_AS11_BLE_STATUS_JSON_RESERVE) ||
        !ble_pairing_build_json_.reserve(
            AC_WEB_AS11_BLE_STATUS_JSON_RESERVE)) {
        return false;
    }
    return publish_ble_pairing_snapshot();
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

    server.on(AsyncURIMatcher::exact("/api/as11/ble"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_ble_status(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/as11/ble"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_ble_action(request); },
        nullptr, http_request_body_handler);
}

void DeviceHttpController::poll() {
    if (!rpc_ || !device_ || !time_sync_ || !ble_link_) return;

    as11_unavailable_.store(device_->unavailable(),
                            std::memory_order_release);

    for (size_t i = 0; i < CommandsPerPoll; ++i) {
        Command command;
        if (!commands_.pop(command)) break;
        execute(command);
    }

    if (ble_link_->pairing_revision() != observed_ble_pairing_revision_) {
        (void)publish_ble_pairing_snapshot();
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
        case CommandKind::BlePairScan:
            (void)ble_link_->request_pairing_scan();
            break;
        case CommandKind::BlePairSelect:
            (void)ble_link_->request_pairing_device(command.value.c_str());
            break;
        case CommandKind::BlePairPasskey:
            (void)ble_link_->submit_pairing_passkey(command.value.c_str());
            break;
        case CommandKind::BlePairCancel:
            (void)ble_link_->cancel_pairing();
            break;
        case CommandKind::BlePairForget:
            (void)ble_link_->forget_pairing();
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

    const bool requires_as11 = command.kind == CommandKind::TimePush ||
                               command.kind == CommandKind::TimePull;
    if (requires_as11 &&
        as11_unavailable_.load(std::memory_order_acquire)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"as11_unavailable\"}");
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

    if (as11_unavailable_.load(std::memory_order_acquire)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"as11_unavailable\"}");
        return;
    }

    const bool queued = enqueue(command);
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

void DeviceHttpController::send_ble_status(
    AsyncWebServerRequest *request) const {
    if (!ble_link_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"ble_unavailable\"}");
        return;
    }

    AsyncResponseStream *response = nullptr;
    const JsonSnapshotResponse result =
        ble_pairing_snapshot_.prepare_response(request, response);
    if (result == JsonSnapshotResponse::Busy) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache_busy\"}");
        return;
    }
    if (result != JsonSnapshotResponse::Ready) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    request->send(response);
}

bool DeviceHttpController::publish_ble_pairing_snapshot() {
    if (!ble_link_) return false;

    const As11BlePairingStatus pairing = ble_link_->pairing_status();
    if (pairing.revision == observed_ble_pairing_revision_) return true;

    const As11BleLinkStatus link = ble_link_->ble_status();
    ble_pairing_build_json_.clear();
    if (!build_ble_pairing_json(ble_pairing_build_json_, pairing, link)) {
        return false;
    }

    if (!ble_pairing_snapshot_.replace(ble_pairing_build_json_)) {
        return false;
    }

    observed_ble_pairing_revision_ = pairing.revision;
    return true;
}

void DeviceHttpController::send_ble_action(
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
    if (strcmp(action, "pair") == 0 || strcmp(action, "scan") == 0) {
        command.kind = CommandKind::BlePairScan;
    } else if (strcmp(action, "select") == 0) {
        command.kind = CommandKind::BlePairSelect;
        command.value = doc["address"] | "";
    } else if (strcmp(action, "passkey") == 0) {
        command.kind = CommandKind::BlePairPasskey;
        command.value = doc["passkey"] | "";
    } else if (strcmp(action, "cancel") == 0) {
        command.kind = CommandKind::BlePairCancel;
    } else if (strcmp(action, "forget") == 0) {
        command.kind = CommandKind::BlePairForget;
    } else {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_action\"}");
        return;
    }

    if ((command.kind == CommandKind::BlePairSelect ||
         command.kind == CommandKind::BlePairPasskey) &&
        command.value.empty()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"value_required\"}");
        return;
    }

    const bool queued = enqueue(std::move(command));
    request->send(queued ? 202 : 503, "application/json",
                  queued ? "{\"ok\":true,\"result\":\"queued\"}"
                         : "{\"ok\":false,\"error\":\"queue_full\"}");
}

}  // namespace aircannect
