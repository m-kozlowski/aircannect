#include "storage_upload_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_system.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdlib.h>
#include <string>
#include <string.h>

#include "board_storage.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "large_byte_buffer.h"
#include "storage_archive_port.h"
#include "storage_delete_port.h"
#include "storage_path.h"
#include "storage_service.h"
#include "storage_upload_port.h"

namespace aircannect {
namespace {

static constexpr uint32_t CAPABILITY_IDLE_TIMEOUT_MS = 15 * 60 * 1000;
static constexpr uint64_t UPLOAD_FREE_RESERVE_BYTES = 1024ULL * 1024ULL;

struct UploadChunkBody {
    LargeByteBuffer *buffer = nullptr;
    StorageUploadChunkResult result;
    uint32_t id = 0;
    uint64_t offset = 0;
    size_t received = 0;
    int response_status = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
    bool submitted = false;
};

void copy_error(char *destination,
                size_t destination_size,
                const char *source) {
    if (!destination || destination_size == 0) return;
    const size_t length = std::min(
        source ? strlen(source) : 0, destination_size - 1);
    if (length > 0) memcpy(destination, source, length);
    destination[length] = '\0';
}

void json_add_u64(String &out,
                  const char *key,
                  uint64_t value,
                  bool comma = true) {
    if (comma) out += ',';
    out += '"';
    out += key;
    out += "\":";

    char number[24] = {};
    snprintf(number, sizeof(number), "%llu",
             static_cast<unsigned long long>(value));
    out += number;
}

void release_chunk_body(AsyncWebServerRequest *request) {
    if (!request || !request->_tempObject) return;

    UploadChunkBody *body =
        static_cast<UploadChunkBody *>(request->_tempObject);
    delete body->buffer;
    body->buffer = nullptr;
    free(body);
    request->_tempObject = nullptr;
}

bool parse_uint64_arg(AsyncWebServerRequest *request,
                      const char *name,
                      uint64_t &out) {
    out = 0;
    if (!request || !name || !request->hasArg(name)) return false;

    const String value = request->arg(name);
    if (value.isEmpty() || value[0] == '-') return false;

    char *end = nullptr;
    const unsigned long long parsed = strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0') return false;

    out = static_cast<uint64_t>(parsed);
    return true;
}

bool parse_uint32_arg(AsyncWebServerRequest *request,
                      const char *name,
                      uint32_t &out) {
    uint64_t value = 0;
    if (!parse_uint64_arg(request, name, value) || value == 0 ||
        value > UINT32_MAX) {
        out = 0;
        return false;
    }

    out = static_cast<uint32_t>(value);
    return true;
}

String upload_status_json(const StorageUploadStatus &status) {
    String json;
    json.reserve(384);
    json += '{';
    json_add_int(json, "id", static_cast<long>(status.id), false);
    json_add_string(json, "state", storage_upload_state_name(status.state));
    json_add_u64(json, "total_bytes", status.total_bytes);
    json_add_u64(json, "committed_bytes", status.committed_bytes);
    json_add_string(json, "path", status.path);
    json_add_string(json, "error", status.error);
    json += '}';
    return json;
}

String upload_error_json(const char *error, uint64_t committed_bytes = 0) {
    String json;
    json.reserve(160);
    json += '{';
    json_add_bool(json, "ok", false, false);
    json_add_string(json, "error", error ? error : "upload_failed");
    json_add_u64(json, "committed_bytes", committed_bytes);
    json += '}';
    return json;
}

}  // namespace

bool StorageUploadHttpController::begin(StorageUploadPort &upload_port,
                                        StorageArchivePort &archive_port,
                                        StorageDeletePort &delete_port,
                                        StorageStatusPort &status_port) {
    upload_port_ = &upload_port;
    archive_port_ = &archive_port;
    delete_port_ = &delete_port;
    status_port_ = &status_port;

    if (!capability_mutex_) {
        capability_mutex_ = xSemaphoreCreateMutexStatic(
            &capability_mutex_storage_);
    }
    return capability_mutex_ != nullptr;
}

void StorageUploadHttpController::publish_activity(
    const ActivitySnapshot &activity) {
    therapy_active_.store(activity.therapy_active,
                          std::memory_order_relaxed);
}

void StorageUploadHttpController::register_routes(AsyncWebServer &server) {
    server.on(
        AsyncURIMatcher::exact("/api/storage/upload/start"), HTTP_POST,
        [this](AsyncWebServerRequest *request) { send_start(request); },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/storage/upload/chunk"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_chunk_result(request);
        },
        nullptr,
        [this](AsyncWebServerRequest *request,
               uint8_t *data,
               size_t length,
               size_t index,
               size_t total) {
            receive_chunk(request, data, length, index, total);
        });

