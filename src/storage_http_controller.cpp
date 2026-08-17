#include "storage_http_controller.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <memory>
#include <stdio.h>
#include <string.h>
#include <string>

#include "async_prepared_response.h"
#include "board.h"
#include "debug_log.h"
#include "http_request_utils.h"
#include "json_util.h"
#include "large_byte_buffer.h"
#include "large_text_buffer.h"
#include "runtime_clock.h"
#include "storage_archive_port.h"
#include "storage_browser_port.h"
#include "storage_delete_port.h"
#include "storage_path.h"
#include "storage_path_port.h"
#include "storage_read_port.h"
#include "storage_service.h"

namespace aircannect {
namespace {

static constexpr size_t WEB_JSON_RESERVE_SMALL = 512;
static constexpr size_t kStorageListDefaultLimit = 64;
static constexpr size_t kStorageListMaxLimit = 128;
static constexpr uint32_t kStorageListRetryMs = 750;
static constexpr size_t kPreparedResponseCopyBytes = 4096;
static constexpr uint32_t kArchiveResponseStartTimeoutMs = 10000;

struct StorageSelectionRequest {
    JsonDocument doc;
    std::string body;
    const char *base = nullptr;
    const char *names[AC_STORAGE_MAX_SELECTIONS] = {};
    size_t count = 0;
};

bool parse_storage_selection_body(AsyncWebServerRequest *request,
                                  StorageSelectionRequest &selection,
                                  const char **error_out) {
    if (error_out) *error_out = "";
    if (!http_parse_json_body(request, selection.doc, selection.body)) {
        if (error_out) *error_out = "bad_json";
        return false;
    }
    if (!selection.doc["base"].is<const char *>() ||
        !selection.doc["items"].is<JsonArray>()) {
        if (error_out) *error_out = "bad_selection";
        return false;
    }
    selection.base = selection.doc["base"].as<const char *>();
    JsonArray items = selection.doc["items"].as<JsonArray>();
    if (!storage_user_path_valid(selection.base) || items.size() == 0 ||
        items.size() > AC_STORAGE_MAX_SELECTIONS) {
        if (error_out) *error_out = "bad_selection";
        return false;
    }
    for (JsonVariant item : items) {
        if (!item.is<const char *>()) {
            if (error_out) *error_out = "bad_selection";
            return false;
        }
        const char *name = item.as<const char *>();
        if (!storage_valid_child_name(name)) {
            if (error_out) *error_out = "bad_name";
            return false;
        }
        selection.names[selection.count++] = name;
    }
    return true;
}

bool append_storage_list_entry(LargeTextBuffer &json,
                               const char *name,
                               const char *path,
                               bool directory,
                               uint64_t size,
                               uint64_t modified,
                               bool first) {
    if (!first) json += ',';
    json += '{';
    json_add_string(json, "name", name, false);
    json_add_string(json, "path", path);
    json_add_string(json, "type", directory ? "dir" : "file");
    if (!directory) json_add_uint64(json, "size", size);
    json_add_uint64(json, "modified", modified);
    json += '}';
    return !json.overflowed();
}

struct ArchiveDownloadRef {
    StorageArchivePort *port = nullptr;
    std::shared_ptr<StorageArchiveDownload> download;
    std::shared_ptr<const LargeByteBuffer> prefix;

    ~ArchiveDownloadRef() {
        if (port && download) port->finish_download(*download);
    }

    size_t fill(uint8_t *buffer, size_t capacity, size_t offset) {
        if (!port || !download || !buffer || capacity == 0) return 0;

        if (prefix && offset < prefix->size()) {
            const size_t copied = std::min(
                capacity, prefix->size() - offset);
            memcpy(buffer, prefix->data() + offset, copied);
            return copied;
        }

        const PreparedByteRead read =
            port->read_download(*download, buffer, capacity, offset);
        if (read.state == PreparedByteReadState::Retry) {
            return RESPONSE_TRY_AGAIN;
        }
        return read.bytes;
    }
};

struct StorageDownloadRef {
    StorageBrowserPort *port = nullptr;
    std::shared_ptr<StoragePreparedDownload> download;

    ~StorageDownloadRef() {
        if (port && download) port->finish_download(*download);
    }
};

bool therapy_request_idle(AsyncWebServerRequest *request,
                          bool therapy_active) {
    if (!therapy_active) return true;

    request->send(409, "application/json",
                  "{\"ok\":false,\"error\":\"therapy_active\"}");
    return false;
}

bool storage_heavy_request_available(AsyncWebServerRequest *request,
                                     bool therapy_active,
                                     const StorageStatusPort *status_port) {
    if (!therapy_request_idle(request, therapy_active)) return false;

    if (!status_port || !status_port->mounted()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"storage_unavailable\"}");
        return false;
    }

