#include "web_ui.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <time.h>
#include <utility>

#include "auth_utils.h"
#include "app_config_registry.h"
#include "app_config_update.h"
#include "async_prepared_response.h"
#include "as11_rpc.h"
#include "background_worker.h"
#include "board.h"
#include "debug_log.h"
#include "storage_service.h"
#include "export_coordinator.h"
#include "json_util.h"
#include "memory_manager.h"
#include "report_records.h"
#include "report_range_tile.h"
#include "report_store.h"
#include "session_manager.h"
#include "sink_manager.h"
#include "storage_archive_port.h"
#include "storage_directory.h"
#include "storage_manager.h"
#include "storage_path.h"
#include "system_status_snapshot.h"
#include "string_util.h"
#include "string_print.h"
#include "version.h"
#include "web_ui_html.h"

namespace aircannect {

namespace {

static constexpr size_t WEB_JSON_RESERVE_SMALL = 512;
static constexpr size_t WEB_JSON_RESERVE_MEDIUM = 1024;
static constexpr size_t WEB_CONFIG_FULL_JSON_RESERVE = 2304;
static constexpr size_t WEB_CONFIG_SCHEMA_JSON_RESERVE = 12 * 1024;
static constexpr size_t WEB_CONFIG_SYNC_JSON_RESERVE = 640;
static constexpr size_t WEB_CONFIG_SMALL_JSON_RESERVE = 384;
static constexpr size_t WEB_REPORT_RESULT_JSON_RESERVE = 4096;
static constexpr size_t WEB_REPORT_SUMMARY_JSON_RESERVE = 24 * 1024;
static constexpr size_t WEB_REPORT_PLOT_HTTP_ETAG_MAX = 144;
static constexpr uint32_t WEB_LIVE_VIEW_LEASE_MS = 12000;
static constexpr uint32_t WEB_REPORT_SLOW_HANDLER_MS = 1000;

class ScopedReportHttpTimer {
public:
    ScopedReportHttpTimer(const char *endpoint, long index = -1) :
        endpoint_(endpoint), index_(index), start_ms_(millis()) {}

    ~ScopedReportHttpTimer() {
        const uint32_t elapsed_ms =
            static_cast<uint32_t>(millis() - start_ms_);
        if (elapsed_ms < WEB_REPORT_SLOW_HANDLER_MS) return;

        if (index_ >= 0) {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "slow HTTP report endpoint=%s index=%ld ms=%lu\n",
                      endpoint_ ? endpoint_ : "--",
                      index_,
                      static_cast<unsigned long>(elapsed_ms));
        } else {
            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "slow HTTP report endpoint=%s ms=%lu\n",
                      endpoint_ ? endpoint_ : "--",
                      static_cast<unsigned long>(elapsed_ms));
        }
    }

    ScopedReportHttpTimer(const ScopedReportHttpTimer &) = delete;
    ScopedReportHttpTimer &operator=(const ScopedReportHttpTimer &) = delete;

private:
    const char *endpoint_ = nullptr;
    long index_ = -1;
    uint32_t start_ms_ = 0;
};

const char *web_command_name(uint8_t kind) {
    switch (kind) {
        case WebCommandConsoleLine: return "console_line";
        case WebCommandConsoleClear: return "console_clear";
        case WebCommandConfigUpdate: return "config_update";
        case WebCommandWifiUpdate: return "wifi_update";
        case WebCommandTimeAction: return "time_action";
        case WebCommandSettingsRefresh: return "settings_refresh";
        case WebCommandSettingsUpdate: return "settings_update";
        case WebCommandTherapyAction: return "therapy_action";
        case WebCommandOximetryAction: return "oximetry_action";
        case WebCommandReportSummaryRefresh:
            return "report_summary_refresh";
        case WebCommandResmedOtaInit: return "resmed_ota_init";
        case WebCommandResmedOtaBlock: return "resmed_ota_block";
        case WebCommandResmedOtaCheck: return "resmed_ota_check";
        case WebCommandResmedOtaApply: return "resmed_ota_apply";
        case WebCommandResmedOtaAbort: return "resmed_ota_abort";
        default: return "unknown";
    }
}

bool web_config_section_known(const char *section) {
    if (!section || !section[0] || strcmp(section, "all") == 0) return true;
    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(section, app_config_group_id(fields[i].group)) == 0) {
            return true;
        }
    }
    return false;
}

bool web_config_section_includes(const char *section, AppConfigGroup group) {
    return !section || !section[0] || strcmp(section, "all") == 0 ||
           strcmp(section, app_config_group_id(group)) == 0;
}

size_t json_escaped_capacity(size_t raw_len, size_t overhead = 128) {
    // JSON string escaping can expand a control byte to "\\u00XX".
    return overhead + raw_len * 6;
}

uint32_t fnv1a32_string(const String &text) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < text.length(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 16777619u;
    }
    return hash ? hash : 1u;
}

size_t web_config_json_reserve(const char *section) {
    if (!section || !section[0] || strcmp(section, "all") == 0) {
        return 4096;
    }
    if (strcmp(section, "access") == 0 ||
        strcmp(section, "logging") == 0) {
        return 2048;
    }
    if (strcmp(section, "smb") == 0 ||
        strcmp(section, "sleephq") == 0) {
        return WEB_CONFIG_SYNC_JSON_RESERVE;
    }
    return WEB_CONFIG_SMALL_JSON_RESERVE;
}

const char *web_config_field_type_name(AppConfigFieldType type) {
    switch (type) {
        case AppConfigFieldType::Bool: return "bool";
        case AppConfigFieldType::UInt16: return "number";
        case AppConfigFieldType::String: return "text";
        case AppConfigFieldType::Secret: return "password";
        case AppConfigFieldType::Enum:
        case AppConfigFieldType::LogLevel:
            return "enum";
    }
    return "text";
}

static constexpr AppConfigEnumValue WEB_LOG_LEVEL_VALUES[] = {
    {"ERROR", "Error"},
    {"WARN", "Warn"},
    {"INFO", "Info"},
    {"DEBUG", "Debug"},
};

void append_config_schema_enum(LargeTextBuffer &json,
                               const AppConfigFieldDescriptor &field) {
    const AppConfigEnumValue *values = field.enum_values;
    size_t count = field.enum_value_count;
    if (field.type == AppConfigFieldType::LogLevel) {
        values = WEB_LOG_LEVEL_VALUES;
        count = sizeof(WEB_LOG_LEVEL_VALUES) / sizeof(WEB_LOG_LEVEL_VALUES[0]);
    }
    if (!values || !count) return;

    json += ",\"enum\":[";
    for (size_t i = 0; i < count; ++i) {
        if (i) json += ',';
        json += "{";
        json_add_string(json, "value", values[i].value, false);
        json_add_string(json, "label", values[i].label);
        json += "}";
    }
    json += "]";
}

void release_request_body(AsyncWebServerRequest *request) {
    if (!request || !request->_tempObject) return;
    Memory::free(request->_tempObject);
    request->_tempObject = nullptr;
}

bool append_le32(ReportSpoolBuffer &out, uint32_t value) {
    size_t offset = 0;
    uint8_t *dst = out.append_uninitialized(4, offset);
    if (!dst) return false;
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    return true;
}

void handle_body(AsyncWebServerRequest *request,
                 uint8_t *data,
                 size_t len,
                 size_t index,
                 size_t total) {
    if (index == 0) {
        release_request_body(request);
        if (total > AC_WEB_MAX_POST_BODY) return;
        request->_tempObject =
            Memory::calloc_large(total + 1, sizeof(char));
    }
    char *body = static_cast<char *>(request->_tempObject);
    if (!body || index + len > AC_WEB_MAX_POST_BODY) return;
    memcpy(body + index, data, len);
    body[index + len] = 0;
}

std::string take_request_body(AsyncWebServerRequest *request) {
    std::string body;
    if (request && request->_tempObject) {
        body = static_cast<const char *>(request->_tempObject);
    }
    release_request_body(request);
    return body;
}

bool json_get_string(JsonDocument &doc, const char *key, String &out) {
    if (!doc[key].is<const char *>()) return false;
    out = doc[key].as<const char *>();
    return true;
}

bool parse_body_copy(AsyncWebServerRequest *request,
                     JsonDocument &doc,
                     std::string &body) {
    body = take_request_body(request);
    if (body.empty()) return false;
    return !deserializeJson(doc, body.c_str());
}

int request_profile_mode_arg(AsyncWebServerRequest *request) {
    if (!request) return -1;
    const char *arg_name = nullptr;
    if (request->hasArg("profile_mode")) {
        arg_name = "profile_mode";
    } else if (request->hasArg("mode")) {
        arg_name = "mode";
    } else {
        return -1;
    }
    return as11_mode_index_from_value(
        std::string(request->arg(arg_name).c_str()));
}

int active_settings_mode(const As11DeviceState *device_state,
                         const As11SettingsManager *settings_manager) {
    if (!device_state || !settings_manager) return -1;
    int mode = settings_manager->state().mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(
            device_state->active_therapy_profile());
    }
    return mode;
}

bool request_size_arg(AsyncWebServerRequest *request,
                      const char *name,
                      size_t &out) {
    out = 0;
    if (!request || !name || !request->hasArg(name)) return false;
    const String value = request->arg(name);
    char *end = nullptr;
    const unsigned long long parsed =
        strtoull(value.c_str(), &end, 10);
    if (!end || *end != 0 || parsed == 0 ||
        parsed > AC_RESMED_OTA_MAX_FILE_BYTES) {
        return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
}

bool request_size_arg_limited(AsyncWebServerRequest *request,
                              const char *name,
                              size_t default_value,
                              size_t max_value,
                              size_t &out) {
    out = default_value;
    if (!request || !name || !request->hasArg(name)) return true;
    const String value = request->arg(name);
    char *end = nullptr;
    const unsigned long long parsed =
        strtoull(value.c_str(), &end, 10);
    if (!end || *end != 0 || parsed > max_value) return false;
    out = static_cast<size_t>(parsed);
    return true;
}

bool request_ota_upload_args(AsyncWebServerRequest *request,
                             size_t &image_size,
                             OtaUploadEncoding &encoding,
                             size_t &wire_size) {
    image_size = 0;
    wire_size = 0;
    encoding = OtaUploadEncoding::Auto;

    if (!request_size_arg_limited(request, "size", 0,
                                  AC_RESMED_OTA_MAX_FILE_BYTES,
                                  image_size)) {
        return false;
    }

    if (request && request->hasArg("encoding")) {
        const String value = request->arg("encoding");
        if (!parse_ota_upload_encoding(value.c_str(), encoding)) {
            return false;
        }
    }

    if (request && request->hasArg("wire_size")) {
        if (!request_size_arg(request, "wire_size", wire_size)) return false;
    } else if (image_size) {
        wire_size = image_size;
    }

    if (!wire_size) return false;
    if (encoding == OtaUploadEncoding::Plain) {
        return image_size > 0 && image_size == wire_size;
    }
    return true;
}

bool request_ota_url_args(AsyncWebServerRequest *request,
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

    if (!request_size_arg_limited(request, "size", 0,
                                  AC_RESMED_OTA_MAX_FILE_BYTES,
                                  image_size) ||
        !request_size_arg_limited(request, "wire_size", 0,
                                  AC_RESMED_OTA_MAX_FILE_BYTES,
                                  wire_size)) {
        return false;
    }

    if (encoding == OtaUploadEncoding::Plain) {
        return !image_size || !wire_size || image_size == wire_size;
    }
    return true;
}

bool request_int64_arg(AsyncWebServerRequest *request,
                       const char *name,
                       int64_t &out) {
    out = 0;
    if (!request || !name || !request->hasArg(name)) return false;
    const String value = request->arg(name);
    if (!value.length()) return false;
    char *end = nullptr;
    const long long parsed = strtoll(value.c_str(), &end, 10);
    if (!end || *end != 0 || parsed < 0) return false;
    out = static_cast<int64_t>(parsed);
    return true;
}

bool request_uint64_arg(AsyncWebServerRequest *request,
                        const char *name,
                        uint64_t &out) {
    out = 0;
    if (!request || !name || !request->hasArg(name)) return false;

    const String value = request->arg(name);
    if (!value.length()) return false;

    uint64_t parsed = 0;
    for (size_t i = 0; i < value.length(); ++i) {
        const char ch = value.charAt(i);
        if (ch < '0' || ch > '9') return false;

        const uint8_t digit = static_cast<uint8_t>(ch - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed == 0) return false;

    out = parsed;
    return true;
}

void strip_http_etag_quotes(String &etag) {
    if (etag.length() >= 2 && etag.charAt(0) == '"' &&
        etag.charAt(etag.length() - 1) == '"') {
        etag = etag.substring(1, etag.length() - 1);
    }
}

void add_report_result_cache_headers(AsyncWebServerResponse *response,
                                     const char *etag) {
    if (!response) return;

    if (etag && etag[0]) {
        char etag_header[AC_REPORT_RESULT_ETAG_MAX + 4] = {};
        snprintf(etag_header, sizeof(etag_header), "\"%s\"", etag);
        response->addHeader("ETag", etag_header);
    }

    response->addHeader("Cache-Control", "no-cache");
}

bool format_report_plot_http_etag(const char *plot_etag,
                                  bool range_requested,
                                  int64_t range_from_ms,
                                  int64_t range_to_ms,
                                  char *out,
                                  size_t out_size) {
    if (!plot_etag || !plot_etag[0] || !out || out_size == 0) return false;

    int written = 0;
    if (range_requested) {
        written = snprintf(out,
                           out_size,
                           "%s-%lld-%lld",
                           plot_etag,
                           static_cast<long long>(range_from_ms),
                           static_cast<long long>(range_to_ms));
    } else {
        written = snprintf(out, out_size, "%s", plot_etag);
    }

    return written > 0 && static_cast<size_t>(written) < out_size;
}

void add_report_plot_cache_headers(AsyncWebServerResponse *response,
                                   const char *http_etag) {
    if (!response) return;

    if (http_etag && http_etag[0]) {
        char etag_hdr[WEB_REPORT_PLOT_HTTP_ETAG_MAX + 4] = {};
        snprintf(etag_hdr, sizeof(etag_hdr), "\"%s\"", http_etag);
        response->addHeader("ETag", etag_hdr);
    }

    response->addHeader("Cache-Control",
                        "public, max-age=31536000, immutable");
}

static constexpr size_t kStorageListDefaultLimit = 64;
static constexpr size_t kStorageListMaxLimit = 128;
static constexpr uint32_t kStorageListRetryMs = 750;

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
    if (!parse_body_copy(request, selection.doc, selection.body)) {
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

    ~ArchiveDownloadRef() {
        if (port && download) port->finish_download(*download);
    }
};

struct StorageDownloadRef {
    StorageBrowserPort *port = nullptr;
    std::shared_ptr<StoragePreparedDownload> download;

    ~StorageDownloadRef() {
        if (port && download) port->finish_download(*download);
    }
};

struct PendingPreparedReadRef {
    StorageReadPort *port = nullptr;
    OperationTicket ticket;
    StoragePreparedRead prepared;
    bool failed = false;

    ~PendingPreparedReadRef() {
        if (port && ticket.valid()) (void)port->abandon(ticket);
        if (port && prepared.valid()) port->release_prepared(prepared);
    }

    size_t fill(uint8_t *buffer, size_t capacity, size_t offset) {
        if (!port || !buffer || capacity == 0) return 0;

        if (ticket.valid()) {
            StorageReadCompletion completion;
            if (!port->take_completion(ticket, completion)) {
                return RESPONSE_TRY_AGAIN;
            }

            ticket = {};
            if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded ||
                !completion.prepared.valid()) {
                if (completion.prepared.valid()) {
                    port->release_prepared(completion.prepared);
                }
                failed = true;
            } else {
                prepared = completion.prepared;
            }
        }

        if (failed) {
            static constexpr char MESSAGE[] = "file log unavailable\n";
            const size_t length = sizeof(MESSAGE) - 1;
            if (offset >= length) return 0;

            const size_t count = std::min(capacity, length - offset);
            memcpy(buffer, MESSAGE + offset, count);
            return count;
        }

        if (!prepared.valid() || offset >= prepared.length) return 0;
        return port->read_prepared(prepared, offset, buffer, capacity);
    }
};

bool therapy_request_idle(AsyncWebServerRequest *request,
                          const SessionManager *session_manager,
                          const As11DeviceState *device_state) {
    if (session_manager &&
        session_manager->status().state == SessionState::Active) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"therapy_active\"}");
        return false;
    }
    if (device_state &&
        device_state->therapy_state() == As11TherapyState::Running) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"therapy_active\"}");
        return false;
    }
    return true;
}