    server.on(AsyncURIMatcher::exact("/api/storage/upload/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_status(request);
    });

    server.on(AsyncURIMatcher::exact("/api/storage/upload/cancel"), HTTP_POST,
              [this](AsyncWebServerRequest *request) {
        send_cancel(request);
    });
}

void StorageUploadHttpController::send_start(
    AsyncWebServerRequest *request) {
    if (!upload_port_ || !status_port_) {
        http_discard_request_body(request);
        request->send(503, "application/json",
                      upload_error_json("upload_unavailable"));
        return;
    }
    if (therapy_active_.load(std::memory_order_relaxed)) {
        http_discard_request_body(request);
        request->send(409, "application/json",
                      upload_error_json("therapy_active"));
        return;
    }
    if (!status_port_->mounted()) {
        http_discard_request_body(request);
        request->send(503, "application/json",
                      upload_error_json("storage_unavailable"));
        return;
    }

    const StorageServiceStatus storage = status_port_->status();
    if (storage.busy || storage.edf_queued > 0 ||
        storage.open_file_count > 0 ||
        (archive_port_ && archive_port_->active()) ||
        (delete_port_ && delete_port_->active())) {
        http_discard_request_body(request);
        request->send(409, "application/json",
                      upload_error_json("storage_busy"));
        return;
    }

    JsonDocument document;
    std::string body;
    if (!http_parse_json_body(request, document, body) ||
        !document["directory"].is<const char *>() ||
        !document["filename"].is<const char *>() ||
        !document["total_size"].is<uint64_t>()) {
        request->send(400, "application/json",
                      upload_error_json("bad_request"));
        return;
    }

    const char *directory = document["directory"].as<const char *>();
    const char *filename = document["filename"].as<const char *>();
    char path[AC_STORAGE_PATH_MAX] = {};
    if (!storage_append_child_path(directory, filename, path, sizeof(path))) {
        request->send(400, "application/json",
                      upload_error_json("bad_path"));
        return;
    }

    StorageUploadConflict conflict = StorageUploadConflict::Fail;
    const char *conflict_value = document["conflict"] | "fail";
    if (strcmp(conflict_value, "replace") == 0) {
        conflict = StorageUploadConflict::Replace;
    } else if (strcmp(conflict_value, "fail") != 0) {
        request->send(400, "application/json",
                      upload_error_json("bad_conflict"));
        return;
    }

    uint32_t generation = next_generation_.fetch_add(
        1, std::memory_order_relaxed) + 1;
    if (generation == 0) {
        generation = next_generation_.fetch_add(
            1, std::memory_order_relaxed) + 1;
    }

    StorageUploadStartCommand command;
    command.path = path;
    command.total_size = document["total_size"].as<uint64_t>();
    command.expected_sha256 =
        document["sha256"].is<const char *>()
            ? document["sha256"].as<const char *>()
            : "";
    command.free_reserve_bytes = UPLOAD_FREE_RESERVE_BYTES;
    command.conflict = conflict;
    command.generation = generation;

    const StorageUploadStartResult result = upload_port_->start(command);
    if (!result.accepted()) {
        const int status = result.admission == OperationAdmission::Busy
            ? 409
            : 400;
        request->send(status, "application/json",
                      upload_error_json(result.error[0]
                                            ? result.error
                                            : "upload_rejected"));
        return;
    }

    char token[33] = {};
    if (!issue_capability(result.id, token)) {
        (void)upload_port_->cancel(result.id);
        request->send(503, "application/json",
                      upload_error_json("capability_unavailable"));
        return;
    }

    String json;
    json.reserve(256);
    json += '{';
    json_add_bool(json, "ok", true, false);
    json_add_int(json, "id", static_cast<long>(result.id));
    json_add_string(json, "token", token);
    json_add_int(json, "chunk_size", static_cast<long>(result.chunk_size));
    json_add_u64(json, "committed_bytes", 0);
    json += '}';
    request->send(202, "application/json", json);
}