    const StorageWorkloadSnapshot storage =
        status_port->workload_snapshot();
    if (!storage.valid || storage.busy || storage.edf_queued > 0 ||
        storage.open_file_count > 0) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    return true;
}

bool storage_read_request_available(AsyncWebServerRequest *request,
                                    bool therapy_active,
                                    const StorageStatusPort *status_port) {
    if (!therapy_request_idle(request, therapy_active)) return false;

    if (!status_port || !status_port->mounted()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"storage_unavailable\"}");
        return false;
    }
    return true;
}

bool storage_jobs_available(AsyncWebServerRequest *request,
                            const StorageArchivePort *archive_port,
                            const StorageDeletePort *delete_port) {
    if (archive_port && archive_port->active()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    if (delete_port && delete_port->active()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    return true;
}

bool storage_delete_available(AsyncWebServerRequest *request,
                              const StorageDeletePort *delete_port) {
    if (delete_port && delete_port->active()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    return true;
}

class StorageJobGate {
public:
    StorageJobGate(AsyncWebServerRequest *request, SemaphoreHandle_t mutex) :
        mutex_(mutex) {
        if (mutex_ &&
            xSemaphoreTake(mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            locked_ = true;
            return;
        }
        if (request) {
            request->send(409,
                          "application/json",
                          "{\"ok\":false,\"error\":\"storage_busy\"}");
        }
    }

    StorageJobGate(const StorageJobGate &) = delete;
    StorageJobGate &operator=(const StorageJobGate &) = delete;

    ~StorageJobGate() {
        if (locked_) xSemaphoreGive(mutex_);
    }

    bool locked() const { return locked_; }

private:
    SemaphoreHandle_t mutex_ = nullptr;
    bool locked_ = false;
};


}  // namespace

struct StorageHttpController::PendingFileLogTail {
    StorageReadPort *port = nullptr;
    OperationTicket ticket;
    StoragePreparedRead prepared;
    std::unique_ptr<LargeByteBuffer> buffer;
    size_t copied = 0;
    AsyncWebServerRequestPtr request;

    ~PendingFileLogTail() {
        if (port && ticket.valid()) (void)port->abandon(ticket);
        if (port && prepared.valid()) port->release_prepared(prepared);
    }
};

struct StorageHttpController::PendingArchiveDownload {
    std::shared_ptr<ArchiveDownloadRef> response;
    std::unique_ptr<LargeByteBuffer> prefix;
    AsyncWebServerRequestPtr request;
    uint64_t archive_size = 0;
    uint32_t deadline_ms = 0;
    char filename[AC_STORAGE_ARCHIVE_NAME_MAX] = {};
};

struct StorageHttpController::PendingStorageRename {
    StoragePathPort *port = nullptr;
    OperationTicket ticket;
    AsyncWebServerRequestPtr request;

    ~PendingStorageRename() {
        if (port && ticket.valid()) (void)port->abandon(ticket);
    }
};

StorageHttpController::StorageHttpController() = default;
StorageHttpController::~StorageHttpController() = default;

bool StorageHttpController::begin(StorageReadPort &read_port,
                                  StorageBrowserPort &browser_port,
                                  StoragePathPort &path_port,
                                  StorageArchivePort &archive_port,
                                  StorageDeletePort &delete_port,
                                  StorageStatusPort &status_port) {
    storage_read_ = &read_port;
    storage_browser_ = &browser_port;
    storage_path_ = &path_port;
    storage_archive_ = &archive_port;
    storage_delete_ = &delete_port;
    storage_status_ = &status_port;

    if (!job_mutex_) {
        job_mutex_ = xSemaphoreCreateMutexStatic(&job_mutex_storage_);
    }
    return job_mutex_ != nullptr;
}

void StorageHttpController::publish_activity(const ActivitySnapshot &activity) {
    therapy_active_.store(activity.therapy_active,
                          std::memory_order_relaxed);
}

void StorageHttpController::poll() {
    poll_file_log_tail();
    poll_archive_download();
    poll_storage_rename();
}

void StorageHttpController::poll_storage_rename() {
    if (!job_mutex_ || !storage_path_ ||
        xSemaphoreTake(job_mutex_, 0) != pdTRUE) {
        return;
    }

    if (!pending_storage_rename_) {
        xSemaphoreGive(job_mutex_);
        return;
    }

    if (pending_storage_rename_->request.expired()) {
        pending_storage_rename_.reset();
        xSemaphoreGive(job_mutex_);
        return;
    }

    StoragePathCompletion completion;
    if (!storage_path_->take_completion(
            pending_storage_rename_->ticket, completion)) {
        xSemaphoreGive(job_mutex_);
        return;
    }

    const AsyncWebServerRequestPtr pending_request =
        pending_storage_rename_->request;
    pending_storage_rename_->ticket = {};
    pending_storage_rename_.reset();
    xSemaphoreGive(job_mutex_);

    std::shared_ptr<AsyncWebServerRequest> request = pending_request.lock();
    if (!request) return;
    if (completion.outcome.disposition != OperationDisposition::Succeeded) {
        char body[128] = {};
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 completion.error[0] ? completion.error : "rename_failed");
        request->send(409, "application/json", body);
        return;
    }

    request->send(200, "application/json", "{\"ok\":true}");
}

void StorageHttpController::poll_file_log_tail() {
    if (!job_mutex_ ||
        xSemaphoreTake(job_mutex_, 0) != pdTRUE) {
        return;
    }

    if (!pending_file_log_tail_) {
        xSemaphoreGive(job_mutex_);
        return;
    }

    if (pending_file_log_tail_->request.expired()) {
        std::unique_ptr<PendingFileLogTail> abandoned =
            std::move(pending_file_log_tail_);
        xSemaphoreGive(job_mutex_);
        return;
    }

    bool failed = false;
    if (pending_file_log_tail_->ticket.valid()) {
        StorageReadCompletion completion;
        if (!storage_read_->take_completion(
                pending_file_log_tail_->ticket, completion)) {
            xSemaphoreGive(job_mutex_);
            return;
        }

        pending_file_log_tail_->ticket = {};
        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.prepared.valid()) {
            if (completion.prepared.valid()) {
                storage_read_->release_prepared(completion.prepared);
            }
            failed = true;
        } else {
            pending_file_log_tail_->prepared = completion.prepared;
            if (completion.prepared.length > 0) {
                pending_file_log_tail_->buffer =
                    LargeByteBuffer::allocate(completion.prepared.length);
                failed = !pending_file_log_tail_->buffer;
            }
        }
    }

    PendingFileLogTail &pending = *pending_file_log_tail_;
    if (!failed && pending.prepared.valid() &&
        pending.prepared.length > 0) {
        const size_t wanted = std::min(
            kPreparedResponseCopyBytes,
            pending.prepared.length - pending.copied);
        const PreparedByteRead read = storage_read_->read_prepared(
            pending.prepared,
            pending.copied,
            pending.buffer->data() + pending.copied,
            wanted);
        if (read.state == PreparedByteReadState::Retry) {
            xSemaphoreGive(job_mutex_);
            return;
        }
        if (read.state != PreparedByteReadState::Data ||
            read.bytes == 0 || read.bytes > wanted) {
            failed = true;
        } else {
            pending.copied += read.bytes;
            if (pending.copied < pending.prepared.length) {
                xSemaphoreGive(job_mutex_);
                return;
            }
        }
    }

    std::shared_ptr<const LargeByteBuffer> payload;
    if (!failed && pending.prepared.valid()) {
        storage_read_->release_prepared(pending.prepared);
        pending.prepared = {};
        if (pending.buffer) {
            payload = LargeByteBuffer::freeze(std::move(pending.buffer));
            failed = !payload;
        }
    }

    const AsyncWebServerRequestPtr pending_request = pending.request;
    pending_file_log_tail_.reset();
    xSemaphoreGive(job_mutex_);

    std::shared_ptr<AsyncWebServerRequest> request = pending_request.lock();
    if (!request) return;
    if (failed) {
        request->send(503, "text/plain", "file log unavailable\n");
        return;
    }

    const size_t length = payload ? payload->size() : 0;
    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            200,
            "text/plain",
            length,
            [payload](uint8_t *buffer,
                      size_t capacity,
                      size_t offset) -> size_t {
                if (!payload || !buffer || offset >= payload->size()) return 0;

                const size_t copied = std::min(
                    capacity, payload->size() - offset);
                memcpy(buffer, payload->data() + offset, copied);
                return copied;
            });
    if (!response) {
        request->send(503, "text/plain", "response alloc\n");
        return;
    }

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void StorageHttpController::poll_archive_download() {
    if (!job_mutex_ ||
        xSemaphoreTake(job_mutex_, 0) != pdTRUE) {
        return;
    }

    if (!pending_archive_download_) {
        xSemaphoreGive(job_mutex_);
        return;
    }

    PendingArchiveDownload &pending = *pending_archive_download_;
    if (pending.request.expired()) {
        pending_archive_download_.reset();
        xSemaphoreGive(job_mutex_);
        return;
    }

    bool failed = millis_deadline_reached(millis(), pending.deadline_ms);
    if (!failed) {
        const PreparedByteRead read = pending.response->port->read_download(
            *pending.response->download,
            pending.prefix->data(),
            pending.prefix->size(),
            0);
        if (read.state == PreparedByteReadState::Retry) {
            xSemaphoreGive(job_mutex_);
            return;
        }
        if (read.state != PreparedByteReadState::Data || read.bytes == 0 ||
            !pending.prefix->truncate(read.bytes)) {
            failed = true;
        } else {
            pending.response->prefix =
                LargeByteBuffer::freeze(std::move(pending.prefix));
            failed = !pending.response->prefix;
        }
    }

    const AsyncWebServerRequestPtr pending_request = pending.request;
    std::shared_ptr<ArchiveDownloadRef> ref = std::move(pending.response);
    const uint64_t archive_size = pending.archive_size;
    char filename[AC_STORAGE_ARCHIVE_NAME_MAX] = {};
    memcpy(filename, pending.filename, sizeof(filename));
    pending_archive_download_.reset();
    xSemaphoreGive(job_mutex_);

    std::shared_ptr<AsyncWebServerRequest> request = pending_request.lock();
    if (!request) return;
    if (failed) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"archive_stream_unavailable\"}");
        return;
    }

    AsyncWebServerResponse *response = new (std::nothrow)
        AsyncPreparedResponse(
            "application/zip",
            static_cast<size_t>(archive_size),
            [ref](uint8_t *buffer,
                  size_t max_length,
                  size_t offset) -> size_t {
                return ref->fill(buffer, max_length, offset);
            });
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }

    char disposition[128] = {};
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"",
             filename[0] ? filename : "archive.zip");
    response->addHeader("Content-Disposition", disposition);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Accept-Ranges", "none");
    request->send(response);
}