bool storage_heavy_request_available(AsyncWebServerRequest *request,
                                     const SessionManager *session_manager,
                                     const As11DeviceState *device_state) {
    if (!therapy_request_idle(request, session_manager, device_state)) {
        return false;
    }
    if (!Storage::mounted()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"storage_unavailable\"}");
        return false;
    }
    const StorageServiceStatus edf_storage = StorageService::status();
    if (edf_storage.busy || edf_storage.edf_queued > 0 ||
        edf_storage.open_file_count > 0) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    return true;
}

bool storage_read_request_available(AsyncWebServerRequest *request,
                                    const SessionManager *session_manager,
                                    const As11DeviceState *device_state) {
    if (!therapy_request_idle(request, session_manager, device_state)) {
        return false;
    }
    if (!Storage::mounted()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"storage_unavailable\"}");
        return false;
    }
    return true;
}

bool storage_jobs_available(AsyncWebServerRequest *request,
                            const StorageArchivePort *archive_port,
                            const StorageDeletePort *delete_port,
                            const StorageSyncJob *sync_job,
                            const SleepHqSyncJob *sleephq_job) {
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
    if (sync_job && sync_job->active()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"storage_busy\"}");
        return false;
    }
    if (sleephq_job && sleephq_job->active()) {
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

void append_sleephq_sync_json(LargeTextBuffer &json,
                              const SleepHqSyncStatus &status,
                              const AppConfigData &config,
                              bool include_ok) {
    bool comma = false;
    if (include_ok) {
        json_add_bool(json, "ok", true, false);
        comma = true;
    }
    json_add_string(json, "state", sleephq_sync_state_name(status.state),
                    comma);
    json_add_bool(json, "configured", status.configured);
    json_add_bool(json, "network_available", status.network_available);
    json_add_bool(json, "pending", status.pending);
    json_add_string(json, "pending_reason", status.pending_reason);
    json_add_string(json, "error", status.last_error);
    json_add_string(json, "current_path", status.current_path);
    json_add_string(json, "configured_team_id",
                    config.sleephq_team_id.c_str());
    json_add_string(json, "device_id",
                    config.sleephq_device_id.c_str());
    json_add_int(json, "team_id",
                 static_cast<long>(status.team_id));
    json_add_int(json, "import_id",
                 static_cast<long>(status.import_id));
    json_add_string(json, "import_status", status.import_status);
    json_add_int(json, "files_seen",
                 static_cast<long>(status.files_seen));
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
    json_add_int(json, "started_ms",
                 static_cast<long>(status.started_ms));
    json_add_int(json, "updated_ms",
                 static_cast<long>(status.updated_ms));
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

bool request_bool_arg_default(AsyncWebServerRequest *request,
                              const char *name,
                              bool default_value) {
    if (!request || !name || !request->hasArg(name)) return default_value;
    String value = request->arg(name);
    value.trim();
    value.toLowerCase();
    if (value == "0" || value == "false" || value == "off" ||
        value == "no") {
        return false;
    }
    return true;
}

String settings_placeholder_json(bool refresh_queued, bool snapshot_pending) {
    String json = "{";
    json_add_bool(json, "valid", false, false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_bool(json, "snapshot_pending", snapshot_pending);
    json_add_int(json, "pending_count", 0);
    json_add_string(json, "last_write_status", "");
    json += ",\"last_write_age_ms\":null";
    json += ",\"age_ms\":null";
    json += ",\"settings\":[]}";
    return json;
}

bool motor_hours(std::string_view iso_duration,
                 char *out,
                 size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = 0;
    if (iso_duration.size() < 4 || iso_duration.substr(0, 2) != "PT") {
        return false;
    }
    size_t start = 2;
    const size_t end = iso_duration.find('S', start);
    if (end == std::string_view::npos || end == start) return false;
    uint64_t seconds = 0;
    for (size_t i = start; i < end; ++i) {
        const char c = iso_duration[i];
        if (c < '0' || c > '9') return false;
        seconds = seconds * 10 + static_cast<unsigned>(c - '0');
    }
    const unsigned long hours =
        static_cast<unsigned long>((seconds + 1800) / 3600);
    snprintf(out, out_size, "%lu", hours);
    return true;
}

const char *setting_kind_name(As11SettingKind kind) {
    switch (kind) {
        case As11SettingKind::Number: return "number";
        case As11SettingKind::Enum: return "enum";
        case As11SettingKind::Bool: return "bool";
        case As11SettingKind::Text: return "text";
        default: return "text";
    }
}

const char *stream_command_name(StreamCommandType type) {
    switch (type) {
        case StreamCommandType::Start: return "start";
        case StreamCommandType::Stop: return "stop";
        case StreamCommandType::None:
        default: return "none";
    }
}

const char *oximetry_source_name(OximetrySource source) {
    switch (source) {
        case OximetrySource::None: return "none";
        case OximetrySource::Udp: return "udp";
        case OximetrySource::Ble: return "ble";
        default: return "unknown";
    }
}

const char *oximetry_sensor_state_name(OximetrySensorState state) {
    switch (state) {
        case OximetrySensorState::Off: return "off";
        case OximetrySensorState::Idle: return "idle";
        case OximetrySensorState::Scanning: return "scanning";
        case OximetrySensorState::Connecting: return "connecting";
        case OximetrySensorState::Connected: return "connected";
        case OximetrySensorState::Streaming: return "streaming";
        default: return "unknown";
    }
}

template <typename JsonOut>
void append_oximetry_sensor(JsonOut &json,
                            const OximetrySensorDevice &device,
                            size_t index,
                            bool include_index) {
    json += '{';
    if (include_index) json_add_int(json, "index", index, false);
    else json_add_string(json, "addr", device.addr, false);
    if (include_index) json_add_string(json, "addr", device.addr);
    json_add_int(json, "addr_type", device.addr_type);
    json_add_string(json, "name", device.name);
    json_add_int(json, "rssi", device.rssi);
    json_add_bool(json, "autoconnect", device.autoconnect);
    json += '}';
}

template <typename JsonOut>
void append_json_float_value(JsonOut &json, float value) {
    append_json_float(json, value);
}

template <typename JsonOut>
void append_live_series(JsonOut &json,
                        const char *key,
                        const LiveChartSeriesBatch &series,
                        bool comma = true) {
    if (comma) json += ',';
    json += '"';
    json += key;
    json += "\":[";
    const size_t count =
        series.count <= series.capacity ? series.count : series.capacity;
    for (size_t i = 0; series.values && series.valid && i < count; ++i) {
        if (i) json += ',';
        if (!series.valid[i]) {
            json += "null";
        } else {
            append_json_float_value(json, series.values[i]);
        }
    }
    json += ']';
}

template <typename JsonOut>
void build_ota_json(JsonOut &json, const OtaManagerStatus &ota) {
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
    json_add_bool(json, "update_check_attempted",
                  ota.update_check_attempted);
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
void build_resmed_ota_json(JsonOut &json, const ResmedOtaManager &ota) {
    const ResmedOtaStatus status = ota.status();
    json = "{";
    json_add_string(json, "phase", ota.phase_name(), false);
    json_add_bool(json, "active", ota.active());
    json_add_bool(json, "waiting", status.waiting);
    json_add_int(json, "total_size", static_cast<long>(status.total_size));
    json_add_int(json, "uploaded_bytes",
                 static_cast<long>(status.uploaded_bytes));
    json_add_int(json, "xfer_block_size",
                 static_cast<long>(status.xfer_block_size));
    json_add_int(json, "progress", status.progress_percent);
    json_add_string(json, "filename", status.filename.c_str());
    json_add_string(json, "expected_sha256",
                    status.expected_sha256.c_str());
    json_add_string(json, "computed_sha256",
                    status.computed_sha256.c_str());
    json_add_string(json, "apply_mode", status.apply_mode.c_str());
    json_add_string(json, "input_type", status.input_type.c_str());
    json_add_string(json, "target", status.target.c_str());
    json_add_string(json, "staged_path", status.staged_path.c_str());
    json_add_string(json, "last_result", status.last_result.c_str());
    json_add_string(json, "last_error", status.last_error.c_str());
    json += '}';
}

}  // namespace

bool WebUI::begin(RpcRequestPort &rpc,
                  StreamBroker &stream,
                  As11DeviceService &device,
                  As11SettingsManager &settings_manager,
                  WifiManager &wifi_manager,
                  TcpBridge &tcp_bridge,
                  AppConfig &app_config,
                  TimeSyncService &time_sync_service,
                  OtaManager &ota_manager,
                  ResmedOtaManager &resmed_ota_manager,
                  SessionManager &session_manager,
                  SinkManager &sink_manager,
                  OximetryManager &oximetry_manager,
                  ReportManager &report_manager,
                  StorageReadPort &storage_read,
                  StorageBrowserPort &storage_browser,
                  StorageArchivePort &storage_archive,
                  StorageDeletePort &storage_delete,
                  ExportCoordinator &export_coordinator,
                  StorageSyncJob *storage_sync_job,
                  SleepHqSyncJob *sleephq_sync_job,
                  ConsoleContext &console_ctx,
                  uint16_t port) {
    if (started_) return true;
    stop();
    rpc_ = &rpc;
    stream_ = &stream;
    device_ = &device;
    settings_manager_ = &settings_manager;
    wifi_manager_ = &wifi_manager;
    tcp_bridge_ = &tcp_bridge;
    app_config_ = &app_config;
    time_sync_service_ = &time_sync_service;
    ota_manager_ = &ota_manager;
    resmed_ota_manager_ = &resmed_ota_manager;
    session_manager_ = &session_manager;
    sink_manager_ = &sink_manager;
    oximetry_manager_ = &oximetry_manager;
    report_manager_ = &report_manager;
    storage_read_ = &storage_read;
    storage_browser_ = &storage_browser;
    storage_archive_ = &storage_archive;
    storage_delete_ = &storage_delete;
    export_coordinator_ = &export_coordinator;
    storage_sync_job_ = storage_sync_job;
    sleephq_sync_job_ = sleephq_sync_job;
    console_ctx_ = &console_ctx;

    if (!command_mutex_) {
        command_mutex_ = xSemaphoreCreateMutexStatic(&command_mutex_storage_);
    }
    if (!cache_mutex_) {
        cache_mutex_ = xSemaphoreCreateMutexStatic(&cache_mutex_storage_);
    }
    if (!sse_mutex_) {
        sse_mutex_ = xSemaphoreCreateMutexStatic(&sse_mutex_storage_);
    }
    if (!live_view_mutex_) {
        live_view_mutex_ =
            xSemaphoreCreateMutexStatic(&live_view_mutex_storage_);
    }
    if (!storage_job_mutex_) {
        storage_job_mutex_ =
            xSemaphoreCreateMutexStatic(&storage_job_mutex_storage_);
    }
    if (!command_mutex_ || !cache_mutex_ || !sse_mutex_ ||
        !live_view_mutex_ || !storage_job_mutex_) {
        stop();
        return false;
    }
    reserve_cached_json();

    server_ = new AsyncWebServer(port);
    if (!server_) {
        stop();
        return false;
    }
    server_->addMiddleware([this](AsyncWebServerRequest *request,
                                 ArMiddlewareNext next) {
        if (request_allowed_cached(request)) {
            next();
            return;
        }
        request->requestAuthentication(AsyncAuthType::AUTH_BASIC,
                                       "AirCANnect");
    });
    events_ = new AsyncEventSource("/api/events");
    if (events_) {
        events_->onConnect([this](AsyncEventSourceClient *client) {
            handle_sse_connect(client);
        });
        events_->onDisconnect([this](AsyncEventSourceClient *client) {
            handle_sse_disconnect(client);
        });
        server_->addHandler(events_);
    }
    register_routes();
    publish_snapshots(true);
    server_->begin();
    started_ = true;
    Log::logf(CAT_GENERAL, LOG_INFO, "[WEB] listening on port %u\n", port);
    return true;
}

void WebUI::reserve_cached_json() {
    cached_status_json_.reserve(AC_WEB_STATUS_JSON_RESERVE);
    cached_stream_json_.reserve(AC_WEB_STREAM_JSON_RESERVE);
    cached_wifi_json_.reserve(AC_WEB_WIFI_JSON_RESERVE);
    cached_oximetry_sensors_json_.reserve(
        AC_WEB_OXIMETRY_SENSORS_JSON_RESERVE);
    cached_ota_json_.reserve(AC_WEB_OTA_JSON_RESERVE);
    cached_resmed_ota_json_.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
    cached_settings_json_.reserve(AC_WEB_SETTINGS_JSON_RESERVE);
    live_json_.reserve(4096);
}

WebUiMemoryStatus WebUI::memory_status() {
    auto capture = [](const LargeTextBuffer &buffer) {
        WebUiBufferMemoryStatus out;
        out.length = buffer.length();
        out.capacity = buffer.capacity();
        return out;
    };

    WebUiMemoryStatus out;
    out.started = started_;
    if (sse_mutex_ && xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            AsyncEventSourceClient *client = ref.client;
            if (!client) continue;
            if (!client->connected()) {
                ref.client = nullptr;
                ref.connected_ms = 0;
                continue;
            }
            const size_t pending = client->packetsWaiting();
            out.sse_clients++;
            out.sse_pending_total += pending;
            if (pending > out.sse_pending_worst) {
                out.sse_pending_worst = pending;
            }
        }
        xSemaphoreGive(sse_mutex_);
    } else {
        out.sse_clients = events_ ? events_->count() : 0;
    }
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return out;
    }
    out.status = capture(cached_status_json_);
    out.stream = capture(cached_stream_json_);
    out.console.length = console_log_length_;
    out.console.capacity = console_log_capacity_;
    out.wifi = capture(cached_wifi_json_);
    out.oximetry_sensors = capture(cached_oximetry_sensors_json_);
    out.ota = capture(cached_ota_json_);
    out.resmed_ota = capture(cached_resmed_ota_json_);
    out.settings = capture(cached_settings_json_);
    out.live = capture(live_json_);
    out.console_log_length = console_log_length_;
    xSemaphoreGive(cache_mutex_);
    return out;
}

void WebUI::stop() {
    if (sink_manager_) sink_manager_->set_live_chart_enabled(false);
    web_console_.cancel_pending_storage();
    if (events_) {
        events_->close();
    }
    if (server_) {
        server_->end();
        delete server_;
        server_ = nullptr;
    } else if (events_) {
        delete events_;
    }
    events_ = nullptr;

    if (command_mutex_) {
        if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            command_queue_.clear();
            xSemaphoreGive(command_mutex_);
        } else {
            command_queue_.clear();
        }
    }

    if (sse_mutex_) {
        if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (SseClientRef &ref : sse_clients_) ref = {};
            xSemaphoreGive(sse_mutex_);
        }
    } else {
        for (SseClientRef &ref : sse_clients_) ref = {};
    }

    if (live_view_mutex_) {
        if (xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
            for (LiveViewLease &lease : live_view_leases_) lease = {};
            xSemaphoreGive(live_view_mutex_);
        }
    } else {
        for (LiveViewLease &lease : live_view_leases_) lease = {};
    }

    sse_enforce_needed_ = false;
    live_last_send_ms_ = 0;
    live_json_ = "";
    Memory::free(console_log_);
    console_log_ = nullptr;
    console_log_capacity_ = 0;
    console_log_start_ = 0;
    console_log_length_ = 0;
    console_log_write_pos_ = 0;
    console_sse_pos_ = 0;
    console_sse_reset_pending_ = false;
    if (sink_manager_) {
        sink_manager_->set_live_chart_enabled(false);
        sink_manager_->clear_live_chart_batch();
    }
    snapshots_ready_ = false;
    snapshots_dirty_mask_ = SNAPSHOT_ALL;
    observed_settings_refresh_pending_ = false;
    observed_settings_revision_ = 0;
    observed_device_revision_ = 0;
    last_snapshot_ms_ = 0;
    last_sse_push_ms_ = 0;
    sse_push_requested_ = false;
    report_manager_ = nullptr;
    stream_ = nullptr;
    started_ = false;
}

