#include "export_http_controller.h"

#include <ESPAsyncWebServer.h>

#include "export_coordinator.h"
#include "json_util.h"
#include "large_text_buffer.h"

namespace aircannect {
namespace {

static constexpr size_t SMB_STATUS_JSON_RESERVE = 1536;
static constexpr size_t SLEEPHQ_STATUS_JSON_RESERVE = 1024;

void append_sleephq_sync_json(LargeTextBuffer &json,
                              const SleepHqSyncStatus &status,
                              const char *configured_team_id,
                              const char *device_id) {
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state", sleephq_sync_state_name(status.state));
    json_add_bool(json, "configured", status.configured);
    json_add_bool(json, "network_available", status.network_available);
    json_add_bool(json, "pending", status.pending);
    json_add_string(json, "pending_reason", status.pending_reason);
    json_add_string(json, "error", status.last_error);
    json_add_string(json, "current_path", status.current_path);
    json_add_string(json, "configured_team_id", configured_team_id);
    json_add_string(json, "device_id", device_id);
    json_add_int(json, "team_id", static_cast<long>(status.team_id));
    json_add_int(json, "import_id", static_cast<long>(status.import_id));
    json_add_string(json, "import_status", status.import_status);
    json_add_int(json, "files_seen", static_cast<long>(status.files_seen));
    json_add_int(json, "files_uploaded",
                 static_cast<long>(status.files_uploaded));
    json_add_int(json, "files_skipped",
                 static_cast<long>(status.files_skipped));
    json_add_int(json, "files_failed",
                 static_cast<long>(status.files_failed));
    json_add_uint64(json, "bytes_uploaded", status.bytes_uploaded);
    json_add_uint64(json, "last_check_epoch", status.last_check_epoch);
    json_add_uint64(json, "last_sync_epoch", status.last_sync_epoch);
    json_add_int(json, "last_sync_files_seen",
                 static_cast<long>(status.last_sync_files_seen));
    json_add_int(json, "last_sync_files_uploaded",
                 static_cast<long>(status.last_sync_files_uploaded));
    json_add_int(json, "last_sync_files_failed",
                 static_cast<long>(status.last_sync_files_failed));
    json_add_uint64(json, "last_sync_bytes_uploaded",
                    status.last_sync_bytes_uploaded);
    json_add_uint64(json, "last_failure_epoch", status.last_failure_epoch);
    json_add_int(json, "retry_due_ms",
                 static_cast<long>(status.retry_due_ms));
    json_add_int(json, "retry_attempt",
                 static_cast<long>(status.retry_attempt));
    json_add_int(json, "started_ms", static_cast<long>(status.started_ms));
    json_add_int(json, "updated_ms", static_cast<long>(status.updated_ms));
}

bool send_status_json(AsyncWebServerRequest *request, LargeTextBuffer &json) {
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"status_alloc\"}");
        return false;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return false;
    }

    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
    return true;
}

}  // namespace

bool ExportHttpController::begin(ExportCoordinator &coordinator) {
    coordinator_ = &coordinator;
    return true;
}

void ExportHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/storage/sync/start"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_smb_sync_start(request);
    });

    server.on(AsyncURIMatcher::exact("/api/storage/sync/verify"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_smb_sync_verify(request);
    });

    server.on(AsyncURIMatcher::exact("/api/storage/sync/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_smb_sync_status(request);
    });

    server.on(AsyncURIMatcher::exact("/api/sleephq/sync/start"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_start(request);
    });

    server.on(AsyncURIMatcher::exact("/api/sleephq/sync/check"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_check(request);
    });

    server.on(AsyncURIMatcher::exact("/api/sleephq/sync/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_status(request);
    });
}