void StorageHttpController::register_routes(AsyncWebServer &server) {
    // Storage browser and jobs
    server.on(AsyncURIMatcher::exact("/api/storage/list"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_storage_list(request);
    });

    server.on(AsyncURIMatcher::exact("/api/storage/download"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_storage_download(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/storage/rename"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_storage_rename(request);
        },
        nullptr, http_request_body_handler);

    server.on(
        AsyncURIMatcher::exact("/api/storage/archive/start"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_storage_archive_start(request);
        },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/storage/archive/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_storage_archive_status(request);
    });

    server.on(AsyncURIMatcher::exact("/api/storage/archive/download"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_storage_archive_download(request);
    });

    server.on(
        AsyncURIMatcher::exact("/api/storage/delete/start"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_storage_delete_start(request);
        },
        nullptr, http_request_body_handler);

    server.on(AsyncURIMatcher::exact("/api/storage/delete/status"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_storage_delete_status(request);
    });

    server.on(AsyncURIMatcher::exact("/api/log/current"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        size_t lines = AC_FILE_LOG_TAIL_DEFAULT_LINES;
        if (!http_size_arg(request, "tail",
                           AC_FILE_LOG_TAIL_DEFAULT_LINES,
                           AC_FILE_LOG_TAIL_MAX_LINES, lines) ||
            lines == 0) {
            char error[48];
            snprintf(error, sizeof(error), "tail must be 1..%lu\n",
                     static_cast<unsigned long>(
                         AC_FILE_LOG_TAIL_MAX_LINES));
            request->send(400, "text/plain", error);
            return;
        }

        send_file_log_tail(request, lines);
    });
}