void WebUI::poll(PollCheckpoint checkpoint) {
    if (!started_) return;
    const bool realtime_active =
        stream_ &&
        (stream_->activity_active(millis(), AC_WIFI_ROAM_STREAM_QUIET_MS) ||
         (device_ && device_->state().therapy_state() ==
                         As11TherapyState::Running));
    if (device_ && observed_device_revision_ != device_->revision()) {
        observed_device_revision_ = device_->revision();
        mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_SETTINGS);
    }
    if (settings_manager_ &&
        (observed_settings_refresh_pending_ !=
             settings_manager_->refresh_pending() ||
         observed_settings_revision_ != settings_manager_->revision())) {
        mark_snapshots_dirty(SNAPSHOT_SETTINGS);
    }

    if (web_console_.storage_output_pending()) {
        StringPrint capture(AC_FILE_LOG_TAIL_READ_CHUNK);
        web_console_.poll_pending(capture);
        if (capture.text().length()) append_console_log(capture.text());
    }

    drain_commands();
    if (checkpoint) checkpoint("web_ui.commands");
    enforce_sse_limits();
    if (checkpoint) checkpoint("web_ui.sse_limits");
    poll_live_stream();
    if (checkpoint) checkpoint("web_ui.live");
    publish_snapshots(false, realtime_active, checkpoint);
    if (checkpoint) checkpoint("web_ui.snapshots");

    if (!events_ || events_->count() == 0) return;
    const bool push_requested = sse_push_requested_;
    if (!push_requested &&
        static_cast<int32_t>(millis() - last_sse_push_ms_) <
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS)) {
        if (checkpoint) checkpoint("web_ui.sse_idle");
        return;
    }
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) {
        sse_push_requested_ = push_requested;
        if (checkpoint) checkpoint("web_ui.sse_lock");
        return;
    }
    last_sse_push_ms_ = millis();
    sse_push_requested_ = false;
    const uint32_t event_id = millis();
    bool sse_backpressure = false;
    if (send_sse_to_clients(cached_status_json_.c_str(), "status", event_id,
                            true) == SseSendResult::Failed) {
        sse_backpressure = true;
    }
    if (send_sse_to_clients(cached_stream_json_.c_str(), "stream", event_id,
                            false) == SseSendResult::Failed) {
        sse_backpressure = true;
    }
    if (console_sse_seq_ != console_seq_) {
        // Console output can be chatty during RPC activity, so it shares the
        // same throttled SSE cadence as status/stream updates.
        const uint64_t begin = console_log_begin_pos();
        const uint64_t end = console_log_write_pos_;
        const bool reset = console_sse_reset_pending_ ||
                           console_sse_pos_ < begin ||
                           console_sse_pos_ > end;
        const uint64_t from = reset ? begin : console_sse_pos_;
        const size_t payload_len =
            static_cast<size_t>(end > from ? end - from : 0);
        LargeTextBuffer console_json;
        console_json.reserve(json_escaped_capacity(payload_len));
        build_console_sse_json(console_json);
        const SseSendResult console_result =
            console_json.overflowed()
                ? SseSendResult::Failed
                : send_sse_to_clients(console_json.c_str(), "console",
                                      event_id, false);
        if (console_result == SseSendResult::Failed) {
            sse_backpressure = true;
        } else if (console_result == SseSendResult::Sent) {
            note_console_sse_sent();
        }
    }
    xSemaphoreGive(cache_mutex_);
    if (checkpoint) checkpoint("web_ui.sse_send");
    if (sse_backpressure) {
        sse_enforce_needed_ = true;
        enforce_sse_limits();
        if (checkpoint) checkpoint("web_ui.sse_backpressure");
    }
}

void WebUI::handle_sse_connect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    bool stored = false;
    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client && !ref.client->connected()) {
                ref.client = nullptr;
                ref.connected_ms = 0;
                ref.last_status_ms = 0;
            }
        }
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client == client) {
                stored = true;
                break;
            }
        }
        if (!stored) {
            for (SseClientRef &ref : sse_clients_) {
                if (ref.client) continue;
                ref.client = client;
                ref.connected_ms = millis();
                ref.last_status_ms = 0;
                stored = true;
                break;
            }
        }
        sse_enforce_needed_ = true;
        xSemaphoreGive(sse_mutex_);
    }

    if (!stored) {
        client->close();
    }
}

void WebUI::handle_sse_disconnect(AsyncEventSourceClient *client) {
    if (!client || !sse_mutex_) return;

    if (xSemaphoreTake(sse_mutex_, portMAX_DELAY) == pdTRUE) {
        for (SseClientRef &ref : sse_clients_) {
            if (ref.client != client) continue;
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
            break;
        }
        xSemaphoreGive(sse_mutex_);
    }
}

void WebUI::enforce_sse_limits() {
    if (!sse_mutex_) return;

    while (true) {
        AsyncEventSourceClient *drop = nullptr;
        size_t connected_count = 0;
        size_t worst_pending = 0;
        uint32_t oldest_ms = 0;

        if (xSemaphoreTake(sse_mutex_, 0) != pdTRUE) return;
        for (SseClientRef &ref : sse_clients_) {
            AsyncEventSourceClient *client = ref.client;
            if (!client) continue;
            if (!client->connected()) {
                ref.client = nullptr;
                ref.connected_ms = 0;
                ref.last_status_ms = 0;
                continue;
            }

            connected_count++;
            const size_t pending = client->packetsWaiting();
            const bool worse_pending = pending > worst_pending;
            const bool older_tie =
                pending == worst_pending &&
                (!oldest_ms ||
                 static_cast<int32_t>(ref.connected_ms - oldest_ms) < 0);
            if (!drop || worse_pending || older_tie) {
                drop = client;
                worst_pending = pending;
                oldest_ms = ref.connected_ms;
            }
        }

        const bool overflow = connected_count > AC_WEB_SSE_CLIENTS_MAX;
        const bool slow = worst_pending > AC_WEB_SSE_CLIENT_PENDING_HARD_MAX;
        if (!overflow && !slow) {
            sse_enforce_needed_ = false;
            xSemaphoreGive(sse_mutex_);
            return;
        }
        xSemaphoreGive(sse_mutex_);

        if (!drop) return;
        Log::logf(CAT_GENERAL, LOG_DEBUG,
                  "[WEB] closing SSE client count=%u pending=%u\n",
                  static_cast<unsigned>(connected_count),
                  static_cast<unsigned>(worst_pending));
        drop->close();
    }
}

size_t WebUI::healthy_sse_client_count() {
    if (!sse_mutex_) return 0;
    size_t count = 0;
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return 0;
    }
    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
            continue;
        }
        if (client->packetsWaiting() <= AC_WEB_SSE_CLIENT_PENDING_MAX) {
            count++;
        }
    }
    xSemaphoreGive(sse_mutex_);
    return count;
}

WebUI::SseSendResult WebUI::send_sse_to_clients(const char *payload,
                                                const char *event,
                                                uint32_t id,
                                                bool status_heartbeat) {
    if (!payload || !event || !sse_mutex_) return SseSendResult::Skipped;

    const uint32_t now = millis();
    SseSendResult result = SseSendResult::Skipped;
    if (xSemaphoreTake(sse_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return SseSendResult::Failed;
    }

    for (SseClientRef &ref : sse_clients_) {
        AsyncEventSourceClient *client = ref.client;
        if (!client) continue;
        if (!client->connected()) {
            ref.client = nullptr;
            ref.connected_ms = 0;
            ref.last_status_ms = 0;
            continue;
        }

        const size_t pending = client->packetsWaiting();
        if (pending > AC_WEB_SSE_CLIENT_PENDING_MAX) {
            if (!status_heartbeat) continue;
            if (ref.last_status_ms &&
                static_cast<int32_t>(now - ref.last_status_ms) <
                    static_cast<int32_t>(AC_WEB_SSE_BACKPRESSURE_STATUS_MS)) {
                continue;
            }
        }

        if (!client->send(payload, event, id)) {
            result = SseSendResult::Failed;
            continue;
        }
        if (result != SseSendResult::Failed) result = SseSendResult::Sent;
        if (status_heartbeat) ref.last_status_ms = now;
    }

    xSemaphoreGive(sse_mutex_);
    return result;
}

void WebUI::poll_live_stream() {
    const size_t clients = healthy_sse_client_count();
    const bool live_needed = live_view_requested(millis()) && clients > 0;
    if (sink_manager_) sink_manager_->set_live_chart_enabled(live_needed);
    if (!live_needed) {
        if (sink_manager_) sink_manager_->clear_live_chart_batch();
        live_last_send_ms_ = 0;
        return;
    }
    send_live_batch(millis());
}

bool WebUI::live_view_requested(uint32_t now_ms) {
    if (!live_view_mutex_) return false;
    bool active = false;
    if (xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
        return false;
    }
    for (LiveViewLease &lease : live_view_leases_) {
        if (!lease.client_hash) continue;
        if (static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
            lease = {};
            continue;
        }
        active = true;
    }
    xSemaphoreGive(live_view_mutex_);
    return active;
}

void WebUI::send_live_batch(uint32_t now_ms) {
    if (!events_ || !sink_manager_) return;
    const LiveChartRuntimeStatus &live = sink_manager_->live_chart_status();
    const bool has_samples =
        live.pressure.count || live.flow.count || live.leak.count ||
        live.inspiratory_pressure.count ||
        live.expiratory_pressure.count ||
        live.spo2.count || live.pulse.count;
    const bool interval_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_LIVE_PUSH_INTERVAL_MS);
    const bool heartbeat_due =
        static_cast<int32_t>(now_ms - live_last_send_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    if (has_samples && !interval_due) return;
    if (!has_samples && !live.state_dirty && !heartbeat_due) return;
    if (healthy_sse_client_count() == 0) {
        sink_manager_->clear_live_chart_batch();
        return;
    }

    live_json_ = "{";
    json_add_int(live_json_, "seq", static_cast<long>(++live_seq_), false);
    json_add_bool(live_json_, "active", live.desired);
    json_add_bool(live_json_, "attached", live.attached);
    json_add_int(live_json_, "frames", static_cast<long>(live.frames));
    json_add_int(live_json_, "drops", static_cast<long>(live.drops));
    json_add_int(live_json_, "attach_failures",
                 static_cast<long>(live.attach_failures));
    if (live.last_frame_ms) {
        json_add_int(live_json_, "last_age_ms", now_ms - live.last_frame_ms);
    } else {
        live_json_ += ",\"last_age_ms\":null";
    }
    json_add_string(live_json_, "last_error", live.last_error);
    live_json_ += ",\"samples\":{";
    append_live_series(live_json_, "pressure", live.pressure, false);
    append_live_series(live_json_, "flow", live.flow);
    append_live_series(live_json_, "leak", live.leak);
    append_live_series(live_json_, "inspiratory_pressure",
                       live.inspiratory_pressure);
    append_live_series(live_json_, "expiratory_pressure",
                       live.expiratory_pressure);
    append_live_series(live_json_, "spo2", live.spo2);
    append_live_series(live_json_, "pulse", live.pulse);
    live_json_ += "}}";

    if (send_sse_to_clients(live_json_.c_str(), "live", now_ms,
                            false) == SseSendResult::Failed) {
        sse_enforce_needed_ = true;
    }
    live_last_send_ms_ = now_ms;
    sink_manager_->mark_live_chart_sent();
}

void WebUI::handle_event(const RpcEvent &event) {
    if (event.kind == RpcEventKind::BootNotification) {
        mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_SETTINGS);
        if (events_ &&
            send_sse_to_clients("{}", "device_boot", millis(),
                                true) == SseSendResult::Failed) {
            sse_enforce_needed_ = true;
        }
    }

    if (!ManagementConsole::event_has_output(event)) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.handle_event(capture, event);
    if (!capture.text().length()) return;
    append_console_log(capture.text());
}

void WebUI::append_console_log(const String &text) {
    if (!text.length()) return;
    reserve_console_log();
    const char *data = text.c_str();
    const size_t original_len = text.length();
    size_t len = original_len;
    if (console_log_ && console_log_capacity_) {
        if (len >= console_log_capacity_) {
            data += len - console_log_capacity_;
            len = console_log_capacity_;
            console_log_start_ = 0;
            console_log_length_ = len;
            memcpy(console_log_, data, len);
        } else {
            const size_t free_space =
                console_log_capacity_ - console_log_length_;
            const size_t overflow = len > free_space ? len - free_space : 0;
            if (overflow) {
                console_log_start_ =
                    (console_log_start_ + overflow) % console_log_capacity_;
                console_log_length_ -= overflow;
            }

            size_t write_at =
                (console_log_start_ + console_log_length_) %
                console_log_capacity_;
            size_t first = console_log_capacity_ - write_at;
            if (first > len) first = len;
            memcpy(console_log_ + write_at, data, first);
            if (len > first) {
                memcpy(console_log_, data + first, len - first);
            }
            console_log_length_ += len;
        }
    }
    console_log_write_pos_ += original_len;
    console_seq_++;
}

void WebUI::reserve_console_log() {
    if (console_log_ || AC_WEB_CONSOLE_LOG_MAX == 0) return;
    console_log_ = static_cast<char *>(
        Memory::alloc_large(AC_WEB_CONSOLE_LOG_MAX));
    if (console_log_) {
        console_log_capacity_ = AC_WEB_CONSOLE_LOG_MAX;
    }
}

void WebUI::clear_console_log() {
    Memory::free(console_log_);
    console_log_ = nullptr;
    console_log_capacity_ = 0;
    console_log_start_ = 0;
    console_log_length_ = 0;
    console_log_write_pos_++;
    console_seq_++;
    console_sse_reset_pending_ = true;
}

uint64_t WebUI::console_log_begin_pos() const {
    return console_log_write_pos_ - console_log_length_;
}

void WebUI::append_console_log_json_range(LargeTextBuffer &json,
                                          uint64_t from,
                                          uint64_t to) const {
    if (!console_log_ || !console_log_capacity_ || from >= to) return;
    const uint64_t begin = console_log_begin_pos();
    if (from < begin) from = begin;
    if (to > console_log_write_pos_) to = console_log_write_pos_;
    if (from >= to) return;

    const size_t offset = static_cast<size_t>(from - begin);
    const size_t len = static_cast<size_t>(to - from);
    const size_t read_at =
        (console_log_start_ + offset) % console_log_capacity_;
    size_t first = console_log_capacity_ - read_at;
    if (first > len) first = len;
    append_json_escaped(json, console_log_ + read_at, first);
    if (len > first) {
        append_json_escaped(json, console_log_, len - first);
    }
}