void StorageUploadHttpController::receive_chunk(
    AsyncWebServerRequest *request,
    uint8_t *data,
    size_t length,
    size_t index,
    size_t total) {
    if (!request) return;

    if (index == 0) {
        release_chunk_body(request);
        request->onDisconnect([request]() {
            release_chunk_body(request);
        });

        UploadChunkBody *body = static_cast<UploadChunkBody *>(
            calloc(1, sizeof(UploadChunkBody)));
        request->_tempObject = body;
        if (!body) return;

        uint32_t id = 0;
        uint64_t offset = 0;
        const String token = request->hasArg("token")
            ? request->arg("token")
            : String();
        if (!parse_uint32_arg(request, "id", id) ||
            !parse_uint64_arg(request, "offset", offset)) {
            body->response_status = 400;
            copy_error(body->error, sizeof(body->error), "bad_chunk_args");
            return;
        }
        if (!authorize_chunk(id, token.c_str())) {
            body->response_status = 403;
            copy_error(body->error, sizeof(body->error),
                       "invalid_capability");
            return;
        }
        if (total == 0 || total > AC_STORAGE_UPLOAD_CHUNK_BYTES) {
            body->response_status = 413;
            copy_error(body->error, sizeof(body->error), "bad_chunk_size");
            return;
        }

        std::unique_ptr<LargeByteBuffer> buffer =
            LargeByteBuffer::allocate(total);
        if (!buffer) {
            body->response_status = 503;
            copy_error(body->error, sizeof(body->error),
                       "chunk_allocation_failed");
            return;
        }

        body->id = id;
        body->offset = offset;
        body->buffer = buffer.release();
    }

    UploadChunkBody *body =
        static_cast<UploadChunkBody *>(request->_tempObject);
    if (!body || body->response_status != 0 || !body->buffer) return;
    if (index != body->received || length > total - body->received) {
        body->response_status = 400;
        copy_error(body->error, sizeof(body->error), "chunk_body_order");
        return;
    }

    if (length > 0) {
        memcpy(body->buffer->data() + body->received, data, length);
        body->received += length;
    }
    if (body->received != total) return;

    StorageUploadChunkCommand command;
    command.id = body->id;
    command.offset = body->offset;
    command.bytes = LargeByteBuffer::freeze(
        std::unique_ptr<LargeByteBuffer>(body->buffer));
    body->buffer = nullptr;
    body->result = upload_port_->submit(command);
    body->submitted = true;
}