void StorageHttpController::send_storage_list(AsyncWebServerRequest *request) const {
    if (!storage_browser_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"list_unavailable\"}");
        return;
    }

    const String path = request->hasArg("path") ? request->arg("path") : "/";
    size_t offset = 0;
    size_t limit = kStorageListDefaultLimit;
    if (!http_size_arg(request, "offset", 0, 65535, offset) ||
        !http_size_arg(request, "limit", kStorageListDefaultLimit,
                                  kStorageListMaxLimit, limit)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_range\"}");
        return;
    }
    if (limit == 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_range\"}");
        return;
    }
    if (!storage_user_path_valid(path.c_str())) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_path\"}");
        return;
    }
    if (!storage_read_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
        return;
    }

    const bool refresh = http_bool_arg(request, "refresh", false);
    std::shared_ptr<const StorageDirectorySnapshot> snapshot;
    char error[AC_STORAGE_ERROR_MAX] = {};
    const StorageListingRead read = storage_browser_->listing(
        path.c_str(), refresh, snapshot, error, sizeof(error));
    if (read == StorageListingRead::Preparing) {
        char body[80] = {};
        snprintf(body, sizeof(body),
                 "{\"ok\":true,\"state\":\"preparing\",\"retry_ms\":%lu}",
                 static_cast<unsigned long>(kStorageListRetryMs));
        request->send(202, "application/json", body);
        return;
    }
    if (read == StorageListingRead::Error || !snapshot) {
        const int code = strcmp(error, "not_found") == 0 ||
                         strcmp(error, "not_directory") == 0
            ? 404
            : 503;
        char body[128] = {};
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "list_failed");
        request->send(code, "application/json", body);
        return;
    }

    const size_t total = snapshot->size();
    const size_t returned = offset < total
        ? std::min(limit, total - offset)
        : 0;
    const bool truncated = offset + returned < total;

    LargeTextBuffer json;
    json.reserve(512 + returned * 192);
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "path", snapshot->path());
    json_add_int(json, "revision", snapshot->revision());
    json_add_int(json, "offset", static_cast<long>(offset));
    json_add_int(json, "limit", static_cast<long>(limit));
    json += ",\"entries\":[";

    bool first = true;
    for (size_t i = 0; i < returned; ++i) {
        StorageDirectoryEntryView entry;
        if (!snapshot->entry(offset + i, entry)) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"bad_snapshot\"}");
            return;
        }

        char child_path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(snapshot->path(),
                                       entry.name,
                                       child_path,
                                       sizeof(child_path)) ||
            !append_storage_list_entry(json,
                                       entry.name,
                                       child_path,
                                       entry.directory,
                                       entry.size,
                                       entry.modified,
                                       first)) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"list_alloc\"}");
            return;
        }
        first = false;
    }

    json += ']';
    json_add_int(json, "count", static_cast<long>(returned));
    json_add_bool(json, "truncated", truncated);
    if (truncated) {
        char next[24];
        snprintf(next, sizeof(next), "%llu",
                 static_cast<unsigned long long>(offset + returned));
        json += ",\"next_offset\":";
        json += next;
    } else {
        json += ",\"next_offset\":null";
    }
    json += '}';

    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"list_alloc\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