void WebUI::build_console_json(LargeTextBuffer &json) const {
    const uint64_t begin = console_log_begin_pos();
    const uint64_t end = console_log_write_pos_;
    json = "{";
    json_add_uint64(json, "seq", console_seq_, false);
    json_add_uint64(json, "begin", begin);
    json_add_uint64(json, "end", end);
    json += ",\"log\":\"";
    append_console_log_json_range(json, begin, end);
    json += "\"}";
}

void WebUI::build_console_sse_json(LargeTextBuffer &json) const {
    const uint64_t begin = console_log_begin_pos();
    const uint64_t end = console_log_write_pos_;
    const bool reset = console_sse_reset_pending_ ||
                       console_sse_pos_ < begin ||
                       console_sse_pos_ > end;

    json = "{";
    json_add_uint64(json, "seq", console_seq_, false);
    if (reset) {
        json_add_bool(json, "reset", true);
        json_add_uint64(json, "begin", begin);
        json_add_uint64(json, "end", end);
        json += ",\"log\":\"";
        append_console_log_json_range(json, begin, end);
    } else {
        json_add_uint64(json, "from", console_sse_pos_);
        json_add_uint64(json, "to", end);
        json += ",\"append\":\"";
        append_console_log_json_range(json, console_sse_pos_, end);
    }
    json += "\"}";
}

void WebUI::note_console_sse_sent() {
    console_sse_seq_ = console_seq_;
    console_sse_pos_ = console_log_write_pos_;
    console_sse_reset_pending_ = false;
}

void WebUI::send_console_snapshot(AsyncWebServerRequest *request) const {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache busy\"}");
        return;
    }
    LargeTextBuffer json;
    json.reserve(json_escaped_capacity(console_log_length_));
    build_console_json(json);
    if (json.overflowed()) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"console alloc\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    xSemaphoreGive(cache_mutex_);
    request->send(response);
}

void WebUI::mark_snapshots_dirty(uint16_t mask) {
    snapshots_dirty_mask_ |= mask;
    if (mask & (SNAPSHOT_STATUS | SNAPSHOT_OTA | SNAPSHOT_RESMED_OTA)) {
        request_sse_push();
    }
}

void WebUI::request_sse_push() {
    sse_push_requested_ = true;
}

void WebUI::send_cached_settings(AsyncWebServerRequest *request,
                                 int requested_mode,
                                 bool refresh_requested) {
    bool mismatch = false;
    bool has_cached = false;
    bool refresh_queued = refresh_requested;
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"valid\":false,\"refresh_queued\":true,"
                      "\"settings\":[]}");
        return;
    }
    refresh_queued = refresh_queued || observed_settings_refresh_pending_;

    const int active_mode =
        requested_mode < 0
            ? active_settings_mode(device_ ? &device_->state() : nullptr,
                                   settings_manager_)
            : -1;
    const bool explicit_mode_mismatch =
        requested_mode >= 0 &&
        (requested_settings_mode_ != requested_mode ||
         cached_settings_mode_ != requested_mode);
    const bool active_mode_mismatch =
        requested_mode < 0 &&
        (requested_settings_mode_ >= 0 ||
         (active_mode >= 0 && cached_settings_mode_ != active_mode));

    if (explicit_mode_mismatch || active_mode_mismatch) {
        requested_settings_mode_ = requested_mode;
        mark_snapshots_dirty(SNAPSHOT_SETTINGS);
        mismatch = true;
    } else {
        has_cached = !refresh_queued && cached_settings_json_.length() > 0;
    }

    if (!mismatch && has_cached) {
        AsyncResponseStream *response =
            request->beginResponseStream("application/json");
        if (!response) {
            xSemaphoreGive(cache_mutex_);
            request->send(503, "application/json",
                          "{\"valid\":false,\"error\":\"response alloc\"}");
            return;
        }
        response->write(
            reinterpret_cast<const uint8_t *>(cached_settings_json_.c_str()),
            cached_settings_json_.length());
        xSemaphoreGive(cache_mutex_);
        request->send(response);
        return;
    }
    xSemaphoreGive(cache_mutex_);

    const String placeholder =
        settings_placeholder_json(refresh_queued, mismatch);
    request->send(200, "application/json", placeholder);
}

String WebUI::queued_json(const char *result) const {
    String json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_bool(json, "queued", true);
    json_add_string(json, "result", result ? result : "queued");
    json += '}';
    return json;
}

void WebUI::send_cached(AsyncWebServerRequest *request,
                        const LargeTextBuffer &json) const {
    if (!cache_mutex_ ||
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) != pdTRUE) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"cache busy\"}");
        return;
    }
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        xSemaphoreGive(cache_mutex_);
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    xSemaphoreGive(cache_mutex_);
    request->send(response);
}

void WebUI::send_config_json(AsyncWebServerRequest *request,
                             const char *section) const {
    if (!web_config_section_known(section)) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"unknown_section\"}");
        return;
    }

    LargeTextBuffer json;
    if (!json.reserve(web_config_json_reserve(section))) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"config_alloc\"}");
        return;
    }
    build_config_json(json, section);
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"config_overflow\"}");
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

void WebUI::send_config_schema_json(AsyncWebServerRequest *request) const {
    LargeTextBuffer json;
    if (!json.reserve(WEB_CONFIG_SCHEMA_JSON_RESERVE)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"schema_alloc\"}");
        return;
    }
    build_config_schema_json(json);
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"schema_overflow\"}");
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

void WebUI::send_config_update(AsyncWebServerRequest *request) {
    JsonDocument doc;
    std::string body;
    if (!parse_body_copy(request, doc, body)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad json\"}");
        return;
    }
    WebCommand queued;
    queued.kind = WebCommandConfigUpdate;
    queued.body = std::move(body);
    send_queue_result(request, enqueue_command(std::move(queued)));
}

void WebUI::send_report_result(AsyncWebServerRequest *request) const {
    if (!report_manager_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report unavailable\"}");
        return;
    }
    if (!request->hasArg("night")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing night\"}");
        return;
    }

    uint64_t night_start_ms = 0;
    if (!request_uint64_arg(request, "night", night_start_ms)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad night\"}");
        return;
    }

    ScopedReportHttpTimer timer("/api/report/result");

    String inm;
    if (request->hasHeader("If-None-Match")) {
        inm = request->getHeader("If-None-Match")->value();
        strip_http_etag_quotes(inm);
    }

    LargeTextBuffer json;
    if (!json.reserve(WEB_REPORT_RESULT_JSON_RESERVE)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report_alloc\"}");
        return;
    }
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    const ReportManager::ResultRead st = report_manager_->read_result_by_start(
        night_start_ms,
        inm.c_str(),
        etag,
        sizeof(etag),
        json);

    switch (st) {
        case ReportManager::ResultRead::Ready: {
            AsyncResponseStream *response =
                request->beginResponseStream("application/json");
            if (!response) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"response alloc\"}");
                return;
            }
            if (etag[0]) {
                add_report_result_cache_headers(response, etag);
            } else {
                response->addHeader("Cache-Control", "no-store");
            }
            response->write(
                reinterpret_cast<const uint8_t *>(json.c_str()),
                json.length());
            request->send(response);
            return;
        }
        case ReportManager::ResultRead::NotModified: {
            AsyncWebServerResponse *response = request->beginResponse(304);
            if (response) {
                add_report_result_cache_headers(response, etag);
                request->send(response);
            } else {
                request->send(304);
            }
            return;
        }
        case ReportManager::ResultRead::Building:
            request->send(202, "application/json",
                          "{\"ok\":true,\"state\":\"preparing\"}");
            return;
        case ReportManager::ResultRead::QueueFull:
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"report_queue_full\"}");
            return;
        case ReportManager::ResultRead::Unavailable:
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"report_cache_unavailable\"}");
            return;
        case ReportManager::ResultRead::Busy:
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"report_cache_busy\"}");
            return;
        case ReportManager::ResultRead::NotFound:
        default:
            request->send(404, "application/json",
                          "{\"ok\":false,\"error\":\"no such night\"}");
            return;
    }
}

void WebUI::send_live_view_state(AsyncWebServerRequest *request) {
    bool active = false;
    if (request->hasArg("active")) {
        parse_bool_yesno(request->arg("active"), active);
    } else if (request->hasArg("enabled")) {
        parse_bool_yesno(request->arg("enabled"), active);
    }
    const uint32_t client_hash =
        request->hasArg("id") ? fnv1a32_string(request->arg("id")) : 1u;
    const uint32_t now_ms = millis();
    if (live_view_mutex_ &&
        xSemaphoreTake(live_view_mutex_, pdMS_TO_TICKS(20)) == pdTRUE) {
        LiveViewLease *empty = nullptr;
        LiveViewLease *oldest = nullptr;
        for (LiveViewLease &lease : live_view_leases_) {
            if (lease.client_hash == client_hash) {
                if (active) {
                    lease.expires_ms = now_ms + WEB_LIVE_VIEW_LEASE_MS;
                } else {
                    lease = {};
                }
                xSemaphoreGive(live_view_mutex_);
                request->send(200, "application/json",
                              active ? "{\"ok\":true,\"active\":true}"
                                     : "{\"ok\":true,\"active\":false}");
                return;
            }
            if (!lease.client_hash) {
                if (!empty) empty = &lease;
                continue;
            }
            if (static_cast<int32_t>(now_ms - lease.expires_ms) >= 0) {
                lease = {};
                if (!empty) empty = &lease;
                continue;
            }
            if (!oldest ||
                static_cast<int32_t>(lease.expires_ms -
                                     oldest->expires_ms) < 0) {
                oldest = &lease;
            }
        }
        if (active) {
            LiveViewLease *slot = empty ? empty : oldest;
            if (slot) {
                slot->client_hash = client_hash;
                slot->expires_ms = now_ms + WEB_LIVE_VIEW_LEASE_MS;
            }
        }
        xSemaphoreGive(live_view_mutex_);
    }
    request->send(200, "application/json",
                  active ? "{\"ok\":true,\"active\":true}"
                         : "{\"ok\":true,\"active\":false}");
}

void WebUI::send_queue_result(AsyncWebServerRequest *request,
                              bool queued,
                              const char *result) const {
    if (queued) {
        request->send(202, "application/json", queued_json(result));
        return;
    }
    request->send(503, "application/json",
                  "{\"ok\":false,\"queued\":false,"
                  "\"error\":\"web command queue full\"}");
}

bool WebUI::request_allowed_cached(AsyncWebServerRequest *request) const {
    bool required = true;
    String user;
    String password;
    String whitelist;
    if (cache_mutex_ &&
        xSemaphoreTake(cache_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
        required = cached_http_auth_required_;
        user = cached_http_user_;
        password = cached_http_password_;
        whitelist = cached_auth_whitelist_;
        xSemaphoreGive(cache_mutex_);
    }
    if (!required) return true;
    if (request && request->client() &&
        auth_whitelist_matches(request->client()->remoteIP(), whitelist)) {
        return true;
    }
    return request && request->authenticate(user.c_str(), password.c_str());
}

bool WebUI::enqueue_command(WebCommand &&command) {
    const uint8_t kind = command.kind;
    if (!command_mutex_) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue unavailable kind=%s\n",
                  web_command_name(kind));
        return false;
    }

    if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue busy kind=%s\n",
                  web_command_name(kind));
        return false;
    }
    const bool queued = command_queue_.push(std::move(command));
    xSemaphoreGive(command_mutex_);
    if (!queued) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] command queue full kind=%s\n",
                  web_command_name(kind));
    }
    return queued;
}

bool WebUI::enqueue_simple_command(uint8_t kind) {
    WebCommand command;
    command.kind = kind;
    return enqueue_command(std::move(command));
}

void WebUI::drain_commands() {
    if (!command_mutex_) return;
    for (size_t i = 0; i < AC_WEB_COMMANDS_PER_POLL; ++i) {
        WebCommand command;
        if (xSemaphoreTake(command_mutex_, pdMS_TO_TICKS(2)) != pdTRUE) {
            return;
        }
        const bool has_command = command_queue_.pop(command);
        xSemaphoreGive(command_mutex_);
        if (!has_command) break;
        execute_command(command);
    }
}

void WebUI::build_status_json(LargeTextBuffer &json,
                              PollCheckpoint checkpoint) const {
    const SystemStatusSnapshot snap = collect_system_status({
        *device_,
        *wifi_manager_,
        *app_config_,
        *time_sync_service_,
        *ota_manager_,
        *oximetry_manager_,
    }, checkpoint);
    const MemoryStatus &mem = snap.memory;
    const StorageStatus &storage = snap.storage;
    const WifiStatusSnapshot &wifi = snap.wifi;
    const As11StatusSnapshot &as11 = snap.as11;
    const OximetryRuntimeStatus &oxi = snap.oximetry;
    const TimeStatusSnapshot &time = snap.time;

    json = "{";
    json_add_string(json, "version", snap.version, false);
    json_add_string(json, "built", snap.built);
    json_add_string(json, "hostname", app_config_->data().hostname.c_str());
    json_add_int(json, "uptime", snap.uptime_s);
    json_add_int(json, "heap", static_cast<long>(mem.heap_free));
    json_add_bool(json, "psram_available", mem.psram_available);
    json_add_int(json, "psram_free", static_cast<long>(mem.psram_free));
    json_add_string(json, "storage_state",
                    Storage::state_name(storage.state));
    json_add_uint64(json, "storage_total", storage.total_bytes);
    json_add_uint64(json, "storage_used", storage.used_bytes);
    json_add_string_view(json, "wifi_state", wifi.state);
    json_add_string_view(json, "wifi_ssid", wifi.ssid);
    json_add_string(json, "wifi_ip", wifi.ip);
    json_add_int(json, "wifi_rssi", wifi.rssi);
    json_add_int(json, "wifi_channel", wifi.channel);
    json_add_string(json, "wifi_bssid", wifi.bssid);
    json_add_int(json, "wifi_profile", wifi.active_profile);
    json_add_bool(json, "wifi_roam", wifi.roaming_enabled);
    json_add_bool(json, "update_checking", snap.update.checking);
    json_add_bool(json, "update_available", snap.update.available);
    json_add_string(json, "update_version", snap.update.version);
    json_add_string_view(json, "device_name", as11.product_name);
    json_add_string_view(json, "serial", as11.serial_number);
    json_add_string_view(json, "software_id", as11.software_identifier);
    json_add_string(json, "therapy",
                    As11DeviceState::therapy_state_name(as11.therapy_state));
    json_add_string(json, "therapy_pending",
                    As11DeviceState::therapy_target_name(
                        as11.pending_therapy_target));
    json_add_string_view(json, "profile", as11.active_therapy_profile);
    char hours[16];
    motor_hours(as11.motor_run_meter, hours, sizeof(hours));
    json_add_string(json, "motor_hours", hours);
    json += ",\"oximetry\":{";
    json_add_bool(json, "enabled", oxi.enabled, false);
    json_add_string(json, "source", oximetry_source_name(oxi.source));
    json_add_string(json, "source_detail", oxi.source_detail);
    json_add_bool(json, "source_present", oxi.source_present);
    json_add_bool(json, "source_fresh", oxi.source_fresh);
    json_add_bool(json, "valid", oxi.reading.valid);
    json_add_bool(json, "contact_known", oxi.reading.contact_known);
    json_add_bool(json, "contact_present", oxi.reading.contact_present);
    if (oxi.reading.valid) {
        json_add_int(json, "spo2", oxi.reading.spo2);
        json_add_int(json, "pulse_bpm", oxi.reading.pulse_bpm);
    } else {
        json += ",\"spo2\":null,\"pulse_bpm\":null";
    }
    json_add_int(json, "source_age_ms", oxi.last_source_age_ms);
    json_add_string(json, "advertise_mode",
                    oximetry_advertise_mode_name(oxi.advertise_mode));
    json_add_bool(json, "manual_advertising_requested",
                  oxi.manual_advertising_requested);
    json_add_bool(json, "ble_available", oxi.ble_available);
    json_add_bool(json, "advertising", oxi.advertising);
    json_add_bool(json, "connected", oxi.connected);
    json_add_bool(json, "subscribed", oxi.subscribed);
    json_add_bool(json, "pairing_active", oxi.pairing_active);
    json_add_int(json, "pairing_left_ms", oxi.pairing_left_ms);
    json_add_string(json, "ble_name", oxi.ble_name);
    json_add_string(json, "ble_peer", oxi.ble_peer);
    json += '}';
    json_add_string_view(json, "device_datetime", as11.device_datetime);
    if (as11.clock_valid) {
        json_add_int(json, "device_datetime_age_ms",
                     snap.now_ms - as11.clock_sample_ms);
    } else {
        json += ",\"device_datetime_age_ms\":null";
    }
    json_add_bool(json, "resmed_time_sync_enabled",
                  time.resmed_time_sync_enabled);
    json_add_bool(json, "ntp_synced", time.ntp_synced);
    json_add_bool(json, "esp_time_valid", time.esp_time_valid);
    json_add_string_view(json, "esp_time_source", time.esp_time_source);
    json_add_string(json, "esp_datetime", time.esp_datetime);
    json += '}';
}