void ExportHttpController::send_smb_sync_start(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }
    if (!coordinator_->request_smb_sync()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }

    request->send(202, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void ExportHttpController::send_smb_sync_verify(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }
    if (!coordinator_->request_smb_verify()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }

    request->send(202, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void ExportHttpController::send_smb_sync_status(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }

    const ExportSmbStatusSnapshot snapshot = coordinator_->smb_snapshot();
    const StorageSyncStatus &status = snapshot.sync;
    LargeTextBuffer json;
    json.reserve(SMB_STATUS_JSON_RESERVE);
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state", storage_sync_state_name(status.state));
    json_add_bool(json, "enabled", status.enabled);
    json_add_bool(json, "configured", status.configured);
    json_add_string(json, "endpoint", snapshot.smb_endpoint);
    json_add_bool(json, "network_available", status.network_available);
    json_add_bool(json, "pending", status.pending);
    json_add_bool(json, "last_run_verify", status.last_run_verify);
    json_add_bool(json, "last_run_reconcile", status.last_run_reconcile);
    json_add_string(json, "pending_reason", status.pending_reason);
    json_add_string(json, "error", status.last_error);
    json_add_string(json, "current_path", status.current_path);
    json_add_int(json, "files_seen", static_cast<long>(status.files_seen));
    json_add_int(json, "files_uploaded",
                 static_cast<long>(status.files_uploaded));
    json_add_int(json, "files_skipped",
                 static_cast<long>(status.files_skipped));
    json_add_int(json, "files_failed",
                 static_cast<long>(status.files_failed));
    json_add_uint64(json, "bytes_uploaded", status.bytes_uploaded);
    json_add_uint64(json, "last_sync_epoch", status.last_sync_epoch);
    json_add_int(json, "last_sync_files_seen",
                 static_cast<long>(status.last_sync_files_seen));
    json_add_int(json, "last_sync_files_uploaded",
                 static_cast<long>(status.last_sync_files_uploaded));
    json_add_int(json, "last_sync_files_skipped",
                 static_cast<long>(status.last_sync_files_skipped));
    json_add_int(json, "last_sync_files_failed",
                 static_cast<long>(status.last_sync_files_failed));
    json_add_uint64(json, "last_sync_bytes_uploaded",
                    status.last_sync_bytes_uploaded);
    json_add_uint64(json, "last_verify_epoch", status.last_verify_epoch);
    json_add_int(json, "last_verify_files_seen",
                 static_cast<long>(status.last_verify_files_seen));
    json_add_uint64(json, "last_reconcile_epoch",
                    status.last_reconcile_epoch);
    json_add_int(json, "last_reconcile_files_seen",
                 static_cast<long>(status.last_reconcile_files_seen));
    json_add_uint64(json, "last_failure_epoch", status.last_failure_epoch);
    json_add_string(json, "last_failure_error", status.last_failure_error);
    json_add_int(json, "started_ms", static_cast<long>(status.started_ms));
    json_add_int(json, "updated_ms", static_cast<long>(status.updated_ms));
    json_add_int(json, "retry_due_ms",
                 static_cast<long>(status.retry_due_ms));
    json_add_int(json, "retry_attempt",
                 static_cast<long>(status.retry_attempt));
    json += '}';

    (void)send_status_json(request, json);
}

void ExportHttpController::send_sleephq_sync_start(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }
    if (!coordinator_->request_sleephq_sync()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }

    request->send(200, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void ExportHttpController::send_sleephq_sync_check(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }
    if (!coordinator_->request_sleephq_check()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"check_not_ready\"}");
        return;
    }

    request->send(200, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void ExportHttpController::send_sleephq_sync_status(
    AsyncWebServerRequest *request) const {
    if (!coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }

    const ExportSleepHqStatusSnapshot snapshot =
        coordinator_->sleephq_snapshot();
    const SleepHqSyncStatus &status = snapshot.sync;
    LargeTextBuffer json;
    json.reserve(SLEEPHQ_STATUS_JSON_RESERVE);
    json = "{";
    append_sleephq_sync_json(json, status,
                             snapshot.sleephq_team_id,
                             snapshot.sleephq_device_id);
    json += '}';

    (void)send_status_json(request, json);
}

}  // namespace aircannect