void StorageHttpController::send_storage_download(AsyncWebServerRequest *request) const {
    if (!storage_browser_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"download_unavailable\"}");
        return;
    }
    if (!storage_read_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
        return;
    }
    if (!storage_delete_available(request, storage_delete_)) {
        return;
    }

    if (!request->hasArg("id")) {
        if (!request->hasArg("path")) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"missing_path\"}");
            return;
        }
        const String path = request->arg("path");
        if (!storage_user_path_valid(path.c_str())) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"bad_path\"}");
            return;
        }

        StorageDownloadPrepareStatus status;
        const StorageDownloadPrepareState state =
            storage_browser_->prepare_download(path.c_str(), status);
        if (state == StorageDownloadPrepareState::Busy ||
            state == StorageDownloadPrepareState::Error) {
            const int code = state == StorageDownloadPrepareState::Busy
                ? 409
                : (strcmp(status.error, "not_found") == 0 ||
                   strcmp(status.error, "not_file") == 0 ? 404 : 503);
            char body[128] = {};
            snprintf(body, sizeof(body),
                     "{\"ok\":false,\"error\":\"%s\"}",
                     status.error[0] ? status.error : "download_failed");
            request->send(code, "application/json", body);
            return;
        }

        LargeTextBuffer json;
        json.reserve(256);
        json = "{";
        json_add_bool(json, "ok", true, false);
        json_add_string(json, "state",
                        state == StorageDownloadPrepareState::Ready
                            ? "ready"
                            : "preparing");
        json_add_int(json, "id", static_cast<long>(status.id));
        if (state == StorageDownloadPrepareState::Ready) {
            json_add_uint64(json, "size", status.size);
            json_add_string(json, "filename", status.filename);
        }
        json += '}';
        request->send(state == StorageDownloadPrepareState::Ready ? 200 : 202,
                      "application/json", json.c_str());
        return;
    }

    size_t id_arg = 0;
    if (!http_size_arg(request, "id", 0, 0xffffffffu, id_arg) ||
        id_arg == 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_id\"}");
        return;
    }

    std::shared_ptr<StorageDownloadRef> ref =
        std::make_shared<StorageDownloadRef>();
    if (!ref) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    ref->port = storage_browser_;

    char filename[AC_STORAGE_NAME_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint64_t file_size = 0;
    if (!storage_browser_->begin_download(
            static_cast<uint32_t>(id_arg),
            ref->download,
            filename,
            sizeof(filename),
            file_size,
            error,
            sizeof(error))) {
        char body[128] = {};
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "not_ready");
        request->send(409, "application/json", body);
        return;
    }
    if (file_size > static_cast<uint64_t>(SIZE_MAX)) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"file_too_large\"}");
        return;
    }

    AsyncWebServerResponse *response = new (std::nothrow) AsyncPreparedResponse(
        "application/octet-stream",
        static_cast<size_t>(file_size),
        [ref](uint8_t *buffer, size_t max_len, size_t offset) -> size_t {
            if (!buffer || !ref || !ref->port || !ref->download) return 0;
            const PreparedByteRead read = ref->port->read_download(
                *ref->download, buffer, max_len, offset);
            if (read.state == PreparedByteReadState::Retry) {
                return RESPONSE_TRY_AGAIN;
            }
            return read.bytes;
        });
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    char disposition[96];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"",
             filename[0] ? filename : "download");
    response->addHeader("Content-Disposition", disposition);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Accept-Ranges", "none");
    request->send(response);
}