void WebUI::build_oximetry_sensors_json(LargeTextBuffer &json) const {
    const OximetryRuntimeStatus runtime = oximetry_manager_->runtime_status();
    const OximetrySensorStatus sensor = oximetry_manager_->sensor_status();
    OximetrySensorDevice oxi_scan[AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS];
    OximetrySensorDevice oxi_known[AC_OXIMETRY_SENSOR_MAX_KNOWN];
    const size_t oxi_scan_count =
        oximetry_manager_->sensor_scan_results(
            oxi_scan, AC_OXIMETRY_SENSOR_MAX_SCAN_RESULTS);
    const size_t oxi_known_count =
        oximetry_manager_->known_sensors(oxi_known,
                                         AC_OXIMETRY_SENSOR_MAX_KNOWN);

    json = "{";
    json_add_bool(json, "enabled", runtime.enabled, false);
    json_add_bool(json, "ble_available", runtime.ble_available);
    json_add_string(json, "sensor_state",
                    oximetry_sensor_state_name(sensor.sensor_state));
    json_add_bool(json, "sensor_task_started", sensor.sensor_task_started);
#if AC_STACK_PROFILE_ENABLED
    json_add_int(json, "sensor_task_stack_free",
                 static_cast<long>(
                     sensor.sensor_task_stack_high_water_bytes));
#endif
    json_add_bool(json, "sensor_scanning", sensor.sensor_scanning);
    json_add_bool(json, "sensor_connected", sensor.sensor_connected);
    json_add_int(json, "sensor_known_count", sensor.sensor_known_count);
    json_add_int(json, "sensor_scan_count", sensor.sensor_scan_count);
    json_add_int(json, "sensor_scan_generation",
                 sensor.sensor_scan_generation);
    json_add_string(json, "sensor_peer", sensor.sensor_peer);
    json_add_string(json, "sensor_name", sensor.sensor_name);
    json += ",\"sensor_scan_results\":[";
    for (size_t i = 0; i < oxi_scan_count; ++i) {
        if (i) json += ',';
        append_oximetry_sensor(json, oxi_scan[i], i, true);
    }
    json += "],\"sensor_known\":[";
    for (size_t i = 0; i < oxi_known_count; ++i) {
        if (i) json += ',';
        append_oximetry_sensor(json, oxi_known[i], i, false);
    }
    json += "]}";
}

void WebUI::send_report_summary(AsyncWebServerRequest *request) const {
    if (!report_manager_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report unavailable\"}");
        return;
    }
    ScopedReportHttpTimer timer("/api/report/summary");

    LargeTextBuffer json;
    if (!json.reserve(WEB_REPORT_SUMMARY_JSON_RESERVE)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"summary_alloc\"}");
        return;
    }
    report_manager_->build_summary_json(json);
    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    response->write(
        reinterpret_cast<const uint8_t *>(json.c_str()),
        json.length());
    request->send(response);
}

void WebUI::send_report_chunks(AsyncWebServerRequest *request) const {
    if (!report_manager_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report unavailable\"}");
        return;
    }

    size_t offset = 0;
    size_t limit = 32;
    if (!request_size_arg_limited(request, "offset", 0, 65535, offset) ||
        !request_size_arg_limited(request, "limit", 32, 128, limit)) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad range\"}");
        return;
    }

    LargeTextBuffer json;
    json.reserve(1024 + limit * 192);
    report_manager_->build_result_chunks_json(json, offset, limit);
    if (json.overflowed()) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"chunks alloc\"}");
        return;
    }

    AsyncResponseStream *response =
        request->beginResponseStream("application/json");
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                    json.length());
    request->send(response);
}

void WebUI::send_report_plot(AsyncWebServerRequest *request) const {
    if (!report_manager_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report unavailable\"}");
        return;
    }
    if (!request->hasArg("index")) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"missing index\"}");
        return;
    }
    const long index = request->arg("index").toInt();
    if (index < 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad index\"}");
        return;
    }
    ScopedReportHttpTimer timer("/api/report/plot", index);

    String version;
    if (request->hasArg("v")) version = request->arg("v");
    int64_t range_from_ms = 0;
    int64_t range_to_ms = 0;
    const bool range_requested =
        request->hasArg("from") || request->hasArg("to");
    if (range_requested) {
        if (!request_int64_arg(request, "from", range_from_ms) ||
            !request_int64_arg(request, "to", range_to_ms) ||
            range_to_ms <= range_from_ms) {
            request->send(400,
                          "application/json",
                          "{\"ok\":false,\"error\":\"bad range\"}");
            return;
        }

        int64_t normalized_from_ms = 0;
        int64_t normalized_to_ms = 0;
        if (!normalize_report_range_tiles(
                range_from_ms,
                range_to_ms,
                AC_REPORT_RANGE_TILE_MS,
                AC_REPORT_RANGE_PLOT_MAX_WINDOW_MS,
                normalized_from_ms,
                normalized_to_ms)) {
            request->send(400,
                          "application/json",
                          "{\"ok\":false,\"error\":\"range too wide\"}");
            return;
        }

        range_from_ms = normalized_from_ms;
        range_to_ms = normalized_to_ms;
    }
    char request_http_etag[WEB_REPORT_PLOT_HTTP_ETAG_MAX] = {};
    if (version.length()) {
        format_report_plot_http_etag(version.c_str(),
                                     range_requested,
                                     range_from_ms,
                                     range_to_ms,
                                     request_http_etag,
                                     sizeof(request_http_etag));
    }

    if (request_http_etag[0] && request->hasHeader("If-None-Match")) {
        String inm = request->getHeader("If-None-Match")->value();
        strip_http_etag_quotes(inm);
        if (inm == request_http_etag) {
            AsyncWebServerResponse *response = request->beginResponse(304);
            if (response) {
                add_report_plot_cache_headers(response, request_http_etag);
                request->send(response);
            } else {
                request->send(304);
            }
            return;
        }
    }
    std::shared_ptr<ReportSpoolBuffer> payload;
    ReportManager::PlotRead st;
    char plot_etag[AC_REPORT_RESULT_ETAG_MAX] = {};
    if (range_requested) {
        st = report_manager_->read_plot_range(static_cast<size_t>(index),
                                              version.c_str(),
                                              plot_etag,
                                              sizeof(plot_etag),
                                              range_from_ms,
                                              range_to_ms,
                                              payload);
    } else {
        st = report_manager_->read_plot(static_cast<size_t>(index),
                                        version.c_str(),
                                        plot_etag,
                                        sizeof(plot_etag),
                                        payload);
    }
    if (st == ReportManager::PlotRead::NotFound) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"no such night\"}");
        return;
    }
    if (st == ReportManager::PlotRead::Error) {
        request->send(500, "application/json",
                      "{\"ok\":false,\"error\":\"report_plot_failed\"}");
        return;
    }
    if (st == ReportManager::PlotRead::Stale) {
        char body[96];
        snprintf(body,
                 sizeof(body),
                 "{\"ok\":false,\"error\":\"stale_plot\",\"etag\":\"%s\"}",
                 plot_etag);
        AsyncWebServerResponse *response =
            request->beginResponse(409, "application/json", body);
        if (response) {
            response->addHeader("Cache-Control", "no-store");
            request->send(response);
        } else {
            request->send(409, "application/json", body);
        }
        return;
    }
    if (st == ReportManager::PlotRead::Empty) {
        AsyncWebServerResponse *response = request->beginResponse(204);
        if (response) {
            char http_etag[WEB_REPORT_PLOT_HTTP_ETAG_MAX] = {};
            format_report_plot_http_etag(plot_etag,
                                         range_requested,
                                         range_from_ms,
                                         range_to_ms,
                                         http_etag,
                                         sizeof(http_etag));
            add_report_plot_cache_headers(response, http_etag);
            request->send(response);
        } else {
            request->send(204);
        }
        return;
    }
    if (st == ReportManager::PlotRead::QueueFull) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report_queue_full\"}");
        return;
    }
    if (st == ReportManager::PlotRead::Unavailable) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report_cache_unavailable\"}");
        return;
    }
    if (st == ReportManager::PlotRead::Busy) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"report_cache_busy\"}");
        return;
    }
    if (st == ReportManager::PlotRead::Building || !payload ||
        payload->size() == 0) {
        AsyncWebServerResponse *response = request->beginResponse(
            202, "application/json", "{\"ok\":true,\"state\":\"preparing\"}");
        if (response) {
            response->addHeader("Cache-Control", "no-store");
            request->send(response);
        } else {
            request->send(202, "application/json",
                          "{\"ok\":true,\"state\":\"preparing\"}");
        }
        return;
    }
    AsyncWebServerResponse *response = request->beginResponse(
        "application/octet-stream",
        payload->size(),
        [payload](uint8_t *buffer, size_t max_len, size_t offset) -> size_t {
            if (!buffer || offset >= payload->size()) return 0;
            const size_t remaining = payload->size() - offset;
            const size_t n = remaining < max_len ? remaining : max_len;
            memcpy(buffer, payload->data() + offset, n);
            return n;
        });
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response alloc\"}");
        return;
    }
    char http_etag[WEB_REPORT_PLOT_HTTP_ETAG_MAX] = {};
    format_report_plot_http_etag(plot_etag,
                                 range_requested,
                                 range_from_ms,
                                 range_to_ms,
                                 http_etag,
                                 sizeof(http_etag));
    add_report_plot_cache_headers(response, http_etag);
    response->addHeader("Accept-Ranges", "none");
    request->send(response);
}