void StorageUploadHttpController::send_chunk_result(
    AsyncWebServerRequest *request) {
    UploadChunkBody *body = request
        ? static_cast<UploadChunkBody *>(request->_tempObject)
        : nullptr;
    if (!body) {
        request->send(400, "application/json",
                      upload_error_json("missing_chunk_body"));
        return;
    }

    request->_tempObject = nullptr;
    request->onDisconnect(nullptr);
    if (body->response_status != 0) {
        request->send(body->response_status, "application/json",
                      upload_error_json(body->error));
        delete body->buffer;
        free(body);
        return;
    }
    if (!body->submitted) {
        request->send(400, "application/json",
                      upload_error_json("short_chunk_body"));
        delete body->buffer;
        free(body);
        return;
    }

    const StorageUploadChunkResult result = body->result;
    free(body);
    if (!result.accepted()) {
        const int status = result.admission == OperationAdmission::Busy
            ? 409
            : 400;
        request->send(status, "application/json",
                      upload_error_json(result.error[0]
                                            ? result.error
                                            : "chunk_rejected",
                                        result.committed_bytes));
        return;
    }

    String json;
    json.reserve(96);
    json += '{';
    json_add_bool(json, "ok", true, false);
    json_add_u64(json, "committed_bytes", result.committed_bytes);
    json += '}';
    request->send(202, "application/json", json);
}

void StorageUploadHttpController::send_status(
    AsyncWebServerRequest *request) {
    uint32_t id = 0;
    if (!parse_uint32_arg(request, "id", id)) {
        request->send(400, "application/json",
                      upload_error_json("bad_upload_id"));
        return;
    }

    StorageUploadStatus status;
    if (!upload_port_ || !upload_port_->status(id, status)) {
        request->send(404, "application/json",
                      upload_error_json("upload_not_found"));
        return;
    }
    if (!status.active()) clear_capability(id);

    request->send(200, "application/json", upload_status_json(status));
}

void StorageUploadHttpController::send_cancel(
    AsyncWebServerRequest *request) {
    uint32_t id = 0;
    if (!parse_uint32_arg(request, "id", id)) {
        request->send(400, "application/json",
                      upload_error_json("bad_upload_id"));
        return;
    }
    if (!upload_port_ || !upload_port_->cancel(id)) {
        request->send(409, "application/json",
                      upload_error_json("upload_not_active"));
        return;
    }

    clear_capability(id);
    request->send(202, "application/json", "{\"ok\":true}");
}

bool StorageUploadHttpController::issue_capability(uint32_t id,
                                                   char token_out[33]) {
    if (!capability_mutex_ || id == 0 || !token_out) return false;

    uint8_t random[16] = {};
    esp_fill_random(random, sizeof(random));
    static constexpr char DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(random); ++i) {
        token_out[i * 2] = DIGITS[(random[i] >> 4) & 0x0F];
        token_out[i * 2 + 1] = DIGITS[random[i] & 0x0F];
    }
    token_out[32] = '\0';

    if (xSemaphoreTake(capability_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        token_out[0] = '\0';
        return false;
    }
    copy_error(capability_token_, sizeof(capability_token_), token_out);
    capability_upload_id_ = id;
    capability_last_used_ms_ = millis();
    xSemaphoreGive(capability_mutex_);
    return true;
}

bool StorageUploadHttpController::authorize_chunk(uint32_t id,
                                                  const char *token) {
    if (!capability_mutex_ || id == 0 || !token || !token[0]) return false;
    if (xSemaphoreTake(capability_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return false;
    }

    const uint32_t now_ms = millis();
    const bool current = capability_upload_id_ == id &&
                         capability_token_[0] != '\0' &&
                         strcmp(capability_token_, token) == 0;
    const bool fresh = static_cast<int32_t>(
        now_ms - capability_last_used_ms_) <
        static_cast<int32_t>(CAPABILITY_IDLE_TIMEOUT_MS);
    const bool accepted = current && fresh;
    if (accepted) capability_last_used_ms_ = now_ms;
    xSemaphoreGive(capability_mutex_);
    return accepted;
}

void StorageUploadHttpController::clear_capability(uint32_t id) {
    if (!capability_mutex_ ||
        xSemaphoreTake(capability_mutex_, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    if (id == 0 || capability_upload_id_ == id) {
        capability_token_[0] = '\0';
        capability_upload_id_ = 0;
        capability_last_used_ms_ = 0;
    }
    xSemaphoreGive(capability_mutex_);
}

}  // namespace aircannect