void StorageHttpController::send_storage_rename(
    AsyncWebServerRequest *request) {
    if (!storage_path_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"rename_unavailable\"}");
        return;
    }

    JsonDocument document;
    std::string body;
    if (!http_parse_json_body(request, document, body) ||
        !document["base"].is<const char *>() ||
        !document["name"].is<const char *>() ||
        !document["new_name"].is<const char *>()) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_rename\"}");
        return;
    }

    const char *base = document["base"].as<const char *>();
    const char *name = document["name"].as<const char *>();
    const char *new_name = document["new_name"].as<const char *>();
    char source[AC_STORAGE_PATH_MAX] = {};
    char destination[AC_STORAGE_PATH_MAX] = {};
    if (!storage_user_path_valid(base) ||
        !storage_valid_child_name(name) ||
        !storage_valid_child_name(new_name) ||
        strlen(name) >= AC_STORAGE_NAME_MAX ||
        strlen(new_name) >= AC_STORAGE_NAME_MAX ||
        strcmp(name, new_name) == 0 ||
        !storage_append_child_path(base, name, source, sizeof(source)) ||
        !storage_append_child_path(
            base, new_name, destination, sizeof(destination))) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_rename\"}");
        return;
    }

    StorageJobGate gate(request, job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_) ||
        !storage_jobs_available(request, storage_archive_, storage_delete_)) {
        return;
    }
    if (pending_storage_rename_) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"rename_busy\"}");
        return;
    }

    storage_rename_generation_++;
    if (storage_rename_generation_ == 0) storage_rename_generation_++;

    StoragePathCommand command;
    command.operation = StoragePathOperation::Move;
    command.source = source;
    command.destination = destination;
    command.generation = storage_rename_generation_;
    const OperationSubmission submission = storage_path_->request(command);
    if (!submission.accepted()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"rename_rejected\"}");
        return;
    }

    std::unique_ptr<PendingStorageRename> pending(
        new (std::nothrow) PendingStorageRename());
    if (!pending) {
        (void)storage_path_->abandon(submission.ticket);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"rename_alloc\"}");
        return;
    }

    pending->port = storage_path_;
    pending->ticket = submission.ticket;
    pending->request = request->pause();
    pending_storage_rename_ = std::move(pending);
}