void WebUI::send_storage_list(AsyncWebServerRequest *request) const {
    if (BackgroundWorker *w = background_worker()) w->note_activity();

    if (!storage_browser_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"list_unavailable\"}");
        return;
    }

    const String path = request->hasArg("path") ? request->arg("path") : "/";
    size_t offset = 0;
    size_t limit = kStorageListDefaultLimit;
    if (!request_size_arg_limited(request, "offset", 0, 65535, offset) ||
        !request_size_arg_limited(request, "limit", kStorageListDefaultLimit,
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
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
        return;
    }

    const bool refresh = request_bool_arg_default(request, "refresh", false);
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

void WebUI::send_storage_download(AsyncWebServerRequest *request) const {
    if (BackgroundWorker *w = background_worker()) w->note_activity();

    if (!storage_browser_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"download_unavailable\"}");
        return;
    }
    if (!storage_read_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
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
    if (!request_size_arg_limited(request, "id", 0, 0xffffffffu, id_arg) ||
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

void WebUI::send_file_log_tail(AsyncWebServerRequest *request, size_t lines) {
    if (!request || !storage_read_ || !Log::filelog_enabled()) {
        if (request) {
            request->send(404, "text/plain", "file log unavailable\n");
        }
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

    std::shared_ptr<PendingPreparedReadRef> ref =
        std::make_shared<PendingPreparedReadRef>();
    if (!ref) {
        (void)storage_read_->abandon(submission.ticket);
        request->send(503, "text/plain", "response alloc\n");
        return;
    }
    ref->port = storage_read_;
    ref->ticket = submission.ticket;

    AsyncWebServerResponse *response = request->beginChunkedResponse(
        "text/plain",
        [ref](uint8_t *buffer, size_t capacity, size_t offset) -> size_t {
            return ref->fill(buffer, capacity, offset);
        });
    if (!response) {
        request->send(503, "text/plain", "response alloc\n");
        return;
    }

    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void WebUI::send_storage_archive_start(AsyncWebServerRequest *request) const {
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
        StorageJobGate gate(request, storage_job_mutex_);
        if (!gate.locked()) return;
        if (!storage_heavy_request_available(
                request, session_manager_,
                device_ ? &device_->state() : nullptr)) {
            return;
        }
        if (!storage_jobs_available(request,
                                    storage_archive_,
                                    storage_delete_,
                                    storage_sync_job_,
                                    sleephq_sync_job_)) {
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
    release_request_body(request);
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
    StorageJobGate gate(request, storage_job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_,
                                storage_sync_job_,
                                sleephq_sync_job_)) {
        return;
    }

    const bool recursive =
        request_bool_arg_default(request, "recursive", true);
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

void WebUI::send_storage_archive_status(AsyncWebServerRequest *request) const {
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

void WebUI::send_storage_archive_download(AsyncWebServerRequest *request) const {
    if (!storage_archive_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"archive_unavailable\"}");
        return;
    }
    size_t id_arg = 0;
    if (!request_size_arg_limited(request, "id", 0, 0xffffffffu, id_arg) ||
        id_arg == 0) {
        request->send(400, "application/json",
                      "{\"ok\":false,\"error\":\"bad_id\"}");
        return;
    }
    StorageJobGate gate(request, storage_job_mutex_);
    if (!gate.locked()) return;
    if (!storage_read_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
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

    AsyncWebServerResponse *response = new (std::nothrow) AsyncPreparedResponse(
        "application/zip",
        static_cast<size_t>(archive_size),
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
    char disposition[128];
    snprintf(disposition, sizeof(disposition),
             "attachment; filename=\"%s\"",
             filename[0] ? filename : "archive.zip");
    response->addHeader("Content-Disposition", disposition);
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Accept-Ranges", "none");
    request->send(response);
}

void WebUI::send_storage_delete_start(AsyncWebServerRequest *request) const {
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
    StorageJobGate gate(request, storage_job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_,
                                storage_sync_job_,
                                sleephq_sync_job_)) {
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

void WebUI::send_storage_delete_status(AsyncWebServerRequest *request) const {
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

void WebUI::send_storage_sync_start(AsyncWebServerRequest *request) const {
    if (!storage_sync_job_ || !export_coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }
    StorageJobGate gate(request, storage_job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_,
                                nullptr,
                                nullptr)) {
        return;
    }
    if (!export_coordinator_->request_smb_sync()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }
    request->send(202, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void WebUI::send_storage_sync_verify(AsyncWebServerRequest *request) const {
    if (!storage_sync_job_ || !export_coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }
    StorageJobGate gate(request, storage_job_mutex_);
    if (!gate.locked()) return;
    if (!storage_heavy_request_available(
            request, session_manager_,
            device_ ? &device_->state() : nullptr)) {
        return;
    }
    if (!storage_jobs_available(request,
                                storage_archive_,
                                storage_delete_,
                                nullptr,
                                nullptr)) {
        return;
    }
    if (!export_coordinator_->request_smb_verify()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }
    request->send(202, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void WebUI::send_storage_sync_status(AsyncWebServerRequest *request) const {
    if (!storage_sync_job_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sync_unavailable\"}");
        return;
    }
    const StorageSyncStatus status = storage_sync_job_->status();
    LargeTextBuffer json;
    json.reserve(1536);
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state",
                    storage_sync_state_name(status.state));
    json_add_bool(json, "enabled", status.enabled);
    json_add_bool(json, "configured", status.configured);
    json_add_string(json, "endpoint",
                    app_config_->data().smb_endpoint.c_str());
    json_add_bool(json, "network_available", status.network_available);
    json_add_bool(json, "pending", status.pending);
    json_add_bool(json, "last_run_verify", status.last_run_verify);
    json_add_bool(json, "last_run_reconcile",
                  status.last_run_reconcile);
    json_add_string(json, "pending_reason", status.pending_reason);
    json_add_string(json, "error", status.last_error);
    json_add_string(json, "current_path", status.current_path);
    json_add_int(json, "files_seen",
                 static_cast<long>(status.files_seen));
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
    json_add_uint64(json, "last_failure_epoch",
                    status.last_failure_epoch);
    json_add_string(json, "last_failure_error",
                    status.last_failure_error);
    json_add_int(json, "started_ms",
                 static_cast<long>(status.started_ms));
    json_add_int(json, "updated_ms",
                 static_cast<long>(status.updated_ms));
    json_add_int(json, "retry_due_ms",
                 static_cast<long>(status.retry_due_ms));
    json_add_int(json, "retry_attempt",
                 static_cast<long>(status.retry_attempt));
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

void WebUI::send_sleephq_sync_start(AsyncWebServerRequest *request) const {
    if (!sleephq_sync_job_ || !export_coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }
    if (!export_coordinator_->request_sleephq_sync()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"sync_not_ready\"}");
        return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void WebUI::send_sleephq_sync_check(AsyncWebServerRequest *request) const {
    if (!sleephq_sync_job_ || !export_coordinator_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }
    if (!export_coordinator_->request_sleephq_check()) {
        request->send(409, "application/json",
                      "{\"ok\":false,\"error\":\"check_not_ready\"}");
        return;
    }
    request->send(200, "application/json",
                  "{\"ok\":true,\"queued\":true}");
}

void WebUI::send_sleephq_sync_status(AsyncWebServerRequest *request) const {
    if (!sleephq_sync_job_) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"sleephq_unavailable\"}");
        return;
    }
    const SleepHqSyncStatus status = sleephq_sync_job_->status();
    LargeTextBuffer json;
    json.reserve(WEB_JSON_RESERVE_MEDIUM);
    json = "{";
    append_sleephq_sync_json(json, status, app_config_->data(), true);
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

void WebUI::build_stream_json(LargeTextBuffer &json) const {
    const StreamBroker &stream = *stream_;
    const LiveChartRuntimeStatus &live = sink_manager_->live_chart_status();

    json = "{";
    json_add_bool(json, "desired", stream.desired_active(), false);
    json_add_bool(json, "subscribed", stream.actual_active());
    json_add_bool(json, "pending_start", stream.pending_start());
    json_add_bool(json, "pending_stop", stream.pending_stop());
    json_add_bool(json, "error", stream.error());
    json_add_string(json, "error_command",
                    stream_command_name(stream.error_command()));
    json_add_int(json, "consumers", stream.consumer_count());
    json_add_int(json, "published_payloads", stream.published_payloads());
    json_add_int(json, "fanout_targets", stream.fanout_targets());
    json_add_int(json, "fanout_drops", stream.total_queue_drops());
    json_add_int(json, "frame_pool_used", stream.frame_pool_in_use());
    json_add_int(json, "frame_pool_capacity", stream.frame_pool_capacity());
    json_add_int(json, "parse_errors", stream.parse_errors());
    json_add_int(json, "pool_exhaustions", stream.pool_exhaustions());
    json_add_int(json, "truncated_frames", stream.truncated_frames());
    json_add_bool(json, "web_live_attached", live.attached);
    json_add_int(json, "web_live_handle", live.handle);
    json_add_int(json, "web_live_frames", static_cast<long>(live.frames));
    json_add_int(json, "web_live_drops", static_cast<long>(live.drops));
    json_add_int(json, "web_live_attach_failures",
                 static_cast<long>(live.attach_failures));
    json_add_string(json, "web_live_error", live.last_error);
    json_add_int(json, "stream_id", stream.last_stream_id());
    json_add_int(json, "start_requests", stream.start_requests());
    json_add_int(json, "stop_requests", stream.stop_requests());
    json_add_int(json, "command_deferred", stream.command_deferred());
    json_add_int(json, "command_errors", stream.command_errors());
    if (stream.last_notification_ms()) {
        json_add_int(json, "last_age_ms",
                     millis() - stream.last_notification_ms());
    } else {
        json += ",\"last_age_ms\":null";
    }
    json_add_string(json, "start_time", stream.last_start_time().c_str());
    json_add_string(json, "params", stream.params_json().c_str());
    json += ",\"consumer_slots\":[";
    bool first_consumer = true;
    for (size_t i = 0; i < AC_STREAM_CONSUMERS_MAX; ++i) {
        StreamConsumerHandle handle = static_cast<StreamConsumerHandle>(i);
        if (!stream.consumer_active(handle)) continue;
        if (!first_consumer) json += ',';
        first_consumer = false;
        json += "{";
        json_add_bool(json, "active", true, false);
        json_add_int(json, "source",
                     static_cast<unsigned>(stream.consumer_source(handle)));
        json_add_int(json, "queued", stream.consumer_queue_count(handle));
        json_add_int(json, "drops", stream.consumer_queue_drops(handle));
        json += '}';
    }
    json += "]}";
}

void WebUI::build_config_json(LargeTextBuffer &json,
                              const char *section) const {
    const AppConfigData &cfg = app_config_->data();
    json = "{";
    bool comma = false;

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!web_config_section_includes(section, field.group)) continue;

        String value;
        if (!app_config_field_get_raw_value(cfg, field, value)) continue;

        if (app_config_field_is_secret(field)) {
            char set_key[40];
            snprintf(set_key, sizeof(set_key), "%s_set", field.key);
            json_add_bool(json, set_key, value.length() > 0, comma);
            comma = true;
            json_add_string(json, field.key, "");
            comma = true;
            continue;
        }

        switch (field.type) {
            case AppConfigFieldType::Bool:
                json_add_bool(json, field.key, value == "1", comma);
                comma = true;
                break;
            case AppConfigFieldType::UInt16:
                json_add_int(json, field.key, strtol(value.c_str(), nullptr,
                                                     10), comma);
                comma = true;
                break;
            case AppConfigFieldType::String:
            case AppConfigFieldType::Enum:
            case AppConfigFieldType::LogLevel:
                json_add_string(json, field.key, value.c_str(), comma);
                comma = true;
                break;
            case AppConfigFieldType::Secret:
                break;
        }
    }
    json += '}';
}

void WebUI::build_config_schema_json(LargeTextBuffer &json) const {
    json = "{\"groups\":[";

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    AppConfigGroup current_group = AppConfigGroup::Device;
    bool group_open = false;
    bool first_group = true;
    bool first_field = true;

    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!group_open || field.group != current_group) {
            if (group_open) json += "]}";
            if (!first_group) json += ',';
            first_group = false;
            current_group = field.group;
            json += "{";
            json_add_string(json, "id", app_config_group_id(current_group),
                            false);
            json_add_string(json, "label",
                            app_config_group_label(current_group));
            json += ",\"fields\":[";
            group_open = true;
            first_field = true;
        }

        if (!first_field) json += ',';
        first_field = false;
        json += "{";
        json_add_string(json, "key", field.key, false);
        json_add_string(json, "label", field.label);
        json_add_string(json, "type",
                        web_config_field_type_name(field.type));
        json_add_bool(json, "secret", app_config_field_is_secret(field));
        json_add_bool(json, "provisionable",
                      (field.flags & AC_CONFIG_FIELD_PROVISIONABLE) != 0);
        if (field.help && field.help[0]) {
            json_add_string(json, "help", field.help);
        }
        append_config_schema_enum(json, field);
        json += "}";
    }

    if (group_open) json += "]}";
    json += "]}";
}

void WebUI::build_wifi_json(LargeTextBuffer &json) const {
    json = "{";
    json_add_string(json, "state", wifi_manager_->state_name(), false);
    json_add_string(json, "ssid", wifi_manager_->sta_ssid().c_str());
    json_add_int(json, "active", wifi_manager_->active_profile_index());
    json_add_string(json, "ip", wifi_manager_->ip().toString().c_str());
    char bssid_text[AC_WIFI_BSSID_TEXT_MAX];
    wifi_manager_->bssid(bssid_text, sizeof(bssid_text));
    json_add_string(json, "bssid", bssid_text);
    json_add_int(json, "rssi", wifi_manager_->rssi());
    json_add_int(json, "channel", wifi_manager_->channel());
    json_add_bool(json, "roam", wifi_manager_->roaming_enabled());
    json += ",\"profiles\":[";
    for (size_t i = 0; i < wifi_manager_->profile_count(); ++i) {
        const WifiProfile &profile = wifi_manager_->profile(i);
        if (i) json += ',';
        json += "{";
        json_add_string(json, "ssid", profile.ssid.c_str(), false);
        json_add_bool(json, "open", profile.password.length() == 0);
        json += '}';
    }
    json += "]}";
}

void WebUI::build_settings_json(LargeTextBuffer &json,
                                int requested_mode,
                                bool refresh_queued) const {
    const As11SettingsState &state = settings_manager_->state();
    const As11DeviceState &as11 = device_->state();
    int active_mode = state.mode_index();
    if (active_mode < 0) {
        active_mode = as11_mode_index_from_value(
            as11.active_therapy_profile());
    }
    int profile_mode = requested_mode >= 0 ? requested_mode : active_mode;
    const uint16_t supported_modes = state.supported_mode_mask();
    if (profile_mode >= 0 && supported_modes &&
        !(supported_modes & (1u << profile_mode))) {
        profile_mode = active_mode;
    }

    json = "{";
    json_add_bool(json, "valid", state.valid(), false);
    json_add_bool(json, "refresh_queued", refresh_queued);
    json_add_int(json, "supported_mode_mask", supported_modes);
    json_add_int(json, "pending_count",
                 static_cast<long>(state.pending_count()));
    json_add_string(json, "last_write_status",
                    state.last_write_status().c_str());
    if (state.last_write_ms()) {
        json_add_int(json, "last_write_age_ms",
                     millis() - state.last_write_ms());
    } else {
        json += ",\"last_write_age_ms\":null";
    }
    if (state.valid()) {
        json_add_int(json, "age_ms", millis() - state.updated_ms());
    } else {
        json += ",\"age_ms\":null";
    }
    json += ",\"settings\":[";
    size_t emitted = 0;
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (!state.setting_visible(i, profile_mode)) continue;
        if (!as11_setting_readable_via_rpc(def)) continue;

        const bool is_therapy_mode = strcmp(def.key, "MOP") == 0;
        std::string value =
            state.value(i, is_therapy_mode ? active_mode : profile_mode);
        const bool available = !value.empty() || state.pending(i);
        const bool pending = state.pending(i);
        const bool writable = as11_setting_writable_via_rpc(def);
        if (!available && !pending) continue;

        if (emitted++) json += ',';
        json += "{";
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "value", value.c_str());
        if (!available) json_add_bool(json, "available", false);
        if (!writable) json_add_bool(json, "writable", false);
        if (pending) {
            json_add_bool(json, "pending", true);
            json_add_string(json, "pending_value",
                            state.pending_value(i).c_str());
            json_add_int(json, "pending_age_ms",
                         millis() - state.pending_since_ms(i));
        }
        json += '}';
    }
    json += "]}";
}

void WebUI::build_settings_catalog_json(LargeTextBuffer &json) const {
    json = "{\"settings\":[";
    size_t emitted = 0;
    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const As11SettingDef &def = as11_setting(i);
        if (!def.mode_mask) continue;
        if (!as11_setting_readable_via_rpc(def)) continue;

        if (emitted++) json += ',';
        json += "{";
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "label", def.label);
        const std::string rpc_name = as11_setting_rpc_long_name(def);
        json_add_string(json, "rpc_name", rpc_name.c_str());
        json_add_string(json, "group", def.group);
        json_add_string(json, "category", def.category);
        json_add_string(json, "kind", setting_kind_name(def.kind));
        json_add_int(json, "modes", def.mode_mask);
        json_add_float(json, "min", def.min_value);
        json_add_float(json, "max", def.max_value);
        json_add_float(json, "step", def.step);
        json_add_int(json, "scale_div", def.scale_div);
        json_add_int(json, "decimals", def.decimals);
        if (def.options && def.option_count) {
            json += ",\"options\":[";
            size_t options_emitted = 0;
            for (uint8_t opt_index = 0; opt_index < def.option_count;
                 ++opt_index) {
                if (options_emitted++) json += ',';
                json += "{";
                json_add_int(json, "value", opt_index, false);
                json_add_string(json, "label", def.options[opt_index]);
                json += "}";
            }
            json += ']';
        }
        json += '}';
    }
    json += "],\"composites\":[";
    for (size_t i = 0; i < as11_setting_composite_count(); ++i) {
        const As11SettingCompositeDef &def = as11_setting_composite(i);
        if (i) json += ',';
        json += "{";
        json_add_string(json, "key", def.key, false);
        json_add_string(json, "kind", "paired_enum_numeric");
        json_add_string(json, "label", def.label);
        json_add_string(json, "enum_key", def.enum_key);
        json_add_string(json, "numeric_key", def.numeric_key);

        const As11SettingDef *enum_def = as11_find_setting(def.enum_key);
        const As11SettingDef *numeric_def = as11_find_setting(def.numeric_key);
        const std::string enum_rpc_name =
            enum_def ? as11_setting_rpc_long_name(*enum_def) : def.enum_key;
        const std::string numeric_rpc_name =
            numeric_def ? as11_setting_rpc_long_name(*numeric_def)
                        : def.numeric_key;
        std::string rpc_name = enum_rpc_name;
        rpc_name += " + ";
        rpc_name += numeric_rpc_name;
        json_add_string(json, "rpc_name", rpc_name.c_str());
        json_add_string(json, "enum_rpc_name", enum_rpc_name.c_str());
        json_add_string(json, "numeric_rpc_name", numeric_rpc_name.c_str());

        json_add_int(json, "numeric_branch_enum_value",
                     def.numeric_branch_enum_value);
        json_add_string(json, "group", def.group);
        json_add_string(json, "category", def.category);
        json += ",\"options\":[";
        for (uint8_t opt_index = 0; opt_index < def.option_count; ++opt_index) {
            const As11SettingCompositeOption &option = def.options[opt_index];
            if (opt_index) json += ',';
            json += "{";
            json_add_int(json, "value", opt_index, false);
            json_add_string(json, "label", option.label);
            json_add_int(json, "enum_value", option.enum_value);
            if (option.numeric_raw) {
                json_add_string(json, "numeric_raw", option.numeric_raw);
            }
            json += "}";
        }
        json += "]}";
    }
    json += "]}";
}

void WebUI::publish_snapshots(bool force,
                              bool realtime_active,
                              PollCheckpoint checkpoint) {
    const uint32_t now = millis();
    if (!cache_mutex_ || xSemaphoreTake(cache_mutex_, 0) != pdTRUE) return;
    const bool periodic_due =
        static_cast<int32_t>(now - last_snapshot_ms_) >=
        static_cast<int32_t>(AC_WEB_SSE_PUSH_INTERVAL_MS);
    uint16_t rebuild_mask = snapshots_dirty_mask_;
    if (force || !snapshots_ready_) {
        rebuild_mask = SNAPSHOT_ALL;
    } else if (periodic_due) {
        rebuild_mask |= SNAPSHOT_PERIODIC;
    }

    if (!force && snapshots_ready_ && realtime_active) {
        rebuild_mask &= SNAPSHOT_STATUS | SNAPSHOT_STREAM;
    }

    if (!rebuild_mask) {
        xSemaphoreGive(cache_mutex_);
        return;
    }

    if (rebuild_mask & SNAPSHOT_STATUS) {
        build_status_json(cached_status_json_, checkpoint);
        if (checkpoint) checkpoint("web_ui.snapshots.status");
    }
    if (rebuild_mask & SNAPSHOT_STREAM) {
        build_stream_json(cached_stream_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.stream");
    }
    if (rebuild_mask & SNAPSHOT_CONFIG) {
        if (checkpoint) checkpoint("web_ui.snapshots.config");
    }
    if (rebuild_mask & SNAPSHOT_WIFI) {
        build_wifi_json(cached_wifi_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.wifi");
    }
    if (rebuild_mask & SNAPSHOT_OXIMETRY_SENSORS) {
        build_oximetry_sensors_json(cached_oximetry_sensors_json_);
        if (checkpoint) checkpoint("web_ui.snapshots.oximetry_sensors");
    }
    if (rebuild_mask & SNAPSHOT_OTA) {
        build_ota_json(cached_ota_json_, ota_manager_->status());
        if (checkpoint) checkpoint("web_ui.snapshots.ota");
    }
    if (rebuild_mask & SNAPSHOT_RESMED_OTA) {
        build_resmed_ota_json(cached_resmed_ota_json_, *resmed_ota_manager_);
        if (checkpoint) checkpoint("web_ui.snapshots.resmed_ota");
    }
    if (rebuild_mask & SNAPSHOT_SETTINGS) {
        const int requested_mode = requested_settings_mode_;
        const bool refresh_queued =
            settings_manager_ && settings_manager_->refresh_pending();
        observed_settings_refresh_pending_ = refresh_queued;
        observed_settings_revision_ =
            settings_manager_ ? settings_manager_->revision() : 0;
        int published_settings_mode = requested_mode;
        if (published_settings_mode < 0) {
            published_settings_mode =
                active_settings_mode(device_ ? &device_->state() : nullptr,
                                     settings_manager_);
        }

        build_settings_json(cached_settings_json_,
                            requested_mode,
                            refresh_queued);
        cached_settings_mode_ = published_settings_mode;
        if (checkpoint) checkpoint("web_ui.snapshots.settings");
    }
    if (rebuild_mask & SNAPSHOT_CONFIG) {
        cached_http_auth_required_ = network_auth_required(app_config_->data());
        cached_http_user_ = app_config_->data().http_user;
        cached_http_password_ = app_config_->data().http_password;
        cached_auth_whitelist_ = app_config_->data().auth_whitelist;
    }

    snapshots_ready_ = true;
    snapshots_dirty_mask_ &= ~rebuild_mask;
    if (force || periodic_due || (rebuild_mask & SNAPSHOT_PERIODIC)) {
        last_snapshot_ms_ = now;
    }
    xSemaphoreGive(cache_mutex_);
}

void WebUI::execute_command(WebCommand &command) {
    switch (command.kind) {
        case WebCommandConsoleLine:
            execute_console_line(command.text);
            break;
        case WebCommandConsoleClear:
            clear_console_log();
            break;
        case WebCommandConfigUpdate:
            execute_config_update(command.body);
            break;
        case WebCommandWifiUpdate:
            execute_wifi_update(command.body);
            break;
        case WebCommandTimeAction:
            execute_time_action(command.text);
            break;
        case WebCommandSettingsRefresh:
            (void)settings_manager_->request_refresh(
                *rpc_, RpcSource::HttpApi, millis());
            mark_snapshots_dirty(SNAPSHOT_SETTINGS);
            break;
        case WebCommandSettingsUpdate:
            execute_settings_update(command.body);
            break;
        case WebCommandTherapyAction:
            execute_therapy_action(command.text);
            break;
        case WebCommandOximetryAction:
            execute_oximetry_action(command.text, command.body);
            break;
        case WebCommandReportSummaryRefresh:
            execute_report_summary_refresh();
            break;
        case WebCommandResmedOtaInit:
        case WebCommandResmedOtaBlock:
        case WebCommandResmedOtaCheck:
        case WebCommandResmedOtaApply:
        case WebCommandResmedOtaAbort:
            execute_resmed_ota_command(command);
            break;
        default:
            break;
    }
}

void WebUI::execute_report_summary_refresh() {
    if (!report_manager_) return;
    report_manager_->request_summary_refresh(true);
}

void WebUI::execute_console_line(const std::string &line) {
    String command(line.c_str());
    command.trim();
    if (!command.length()) return;
    if (!console_ctx_) return;

    StringPrint capture(AC_WEB_CONSOLE_COMMAND_OUTPUT_MAX);
    web_console_.execute_line(command, capture, *console_ctx_);
    String entry = "> ";
    entry += command;
    entry += "\n";
    entry += capture.text();
    append_console_log(entry);
}

void WebUI::execute_config_update(const std::string &body) {
    AppConfigUpdateResult result;
    const bool parsed = apply_web_config_update(
        *app_config_, body,
        {wifi_manager_->mode_state(), wifi_manager_->has_sta_config()},
        result);
    if (!parsed) return;
    if (!result.persisted) {
        Log::logf(CAT_GENERAL, LOG_WARN,
                  "[WEB] failed to persist one or more config values\n");
    }
    apply_config_runtime_effects(result, *app_config_, *wifi_manager_,
                                 console_ctx_->edf_recorder_manager,
                                 *ota_manager_);
    if (result.changed_fields) mark_snapshots_dirty(SNAPSHOT_ALL);
}

void WebUI::execute_wifi_update(const std::string &body) {
    JsonDocument doc;
    if (deserializeJson(doc, body.c_str())) return;
    String action;
    json_get_string(doc, "action", action);
    bool changed = false;
    if (action == "add" || action == "set") {
        String ssid;
        String pass;
        json_get_string(doc, "ssid", ssid);
        json_get_string(doc, "pass", pass);
        if (action == "set") changed = wifi_manager_->configure_sta(ssid, pass);
        else changed = wifi_manager_->add_profile(ssid, pass, !pass.length());
    } else if (action == "remove" && doc["index"].is<int>()) {
        changed = wifi_manager_->remove_profile(doc["index"].as<int>());
    } else if (action == "clear") {
        wifi_manager_->clear_sta_config();
        changed = true;
    } else if (action == "reconnect") {
        changed = wifi_manager_->reconnect();
    }
    if (changed) mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_WIFI);
}

void WebUI::execute_time_action(const std::string &action) {
    if (action == "ntp_sync") {
        time_sync_service_->force_ntp_sync();
    } else if (action == "sync_to_resmed") {
        time_sync_service_->request_push_esp_to_resmed(RpcSource::HttpApi);
    } else if (action == "sync_from_resmed") {
        time_sync_service_->request_pull_resmed_to_esp(RpcSource::HttpApi);
    } else if (action == "retry_resmed_push") {
        time_sync_service_->reset_resmed_push();
    }
    mark_snapshots_dirty(SNAPSHOT_STATUS);
}

void WebUI::execute_settings_update(const std::string &body) {
    const As11SettingsState &state = settings_manager_->state();
    const As11DeviceState &as11 = device_->state();
    int mode = state.mode_index();
    if (mode < 0) {
        mode = as11_mode_index_from_value(as11.active_therapy_profile());
    }
    size_t accepted = 0;
    std::string params = as11_build_set_params_from_json(body, mode, accepted);
    if (!accepted) return;
    if (settings_manager_->write(*rpc_, params, RpcSource::HttpApi,
                                 millis()).accepted()) {
        mark_snapshots_dirty(SNAPSHOT_SETTINGS);
    }
}

void WebUI::execute_therapy_action(const std::string &action) {
    As11TherapyTarget target = As11TherapyTarget::None;
    if (action == "start") target = As11TherapyTarget::Running;
    if (action == "stop" || action == "standby") {
        target = As11TherapyTarget::Standby;
    }
    if (target == As11TherapyTarget::None) return;

    (void)device_->request_therapy(*rpc_, target, RpcSource::HttpApi,
                                   millis());
    mark_snapshots_dirty(SNAPSHOT_STATUS);
}

void WebUI::execute_oximetry_action(const std::string &action,
                                    const std::string &body) {
    if (!oximetry_manager_) return;
    if (action == "enable") {
        oximetry_manager_->set_enabled(true);
    } else if (action == "disable") {
        oximetry_manager_->set_enabled(false);
    } else if (action == "pair") {
        oximetry_manager_->set_enabled(true);
        oximetry_manager_->request_pairing(true);
    } else if (action == "pair_stop") {
        oximetry_manager_->request_pairing(false);
    } else if (action == "forget") {
        oximetry_manager_->forget_bonds();
    } else if (action == "advertise_start") {
        oximetry_manager_->request_advertising(true);
    } else if (action == "advertise_stop") {
        oximetry_manager_->request_advertising(false);
    } else if (action == "sensor_scan") {
        oximetry_manager_->request_sensor_scan();
    } else if (action == "sensor_disconnect") {
        oximetry_manager_->request_sensor_disconnect();
    } else if (action == "sensor_connect" ||
               action == "sensor_forget" ||
               action == "sensor_autoconnect") {
        JsonDocument doc;
        if (deserializeJson(doc, body.c_str())) return;
        String target;
        String addr;
        String name;
        json_get_string(doc, "target", target);
        json_get_string(doc, "addr", addr);
        json_get_string(doc, "name", name);
        if (action == "sensor_connect") {
            bool ok = false;
            if (addr.length()) {
                OximetrySensorDevice device;
                strncpy(device.addr, addr.c_str(), sizeof(device.addr) - 1);
                device.addr[sizeof(device.addr) - 1] = 0;
                if (doc["addr_type"].is<uint8_t>()) {
                    device.addr_type = doc["addr_type"].as<uint8_t>();
                }
                if (name.length()) {
                    strncpy(device.name, name.c_str(),
                            sizeof(device.name) - 1);
                    device.name[sizeof(device.name) - 1] = 0;
                }
                if (doc["rssi"].is<int>()) device.rssi = doc["rssi"].as<int>();
                ok = oximetry_manager_->request_sensor_connect_device(device);
            } else {
                ok = oximetry_manager_->request_sensor_connect(target.c_str());
            }
            if (!ok) {
                Log::logf(CAT_OXI, LOG_WARN,
                          "[WEB] sensor connect command rejected target=\"%s\" addr=\"%s\"\n",
                          target.c_str(),
                          addr.c_str());
            }
        } else if (action == "sensor_forget") {
            oximetry_manager_->forget_sensor(addr.c_str());
        } else if (action == "sensor_autoconnect" &&
                   doc["enabled"].is<bool>()) {
            oximetry_manager_->set_sensor_autoconnect(
                addr.c_str(), doc["enabled"].as<bool>());
        }
    }
    mark_snapshots_dirty(SNAPSHOT_STATUS | SNAPSHOT_CONFIG |
                         SNAPSHOT_OXIMETRY_SENSORS);
}

void WebUI::execute_resmed_ota_command(const WebCommand &command) {
    if (command.kind == WebCommandResmedOtaCheck) {
        resmed_ota_manager_->request_check();
        mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
        return;
    }
    if (command.kind == WebCommandResmedOtaAbort) {
        resmed_ota_manager_->abort("aborted");
        mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, command.body.c_str())) return;
    if (command.kind == WebCommandResmedOtaInit) {
        if (!doc["size"].is<int>()) return;
        String sha;
        String filename;
        json_get_string(doc, "sha256", sha);
        json_get_string(doc, "filename", filename);
        const int parsed_size = doc["size"].as<int>();
        if (parsed_size > 0) {
            resmed_ota_manager_->begin_upload(
                static_cast<size_t>(parsed_size), sha, filename);
        }
    } else if (command.kind == WebCommandResmedOtaBlock) {
        if (!doc["offset"].is<int>()) return;
        String data;
        if (!json_get_string(doc, "data", data)) return;
        const int parsed_offset = doc["offset"].as<int>();
        if (parsed_offset >= 0) {
            resmed_ota_manager_->submit_block(
                static_cast<size_t>(parsed_offset), data);
        }
    } else if (command.kind == WebCommandResmedOtaApply) {
        String mode;
        String confirm;
        json_get_string(doc, "mode", mode);
        json_get_string(doc, "confirm", confirm);
        if (mode == "plain") {
            const bool reset =
                doc["reset"].is<bool>() && doc["reset"].as<bool>();
            resmed_ota_manager_->request_apply_plain(reset, confirm);
        } else if (mode == "authenticated") {
            String authentication;
            json_get_string(doc, "authentication", authentication);
            resmed_ota_manager_->request_apply_authenticated(authentication,
                                                            confirm);
        }
    }
    mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);
}

void WebUI::register_routes() {
    // Static UI and snapshots
    server_->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        AsyncWebServerResponse *response = request->beginResponse(
            200, "text/html", HTML_PAGE_GZ, HTML_PAGE_GZ_SIZE);
        response->addHeader("Content-Encoding", "gzip");
        request->send(response);
    });

    server_->on(AsyncURIMatcher::exact("/api/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_status_json_);
    });

    server_->on(AsyncURIMatcher::exact("/api/stream"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_stream_json_);
    });

    server_->on(AsyncURIMatcher::exact("/api/live/view"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_live_view_state(request);
    });

    // Storage browser and jobs
    server_->on(AsyncURIMatcher::exact("/api/storage/list"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_list(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/storage/download"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_download(request);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/storage/archive/start"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_storage_archive_start(request);
        },
        nullptr, handle_body);

    server_->on(AsyncURIMatcher::exact("/api/storage/archive/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_archive_status(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/storage/archive/download"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_archive_download(request);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/storage/delete/start"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_storage_delete_start(request);
        },
        nullptr, handle_body);

    server_->on(AsyncURIMatcher::exact("/api/storage/delete/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_delete_status(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/storage/sync/start"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_storage_sync_start(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/storage/sync/verify"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_storage_sync_verify(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/storage/sync/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_storage_sync_status(request);
    });

    // SleepHQ sync
    server_->on(AsyncURIMatcher::exact("/api/sleephq/sync/start"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_start(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/sleephq/sync/check"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_check(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/sleephq/sync/status"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_sleephq_sync_status(request);
    });

    // Reports
    server_->on(AsyncURIMatcher::prefix("/api/report/plot"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        String path = request->url();
        const int query = path.indexOf('?');

        if (query >= 0) path = path.substring(0, query);

        if (path != "/api/report/plot") {
            request->send(404, "application/json",
                          "{\"ok\":false,\"error\":\"not found\"}");
            return;
        }
        send_report_plot(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/report/summary"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_report_summary(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/report/summary"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        send_queue_result(
            request,
            enqueue_simple_command(WebCommandReportSummaryRefresh));
    });

    server_->on(AsyncURIMatcher::exact("/api/report/result"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        if (request->hasArg("night")) {
            send_report_result(request);
            return;
        }
        request->send(400, "application/json",
                      "{\"error\":\"night_required\"}");
    });

    server_->on(AsyncURIMatcher::exact("/api/report/chunks"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_report_chunks(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/report/prefetch"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        BackgroundWorker *w = background_worker();
        if (!w || !report_manager_) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"worker unavailable\"}");
            return;
        }

        const BackgroundWorkerStatus s = w->status();
        const ReportManager::PrefetchSnapshot p =
            report_manager_->prefetch_snapshot();

        LargeTextBuffer json;
        json.reserve(WEB_JSON_RESERVE_SMALL);

        json = "{";
        json_add_bool(json, "ok", true, false);
        json_add_bool(json, "started", s.task_started);
        json_add_bool(json, "enabled", s.enabled);
        json_add_bool(json, "idle", s.idle);
        json_add_string(json, "gate", s.gate_reason);
        json_add_int(json, "ticks", static_cast<long>(s.ticks));
#if AC_STACK_PROFILE_ENABLED
        json_add_int(json, "stack_free",
                     static_cast<long>(s.stack_high_water_words));
#endif
        json_add_int(json, "phase", static_cast<long>(p.phase));
        json_add_uint64(json, "night_ms", p.night_ms);
        json_add_uint64(json, "last_night_ms", p.last_night_ms);
        json_add_uint64(json, "last_failed_night_ms",
                        p.last_failed_night_ms);
        json_add_string(json, "last_source", p.last_source);
        json_add_string(json, "last_error", p.last_error);
        json_add_int(json, "completed", static_cast<long>(p.completed));
        json_add_int(json, "failed", static_cast<long>(p.failed));
        json += "}";

        if (json.overflowed()) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"prefetch alloc\"}");
            return;
        }

        AsyncResponseStream *response =
            request->beginResponseStream("application/json");

        if (!response) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"response alloc\"}");
            return;
        }

        response->write(reinterpret_cast<const uint8_t *>(json.c_str()),
                        json.length());
        request->send(response);
    });

    server_->on(AsyncURIMatcher::exact("/api/report/prefetch"), HTTP_POST,
                [](AsyncWebServerRequest *request) {
        BackgroundWorker *w = background_worker();
        if (!w) {
            request->send(503, "application/json",
                          "{\"ok\":false,\"error\":\"worker unavailable\"}");
            return;
        }
        if (!request->hasArg("enable")) {
            request->send(400, "application/json",
                          "{\"ok\":false,\"error\":\"missing enable=0|1\"}");
            return;
        }

        const bool on = request->arg("enable") != "0";
        w->set_enabled(on);

        char buf[64];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"enabled\":%s,\"applied\":%s}",
                 on ? "true" : "false",
                 w->enabled() ? "true" : "false");

        request->send(200, "application/json", buf);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/report/result"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (!report_manager_) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"report unavailable\"}");
                return;
            }

            if (request->hasArg("night")) {
                uint64_t night_start_ms = 0;
                if (!request_uint64_arg(request, "night", night_start_ms)) {
                    request->send(400, "application/json",
                                  "{\"ok\":false,\"error\":\"bad night\"}");
                    return;
                }

                bool refresh_cache = false;
                if (request->hasArg("refresh_cache")) {
                    parse_bool_yesno(request->arg("refresh_cache"),
                                     refresh_cache);
                }

                const bool queued =
                    report_manager_->request_result_prepare_by_start(
                        night_start_ms,
                        refresh_cache);

                if (queued) {
                    request->send(202, "application/json", queued_json());
                } else {
                    request->send(503, "application/json",
                                  "{\"ok\":false,\"error\":\"report_queue_full\"}");
                }
                return;
            }

            JsonDocument doc;
            std::string body;

            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            if (!doc["night"].is<unsigned long long>()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing night\"}");
                return;
            }

            bool refresh_cache = false;

            if (doc["refresh_cache"].is<bool>()) {
                refresh_cache = doc["refresh_cache"].as<bool>();
            }

            const uint64_t night_start_ms = static_cast<uint64_t>(
                doc["night"].as<unsigned long long>());
            if (night_start_ms == 0) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad night\"}");
                return;
            }

            const bool queued =
                report_manager_->request_result_prepare_by_start(
                    night_start_ms,
                    refresh_cache);

            if (queued) {
                request->send(202, "application/json", queued_json());
            } else {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"report_queue_full\"}");
            }
        },
        nullptr, handle_body);

    // Web console and file log
    server_->on(
        AsyncURIMatcher::exact("/api/console"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_console_snapshot(request);
        });

    server_->on(
        AsyncURIMatcher::exact("/api/console"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;

            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }

            String command;

            if (!json_get_string(doc, "cmd", command)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing cmd\"}");
                return;
            }

            command.trim();

            if (!command.length()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"empty cmd\"}");
                return;
            }

            WebCommand queued;
            queued.kind = WebCommandConsoleLine;
            queued.text = command.c_str();

            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/console/clear"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandConsoleClear));
        });

    server_->on(
        AsyncURIMatcher::exact("/api/log/current"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            size_t lines = AC_FILE_LOG_TAIL_DEFAULT_LINES;

            if (!request_size_arg_limited(request,
                                          "tail",
                                          AC_FILE_LOG_TAIL_DEFAULT_LINES,
                                          AC_FILE_LOG_TAIL_MAX_LINES,
                                          lines) ||
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

    // Configuration
    server_->on(AsyncURIMatcher::exact("/api/config"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_config_json(request);
    });

    server_->on(AsyncURIMatcher::exact("/api/config/schema"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_config_schema_json(request);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/config"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_config_update(request);
        },
        nullptr, handle_body);

    const auto register_config_section =
        [this](const char *path, const char *section) {
            server_->on(AsyncURIMatcher::exact(path), HTTP_GET,
                        [this, section](AsyncWebServerRequest *request) {
                send_config_json(request, section);
            });

            server_->on(
                AsyncURIMatcher::exact(path), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
                    send_config_update(request);
                },
                nullptr, handle_body);
        };

    register_config_section("/api/config/device", "device");
    register_config_section("/api/config/network", "network");
    register_config_section("/api/config/access", "access");
    register_config_section("/api/config/ota", "ota");
    register_config_section("/api/config/logging", "logging");
    register_config_section("/api/config/time", "time");
    register_config_section("/api/config/oximetry", "oximetry");
    register_config_section("/api/config/smb", "smb");
    register_config_section("/api/config/sleephq", "sleephq");

    // ESP OTA
    server_->on(AsyncURIMatcher::exact("/api/ota"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        String json;
        json.reserve(AC_WEB_OTA_JSON_RESERVE);
        build_ota_json(json, ota_manager_->status());
        request->send(200, "application/json", json);
    });

    server_->on(AsyncURIMatcher::exact("/api/ota/check"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        const bool ok = ota_manager_->request_update_check();

        String json;
        json.reserve(AC_WEB_OTA_JSON_RESERVE);
        const OtaManagerStatus status = ota_manager_->status();
        build_ota_json(json, status);

        const int response_status = ok
            ? 202
            : (status.update_error == "ota_busy" ? 409 : 400);
        request->send(response_status, "application/json", json);
    });

    server_->on(AsyncURIMatcher::exact("/api/ota/install-update"), HTTP_POST,
                [this](AsyncWebServerRequest *request) {
        if (resmed_ota_manager_->transport_active()) {
            request->send(409, "application/json",
                          "{\"error\":\"resmed_ota_active\"}");
            return;
        }

        const bool ok = ota_manager_->request_available_update();
        mark_snapshots_dirty(SNAPSHOT_OTA);

        String json;
        json.reserve(AC_WEB_OTA_JSON_RESERVE);
        const OtaManagerStatus status = ota_manager_->status();
        build_ota_json(json, status);

        const int response_status = ok
            ? 202
            : (status.last_error == "ota_busy" ? 409 : 400);
        request->send(response_status, "application/json", json);
    });

    server_->on(AsyncURIMatcher::exact("/api/ota/url"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (resmed_ota_manager_->transport_active()) {
                request->send(409, "application/json",
                              "{\"error\":\"resmed_ota_active\"}");
                return;
            }

            String url;
            size_t image_size = 0;
            size_t wire_size = 0;
            OtaUploadEncoding encoding = OtaUploadEncoding::Auto;
            if (!request_ota_url_args(request, url, image_size, encoding,
                                      wire_size)) {
                request->send(400, "application/json",
                              "{\"error\":\"invalid_url_args\"}");
                return;
            }

            const bool ok = ota_manager_->request_url_update(
                url, encoding, image_size, wire_size);
            mark_snapshots_dirty(SNAPSHOT_OTA);

            String json;
            json.reserve(AC_WEB_OTA_JSON_RESERVE);
            const OtaManagerStatus status = ota_manager_->status();
            build_ota_json(json, status);

            const int response_status =
                ok ? 202 : (status.last_error == "ota_busy" ? 409 : 400);
            request->send(response_status, "application/json", json);
        });

    server_->on(AsyncURIMatcher::exact("/api/ota/prepare"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (resmed_ota_manager_->transport_active()) {
                request->send(409, "application/json",
                              "{\"error\":\"resmed_ota_active\"}");
                return;
            }

            size_t declared_size = 0;
            size_t wire_size = 0;
            OtaUploadEncoding encoding = OtaUploadEncoding::Auto;

            if (!request_ota_upload_args(request, declared_size, encoding,
                                         wire_size)) {
                request->send(400, "application/json",
                              "{\"error\":\"invalid_upload_args\"}");
                return;
            }

            const bool ok =
                ota_manager_->request_http_upload_prepare(declared_size,
                                                          encoding,
                                                          wire_size);

            mark_snapshots_dirty(SNAPSHOT_OTA);

            String json;
            json.reserve(AC_WEB_OTA_JSON_RESERVE);
            const OtaManagerStatus status = ota_manager_->status();
            build_ota_json(json, status);

            const int response_status =
                ok ? 202 : (status.last_error == "ota_busy" ? 409 : 400);
            request->send(response_status, "application/json", json);
        });

    server_->on(
        AsyncURIMatcher::exact("/api/ota/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            if (ota_manager_->status().url_active) {
                request->send(409, "application/json",
                              "{\"error\":\"ota_busy\"}");
                return;
            }

            const bool ok = ota_manager_->finish_http_upload();

            mark_snapshots_dirty(SNAPSHOT_OTA);

            String json;
            json.reserve(AC_WEB_OTA_JSON_RESERVE);
            build_ota_json(json, ota_manager_->status());

            request->send(ok ? 200 : 400, "application/json", json);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final) {
            if (index == 0) {
                if (ota_manager_->status().url_active) {
                    if (request && request->client()) request->client()->close();
                    return;
                }

                size_t declared_size = 0;
                size_t wire_size = 0;
                OtaUploadEncoding encoding = OtaUploadEncoding::Auto;

                if (!request_ota_upload_args(request, declared_size, encoding,
                                             wire_size)) {
                    if (request && request->client()) request->client()->close();
                    return;
                }

                if (!ota_manager_->begin_http_upload(filename, declared_size,
                                                     encoding, wire_size)) {
                    mark_snapshots_dirty(SNAPSHOT_OTA);
                    return;
                }

                mark_snapshots_dirty(SNAPSHOT_OTA);
            }

            if (!ota_manager_->write_http_upload(index, data, len)) {
                mark_snapshots_dirty(SNAPSHOT_OTA);
                if (request && request->client()) request->client()->close();
                return;
            }

            if (final) mark_snapshots_dirty(SNAPSHOT_OTA);
        });

    // ResMed OTA
    server_->on(AsyncURIMatcher::exact("/api/resmed-ota"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_resmed_ota_json_);
        });

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/upload"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            const ResmedOtaStatus status = resmed_ota_manager_->status();
            bool ok = status.phase == ResmedOtaPhase::Staging &&
                      resmed_ota_manager_->finish_staged_upload();

            mark_snapshots_dirty(SNAPSHOT_RESMED_OTA);

            String json;
            json.reserve(AC_WEB_RESMED_OTA_JSON_RESERVE);
            build_resmed_ota_json(json, *resmed_ota_manager_);

            request->send(ok ? 200 : 400, "application/json", json);
        },
        [this](AsyncWebServerRequest *request, const String &filename,
               size_t index, uint8_t *data, size_t len, bool final) {
            (void)final;

            if (index == 0) {
                size_t declared_size = 0;

                if (!request_size_arg(request, "size", declared_size)) {
                    resmed_ota_manager_->abort("missing_size");
                    return;
                }

                const String magic =
                    request->hasArg("magic") ? request->arg("magic") : "";

                if (!resmed_ota_manager_->begin_staged_upload(
                        declared_size, filename, magic)) {
                    return;
                }
            }

            resmed_ota_manager_->write_staged_upload(index, data, len);
        });

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/init"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["size"].is<int>()) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing size\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandResmedOtaInit;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/block"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            if (!doc["offset"].is<int>()) {
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
            WebCommand queued;
            queued.kind = WebCommandResmedOtaBlock;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/check"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandResmedOtaCheck));
        });

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/apply"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandResmedOtaApply;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/resmed-ota/abort"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            send_queue_result(request,
                              enqueue_simple_command(WebCommandResmedOtaAbort));
        });

    server_->on(
        AsyncURIMatcher::exact("/api/time"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            json_get_string(doc, "action", action);
            WebCommand queued;
            queued.kind = WebCommandTimeAction;
            queued.text = action.c_str();
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/oximetry"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            if (!json_get_string(doc, "action", action)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"missing action\"}");
                return;
            }
            action.trim();
            WebCommand queued;
            queued.kind = WebCommandOximetryAction;
            queued.text = action.c_str();
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(AsyncURIMatcher::exact("/api/oximetry/sensors"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            send_cached(request, cached_oximetry_sensors_json_);
        });

    server_->on(AsyncURIMatcher::exact("/api/wifi"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        send_cached(request, cached_wifi_json_);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/wifi"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandWifiUpdate;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(AsyncURIMatcher::exact("/api/settings-catalog"), HTTP_GET,
        [this](AsyncWebServerRequest *request) {
            LargeTextBuffer json;
            json.reserve(AC_WEB_SETTINGS_CATALOG_JSON_RESERVE);
            build_settings_catalog_json(json);
            if (json.overflowed()) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"catalog alloc\"}");
                return;
            }
            AsyncResponseStream *response =
                request->beginResponseStream("application/json");
            if (!response) {
                request->send(503, "application/json",
                              "{\"ok\":false,\"error\":\"response alloc\"}");
                return;
            }
            response->write(
                reinterpret_cast<const uint8_t *>(json.c_str()),
                json.length());
            request->send(response);
        });

    server_->on(AsyncURIMatcher::exact("/api/settings"), HTTP_GET,
                [this](AsyncWebServerRequest *request) {
        const int mode = request_profile_mode_arg(request);
        bool refresh_requested = false;
        if (request->hasArg("refresh")) {
            refresh_requested =
                enqueue_simple_command(WebCommandSettingsRefresh);
        }
        send_cached_settings(request, mode, refresh_requested);
    });

    server_->on(
        AsyncURIMatcher::exact("/api/settings"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandSettingsUpdate;
            queued.body = std::move(body);
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->on(
        AsyncURIMatcher::exact("/api/therapy"), HTTP_POST,
        [this](AsyncWebServerRequest *request) {
            JsonDocument doc;
            std::string body;
            if (!parse_body_copy(request, doc, body)) {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"bad json\"}");
                return;
            }
            String action;
            json_get_string(doc, "action", action);
            if (action != "start" && action != "stop" &&
                action != "standby") {
                request->send(400, "application/json",
                              "{\"ok\":false,\"error\":\"unknown action\"}");
                return;
            }
            WebCommand queued;
            queued.kind = WebCommandTherapyAction;
            queued.text = action.c_str();
            send_queue_result(request, enqueue_command(std::move(queued)));
        },
        nullptr, handle_body);

    server_->onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "application/json",
                      "{\"ok\":false,\"error\":\"not found\"}");
    });
}

}  // namespace aircannect