void StorageHttpController::send_file_log_tail(AsyncWebServerRequest *request,
                                                size_t lines) {
    if (!request || !storage_read_ || !Log::filelog_enabled()) {
        if (request) {
            request->send(404, "text/plain", "file log unavailable\n");
        }
        return;
    }

    StorageJobGate gate(request, job_mutex_);
    if (!gate.locked()) return;
    if (pending_file_log_tail_) {
        request->send(409, "text/plain", "file log busy\n");
        return;
    }

    StorageReadCommand command;
    command.path = AC_FILE_LOG_PATH;
    command.mode = StorageReadMode::TailLines;
    command.length = AC_STORAGE_PREPARED_READ_MAX_BYTES;
    command.tail_lines = lines;
    command.lane = StorageReadLane::Foreground;
    command.generation = 1;

    const OperationSubmission submission =
        storage_read_->request_read(command);
    if (!submission.accepted()) {
        request->send(503, "text/plain", "file log busy\n");
        return;
    }

    std::unique_ptr<PendingFileLogTail> pending(
        new (std::nothrow) PendingFileLogTail());
    if (!pending) {
        (void)storage_read_->abandon(submission.ticket);
        request->send(503, "text/plain", "response alloc\n");
        return;
    }

    pending->port = storage_read_;
    pending->ticket = submission.ticket;
    pending->request = request->pause();
    pending_file_log_tail_ = std::move(pending);
}

void StorageHttpController::send_storage_archive_start(AsyncWebServerRequest *request) const {
    if (!storage_archive_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"archive_unavailable\"}");
        return;
    }
    if (request && request->_tempObject &&
        static_cast<const char *>(request->_tempObject)[0] != 0) {
        StorageSelectionRequest selection;
        const char *parse_error = "bad_selection";
        if (!parse_storage_selection_body(request, selection, &parse_error)) {
            char body[96] = {};
            snprintf(body,
                     sizeof(body),
                     "{\"ok\":false,\"error\":\"%s\"}",
                     parse_error);
            request->send(400, "application/json", body);
            return;
        }
        StorageJobGate gate(request, job_mutex_);
        if (!gate.locked()) return;
        if (!storage_heavy_request_available(
                request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
            return;
        }
        if (!storage_jobs_available(request,
                                    storage_archive_,
                                    storage_delete_)) {
            return;
        }

        uint32_t id = 0;
        char error[AC_STORAGE_ARCHIVE_ERROR_MAX] = {};
        if (!storage_archive_->start_selected(selection.base,
                                              selection.names,
                                              selection.count,
                                              &id,
                                              error,
                                              sizeof(error))) {
            char body[128] = {};
            snprintf(body,
                     sizeof(body),
                     "{\"ok\":false,\"error\":\"%s\"}",
                     error[0] ? error : "archive_start_failed");
            request->send(409, "application/json", body);
            return;
        }

        char body[128] = {};
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":true,\"queued\":true,\"id\":%lu}",
                 static_cast<unsigned long>(id));
        request->send(202, "application/json", body);
        return;
    }
    http_discard_request_body(request);
    if (!request->hasArg("path")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing_path\"}");
        return;
    }
    const String path = request->arg("path");
    if (!storage_user_path_valid(path.c_str())) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_path\"}");
        return;
    }
    StorageJobGate gate(request, job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_)) {
        return;
    }

    const bool recursive =
        http_bool_arg(request, "recursive", true);
    uint32_t id = 0;
    char error[AC_STORAGE_ARCHIVE_ERROR_MAX] = {};
    if (!storage_archive_->start(path.c_str(),
                                 recursive,
                                 &id,
                                 error,
                                 sizeof(error))) {
        char body[128] = {};
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "archive_start_failed");
        request->send(409, "application/json", body);
        return;
    }

    char body[128] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":true,\"queued\":true,\"id\":%lu}",
             static_cast<unsigned long>(id));
    request->send(202, "application/json", body);
}

void StorageHttpController::send_storage_archive_status(AsyncWebServerRequest *request) const {
    if (!storage_archive_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"archive_unavailable\"}");
        return;
    }
    StorageArchiveStatus status;
    if (!storage_archive_->status(status)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"status_unavailable\"}");
        return;
    }
    LargeTextBuffer json;
    json.reserve(WEB_JSON_RESERVE_SMALL);
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state",
                    storage_archive_state_name(status.state));
    json_add_int(json, "id", static_cast<long>(status.id));
    json_add_bool(json, "recursive", status.recursive);
    json_add_string(json, "path", status.source_path);
    json_add_string(json, "filename", status.filename);
    json_add_string(json, "error", status.error);
    json_add_int(json, "files", static_cast<long>(status.files));
    json_add_int(json, "dirs", static_cast<long>(status.dirs));
    json_add_int(json, "files_done", static_cast<long>(status.files_done));
    json_add_uint64(json, "bytes_done", status.bytes_done);
    json_add_uint64(json, "bytes_sent", status.bytes_sent);
    json_add_uint64(json, "estimated_archive_bytes",
                    status.estimated_archive_bytes);
    json += '}';
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"status_alloc\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

void StorageHttpController::send_storage_archive_download(
    AsyncWebServerRequest *request) {
    if (!storage_archive_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"archive_unavailable\"}");
        return;
    }
    size_t id_arg = 0;
    if (!http_size_arg(request, "id", 0, 0xffffffffu, id_arg) ||
        id_arg == 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_id\"}");
        return;
    }
    StorageJobGate gate(request, job_mutex_);
    if (!gate.locked()) return;
    if (pending_archive_download_) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"archive_download_busy\"}");
        return;
    }
    if (!storage_read_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
        return;
    }
    if (!storage_delete_available(request, storage_delete_)) {
        return;
    }

    const uint32_t id = static_cast<uint32_t>(id_arg);
    char filename[AC_STORAGE_ARCHIVE_NAME_MAX] = {};
    std::shared_ptr<ArchiveDownloadRef> ref =
        std::make_shared<ArchiveDownloadRef>();
    if (!ref) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    ref->port = storage_archive_;
    uint64_t archive_size = 0;
    char error[AC_STORAGE_ARCHIVE_ERROR_MAX] = {};
    if (!storage_archive_->begin_download(id,
                                          ref->download,
                                          filename,
                                          sizeof(filename),
                                          archive_size,
                                          error,
                                          sizeof(error))) {
        char body[128] = {};
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "not_ready");
        request->send(409, "application/json", body);
        return;
    }
    if (archive_size > static_cast<uint64_t>(SIZE_MAX)) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"archive_too_large\"}");
        return;
    }
    if (archive_size == 0) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"archive_empty\"}");
        return;
    }

    std::unique_ptr<PendingArchiveDownload> pending(
        new (std::nothrow) PendingArchiveDownload());
    if (!pending) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    pending->prefix = LargeByteBuffer::allocate(std::min(
        kPreparedResponseCopyBytes, static_cast<size_t>(archive_size)));
    if (!pending->prefix) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }

    pending->response = std::move(ref);
    pending->request = request->pause();
    pending->archive_size = archive_size;
    pending->deadline_ms = millis() + kArchiveResponseStartTimeoutMs;
    memcpy(pending->filename, filename, sizeof(pending->filename));
    pending_archive_download_ = std::move(pending);
}

void StorageHttpController::send_storage_delete_start(AsyncWebServerRequest *request) const {
    if (!storage_delete_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"delete_unavailable\"}");
        return;
    }
    StorageSelectionRequest selection;
    const char *parse_error = "bad_selection";
    if (!parse_storage_selection_body(request, selection, &parse_error)) {
        char body[96] = {};
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 parse_error);
        request->send(400, "application/json", body);
        return;
    }
    StorageJobGate gate(request, job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request,
            therapy_active_.load(std::memory_order_relaxed),
            storage_status_)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_)) {
        return;
    }

    uint32_t id = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
    if (!storage_delete_->start_selected(selection.base,
                                         selection.names,
                                         selection.count,
                                         &id,
                                         error,
                                         sizeof(error))) {
        char body[128] = {};
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "delete_start_failed");
        request->send(409, "application/json", body);
        return;
    }

    char body[128] = {};
    snprintf(body,
             sizeof(body),
             "{\"ok\":true,\"queued\":true,\"id\":%lu}",
             static_cast<unsigned long>(id));
    request->send(202, "application/json", body);
}

void StorageHttpController::send_storage_delete_status(AsyncWebServerRequest *request) const {
    if (!storage_delete_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"delete_unavailable\"}");
        return;
    }
    StorageDeleteStatus status;
    if (!storage_delete_->status(status)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"status_unavailable\"}");
        return;
    }
    LargeTextBuffer json;
    json.reserve(WEB_JSON_RESERVE_SMALL);
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state", storage_delete_state_name(status.state));
    json_add_int(json, "id", static_cast<long>(status.id));
    json_add_string(json, "base", status.base_path);
    json_add_string(json, "error", status.error);
    json_add_int(json, "roots", static_cast<long>(status.roots));
    json_add_int(json, "roots_done", static_cast<long>(status.roots_done));
    json_add_int(json,
                 "files_deleted",
                 static_cast<long>(status.files_deleted));
    json_add_int(json,
                 "dirs_deleted",
                 static_cast<long>(status.dirs_deleted));
    json += '}';
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"status_alloc\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

}  // namespace aircannect
