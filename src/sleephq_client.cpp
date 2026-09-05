#include "sleephq_client.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <ctype.h>
#include <errno.h>
#include <esp_rom_md5.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "hex_util.h"
#include "json_util.h"
#include "memory_manager.h"
#include "string_util.h"
#include "tls_memory.h"

namespace aircannect {
namespace {

static constexpr const char *SLEEPHQ_HOST = "sleephq.com";
static constexpr uint32_t SLEEPHQ_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint32_t SLEEPHQ_READ_WAIT_MS = 500;
static constexpr uint32_t SLEEPHQ_HTTP_TIMEOUT_MS = 15000;
static constexpr size_t SLEEPHQ_READ_CHUNK = 384;
static constexpr size_t SLEEPHQ_WRITE_CHUNK = 4096;
static constexpr size_t SLEEPHQ_UPLOAD_CHUNK = SLEEPHQ_WRITE_CHUNK;
static constexpr uint32_t SLEEPHQ_UPLOAD_PROGRESS_INTERVAL_MS = 60000;
static constexpr size_t SLEEPHQ_TOKEN_RESERVE = 2048;
static constexpr size_t SLEEPHQ_RESPONSE_BODY_INITIAL_RESERVE = 512;
static constexpr size_t SLEEPHQ_AUTH_BODY_RESERVE = 384;
static constexpr size_t SLEEPHQ_CREATE_IMPORT_BODY_RESERVE = 96;
// SleepHQ currently serves a Google Trust Services WE1 chain rooted at GTS
// Root R4. This intentionally fails closed if SleepHQ changes CA families; do
// not replace it with setInsecure().
static const char *SLEEPHQ_TRUST_ANCHOR_GTS_ROOT_R4_CA =
    "-----BEGIN CERTIFICATE-----\n"
    "MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD\n"
    "VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG\n"
    "A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw\n"
    "WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz\n"
    "IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi\n"
    "AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi\n"
    "QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR\n"
    "HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW\n"
    "BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D\n"
    "9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8\n"
    "p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD\n"
    "-----END CERTIFICATE-----\n";

const char *json_string_or_empty(JsonVariantConst value) {
    return value.is<const char *>() ? value.as<const char *>() : "";
}

}  // namespace

SleepHqClient::~SleepHqClient() {
    disconnect();
}

void SleepHqClient::set_error(const char *error) {
    copy_cstr(last_error_, sizeof(last_error_), error ? error : "");
}

bool SleepHqClient::configure(const SleepHqConfig &config) {
    disconnect();
    config_ = config;
    access_token_.clear();
    set_error("");
    return configured();
}

bool SleepHqClient::configured() const {
    return config_.client_id[0] && config_.client_secret[0];
}

void SleepHqClient::disconnect() {
    operation_ = nullptr;
    if (client_) {
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }
    header_capacity_ = 0;
}

bool SleepHqClient::tls_heap_available() {
    const TlsMemoryAdmission admission = TlsMemory::admission();
    if (admission.available) return true;

    set_error("tls_heap_guard");
    Log::logf(CAT_EXPORT, LOG_WARN,
              "[SLEEPHQ] TLS heap guard free=%u max_alloc=%u min_free=%u "
              "min_max_alloc=%u psram_tls=%u\n",
              static_cast<unsigned>(admission.internal_free),
              static_cast<unsigned>(admission.internal_largest),
              static_cast<unsigned>(admission.minimum_free),
              static_cast<unsigned>(admission.minimum_largest),
              admission.psram_allocator ? 1u : 0u);
    return false;
}

bool SleepHqClient::operation_allows(
    const BackgroundOperationControl *operation) {
    if (!operation) return true;

    const BackgroundOperationStop reason = operation->stop_reason(millis());
    if (reason == BackgroundOperationStop::None) return true;

    set_error(background_operation_stop_error(reason));
    return false;
}

esp_err_t SleepHqClient::http_event(esp_http_client_event_t *event) {
    auto *self = static_cast<SleepHqClient *>(event->user_data);
    if (!self || !self->operation_) return ESP_OK;

    if (event->event_id == HTTP_EVENT_ON_CONNECTED ||
        event->event_id == HTTP_EVENT_ON_HEADER ||
        event->event_id == HTTP_EVENT_ON_DATA) {
        if (!self->operation_allows(self->operation_)) {
            // IDF ignores callback errors during synchronous reads.
            esp_http_client_close(event->client);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

bool SleepHqClient::open_request(
    const char *method,
    const char *path,
    const char *content_type,
    bool authorize,
    uint64_t content_length,
    const BackgroundOperationControl *operation,
    bool close_after) {
    if (!operation_allows(operation)) {
        disconnect();
        return false;
    }
    if (content_length > INT_MAX) {
        set_error("request_too_large");
        return false;
    }
    if (!configured()) {
        set_error("not_configured");
        return false;
    }

    char url[192];
    const int length = snprintf(url, sizeof(url), "https://%s%s",
                                SLEEPHQ_HOST, path ? path : "/");
    if (length < 0 || static_cast<size_t>(length) >= sizeof(url)) {
        set_error("request_url_too_long");
        return false;
    }

    // IDF must fit each complete header in its transmit buffer.
    const size_t header_capacity = std::max<size_t>(
        512, authorize ? access_token_.length() + 32 : 0);
    if (client_ && header_capacity > header_capacity_) disconnect();

    if (!client_) {
        if (!tls_heap_available()) return false;

        esp_http_client_config_t config = {};
        config.url = url;
        config.cert_pem = SLEEPHQ_TRUST_ANCHOR_GTS_ROOT_R4_CA;
        config.user_agent = "AirCANnect";
        config.timeout_ms = SLEEPHQ_CONNECT_TIMEOUT_MS;
        config.buffer_size = 512;
        config.buffer_size_tx = header_capacity;
        config.disable_auto_redirect = true;
        config.max_authorization_retries = -1;
        config.event_handler = http_event;
        config.user_data = this;
        client_ = esp_http_client_init(&config);
        if (!client_) {
            set_error("http_client_alloc_failed");
            return false;
        }
        header_capacity_ = header_capacity;
    }

    operation_ = operation;
    LargeTextBuffer authorization;
    if (authorize && access_token_.length()) {
        if (!authorization.append("Bearer ") ||
            !authorization.append(access_token_.c_str(), access_token_.length())) {
            set_error("request_header_alloc_failed");
            disconnect();
            return false;
        }
    }

    const esp_http_client_method_t http_method =
        method && strcmp(method, "POST") == 0 ? HTTP_METHOD_POST
                                             : HTTP_METHOD_GET;

    const esp_err_t auth_header = authorization.length()
        ? esp_http_client_set_header(client_, "Authorization",
                                     authorization.c_str())
        : esp_http_client_delete_header(client_, "Authorization");

    const bool prepared =
        esp_http_client_set_url(client_, url) == ESP_OK &&
        esp_http_client_set_method(client_, http_method) == ESP_OK &&
        esp_http_client_set_timeout_ms(client_, SLEEPHQ_CONNECT_TIMEOUT_MS) == ESP_OK &&
        esp_http_client_set_header(client_, "Accept",
            "application/vnd.api+json, application/json") == ESP_OK &&
        esp_http_client_set_header(client_, "Accept-Encoding", "identity") == ESP_OK &&
        esp_http_client_set_header(client_, "Connection",
            close_after ? "close" : "keep-alive") == ESP_OK &&
        esp_http_client_set_header(client_, "Content-Type",
            content_type ? content_type : "application/json") == ESP_OK &&
        (auth_header == ESP_OK ||
         (!authorization.length() && auth_header == ESP_ERR_NOT_FOUND));

    if (!prepared) {
        set_error("request_header_failed");
        disconnect();
        return false;
    }

    const uint32_t connect_started_ms = millis();
    const esp_err_t opened =
        esp_http_client_open(client_, static_cast<int>(content_length));
    if (!operation_allows(operation) || opened != ESP_OK || last_error_[0]) {
        if (!last_error_[0]) {
            const int socket_error = esp_http_client_get_errno(client_);
            int tls_error = 0;
            int tls_flags = 0;
            const esp_err_t tls_result =
                esp_http_client_get_and_clear_last_tls_error(
                    client_, &tls_error, &tls_flags);
            set_error("connect_failed");
            Log::logf(CAT_EXPORT, LOG_WARN,
                      "[SLEEPHQ] connect failed esp=%s(0x%x) "
                      "errno=%d/%.32s tls=%s(0x%x) code=%d "
                      "verify=0x%x elapsed_ms=%u\n",
                      esp_err_to_name(opened), static_cast<unsigned>(opened),
                      socket_error,
                      socket_error ? strerror(socket_error) : "none",
                      esp_err_to_name(tls_result),
                      static_cast<unsigned>(tls_result), tls_error,
                      static_cast<unsigned>(tls_flags),
                      static_cast<unsigned>(millis() - connect_started_ms));
        }
        disconnect();
        return false;
    }

    esp_http_client_set_timeout_ms(client_, SLEEPHQ_HTTP_TIMEOUT_MS);
    return true;
}

bool SleepHqClient::write_all(
    const char *data,
    size_t len,
    const BackgroundOperationControl *operation) {
    return write_bytes(reinterpret_cast<const uint8_t *>(data), len,
                       operation);
}

bool SleepHqClient::write_bytes(
    const uint8_t *data,
    size_t len,
    const BackgroundOperationControl *operation) {
    size_t offset = 0;
    while (offset < len) {
        if (!operation_allows(operation)) return false;

        const size_t chunk = std::min(len - offset, SLEEPHQ_WRITE_CHUNK);
        const int written = esp_http_client_write(
            client_, reinterpret_cast<const char *>(data + offset), chunk);
        if (!operation_allows(operation)) return false;
        if (written <= 0) {
            // A failed IDF write may already have sent part of this chunk.
            set_error("write_failed");
            return false;
        }

        offset += static_cast<size_t>(written);
    }

    return true;
}

bool SleepHqClient::append_body(SleepHqHttpResponse &out,
                                const char *data,
                                size_t len) {
    if (out.body.length() + len > AC_SLEEPHQ_HTTP_RESPONSE_MAX) {
        set_error("response_too_large");
        return false;
    }
    return out.body.append(data, len);
}

bool SleepHqClient::consume_body(
    SleepHqHttpResponse &out,
    const uint8_t *data,
    size_t len,
    SleepHqResponseBodyCallback body_callback,
    void *body_ctx,
    bool buffer_body) {
    if (body_callback && !body_callback(body_ctx, data, len)) {
        set_error("response_body_callback_failed");
        return false;
    }
    if (!buffer_body) return true;
    return append_body(out, reinterpret_cast<const char *>(data), len);
}

bool SleepHqClient::raw_request(const char *method,
                                const char *path,
                                const char *body,
                                const char *content_type,
                                bool authorize,
                                SleepHqHttpResponse &out,
                                SleepHqResponseBodyCallback body_callback,
                                void *body_ctx,
                                const BackgroundOperationControl *operation) {
    out.status = 0;
    out.unauthorized = false;
    out.body.clear();
    set_error("");

    const size_t body_len = body ? strlen(body) : 0;
    if (!open_request(method, path, content_type, authorize, body_len,
                      operation)) {
        return false;
    }

    if (body_len && !write_all(body, body_len, operation)) {
        disconnect();
        return false;
    }

    return read_response(out, body_callback, body_ctx, operation);
}

bool SleepHqClient::read_response(SleepHqHttpResponse &out,
                                  SleepHqResponseBodyCallback body_callback,
                                  void *body_ctx,
                                  const BackgroundOperationControl *operation) {
    esp_http_client_set_timeout_ms(client_, SLEEPHQ_READ_WAIT_MS);
    uint32_t progress_ms = millis();
    int64_t content_length = -1;
    while (true) {
        if (!operation_allows(operation)) {
            disconnect();
            return false;
        }

        content_length = esp_http_client_fetch_headers(client_);
        if (!operation_allows(operation) || last_error_[0]) {
            disconnect();
            return false;
        }
        if (content_length >= 0) break;
        if (content_length != -ESP_ERR_HTTP_EAGAIN ||
            static_cast<uint32_t>(millis() - progress_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("response_header_failed");
            disconnect();
            return false;
        }

        vTaskDelay(1);
    }

    out.status = esp_http_client_get_status_code(client_);
    out.unauthorized = out.status == 401;
    const bool successful_status = out.status >= 200 && out.status < 300;
    const bool buffer_body = body_callback == nullptr;
    const bool body_too_large =
        static_cast<uint64_t>(content_length) > AC_SLEEPHQ_HTTP_RESPONSE_MAX;

    if (buffer_body &&
        (body_too_large ||
         !out.body.reserve(SLEEPHQ_RESPONSE_BODY_INITIAL_RESERVE))) {
        set_error(body_too_large ? "response_too_large"
                                 : "response_alloc_failed");
        disconnect();
        return false;
    }

    progress_ms = millis();
    uint8_t buf[SLEEPHQ_READ_CHUNK];
    while (true) {
        if (!operation_allows(operation)) {
            disconnect();
            return false;
        }

        // Drain bytes cached while fetching headers even if IDF reports
        // the message complete already.
        const int count = esp_http_client_read(
            client_, reinterpret_cast<char *>(buf), sizeof(buf));
        if (!operation_allows(operation) || last_error_[0]) {
            disconnect();
            return false;
        }
        if (count > 0) {
            if (!consume_body(out, buf, static_cast<size_t>(count),
                              successful_status ? body_callback : nullptr,
                              body_ctx, buffer_body)) {
                disconnect();
                return false;
            }

            progress_ms = millis();
            continue;
        }
        if (count == 0 && esp_http_client_is_complete_data_received(client_)) {
            break;
        }
        if ((count < 0 && count != -ESP_ERR_HTTP_EAGAIN) ||
            static_cast<uint32_t>(millis() - progress_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("response_incomplete");
            disconnect();
            return false;
        }

        vTaskDelay(1);
    }

    operation_ = nullptr;
    if (!successful_status) {
        snprintf(last_error_, sizeof(last_error_), "http_%d", out.status);
        disconnect();
        return false;
    }
    if (!esp_http_client_is_persistent_connection(client_)) disconnect();

    return true;
}

bool SleepHqClient::request(const char *method,
                            const char *path,
                            const char *body,
                            const char *content_type,
                            bool authorize,
                            SleepHqHttpResponse &out,
                            SleepHqResponseBodyCallback body_callback,
                            void *body_ctx,
                            const BackgroundOperationControl *operation) {
    if (authorize && access_token_.length() == 0 &&
        !authenticate(operation)) {
        return false;
    }
    if (raw_request(method, path, body, content_type, authorize, out,
                    body_callback, body_ctx, operation)) {
        return true;
    }
    if (!authorize || !out.unauthorized) return false;

    access_token_.clear();
    disconnect();
    if (!authenticate(operation)) return false;
    return raw_request(method, path, body, content_type, true, out,
                       body_callback, body_ctx, operation);
}

bool SleepHqClient::form_encode_append(LargeTextBuffer &out,
                                       const char *value) {
    static const char HEX_DIGITS[] = "0123456789ABCDEF";
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(value ? value : "");
         *p; ++p) {
        const unsigned char c = *p;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else if (c == ' ') {
            out += '+';
        } else {
            out += '%';
            out += HEX_DIGITS[(c >> 4) & 0x0F];
            out += HEX_DIGITS[c & 0x0F];
        }
        if (out.overflowed()) return false;
    }
    return true;
}

bool SleepHqClient::authenticate(
    const BackgroundOperationControl *operation) {
    if (!configured()) {
        set_error("not_configured");
        return false;
    }

    LargeTextBuffer body;
    if (!body.reserve(SLEEPHQ_AUTH_BODY_RESERVE)) {
        set_error("request_alloc_failed");
        return false;
    }
    body += "grant_type=password&scope=read+write+delete&client_id=";
    if (!form_encode_append(body, config_.client_id)) return false;
    body += "&client_secret=";
    if (!form_encode_append(body, config_.client_secret)) return false;

    SleepHqHttpResponse response;
    if (!raw_request("POST", "/oauth/token", body.c_str(),
                     "application/x-www-form-urlencoded", false, response,
                     nullptr, nullptr, operation)) {
        return false;
    }
    return parse_token(response);
}

bool SleepHqClient::parse_token(const SleepHqHttpResponse &response) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response.body.c_str());
    if (err) {
        set_error("token_json_parse");
        return false;
    }
    const char *token = doc["access_token"] | "";
    if (!token[0]) {
        set_error("token_missing");
        return false;
    }
    access_token_.clear();
    if (!access_token_.reserve(SLEEPHQ_TOKEN_RESERVE) ||
        !access_token_.append(token)) {
        set_error("token_alloc_failed");
        return false;
    }
    return true;
}

bool SleepHqClient::parse_uint32_field(const char *text, uint32_t &out) {
    return parse_uint32_decimal(text, out);
}

bool SleepHqClient::resolve_team_id(
    uint32_t &team_id,
    const BackgroundOperationControl *operation) {
    if (parse_uint32_field(config_.team_id, team_id)) return true;

    SleepHqHttpResponse response;
    if (!request("GET", "/api/v1/me", nullptr, nullptr, true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    return parse_team_id(response, team_id);
}

bool SleepHqClient::parse_team_id(const SleepHqHttpResponse &response,
                                  uint32_t &team_id) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response.body.c_str());
    if (err) {
        set_error("team_json_parse");
        return false;
    }

    JsonVariantConst v =
        doc["data"]["relationships"]["current_team"]["data"]["id"];
    if (json_variant_to_uint32(v, team_id)) return true;
    v = doc["data"]["attributes"]["current_team_id"];
    if (json_variant_to_uint32(v, team_id)) return true;
    v = doc["data"]["current_team_id"];
    if (json_variant_to_uint32(v, team_id)) return true;
    v = doc["current_team_id"];
    if (json_variant_to_uint32(v, team_id)) return true;

    set_error("team_id_missing");
    return false;
}

bool SleepHqClient::create_import(
    uint32_t team_id,
    SleepHqImportInfo &out,
    const BackgroundOperationControl *operation) {
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/teams/%lu/imports",
             static_cast<unsigned long>(team_id));

    LargeTextBuffer body;
    if (!body.reserve(SLEEPHQ_CREATE_IMPORT_BODY_RESERVE)) {
        set_error("request_alloc_failed");
        return false;
    }
    body += "programatic=true";
    if (config_.device_id[0]) {
        body += "&device_id=";
        if (!form_encode_append(body, config_.device_id)) return false;
    }
    SleepHqHttpResponse response;
    if (!request("POST", path, body.c_str(),
                 "application/x-www-form-urlencoded", true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    return parse_import(response, out);
}

bool SleepHqClient::attach_file(const SleepHqAttachRequest &attach,
                                SleepHqUploadResult &out,
                                const BackgroundOperationControl *operation) {
    out = SleepHqUploadResult{};
    if (!attach.import_id || !attach.name || !attach.name[0] ||
        !attach.path || !attach.path[0] || !attach.content_hash ||
        strlen(attach.content_hash) != 32) {
        set_error("bad_attach_request");
        return false;
    }

    char api_path[64];
    snprintf(api_path, sizeof(api_path), "/api/v1/imports/%lu/files",
             static_cast<unsigned long>(attach.import_id));
    char boundary[40];
    snprintf(boundary, sizeof(boundary), "----AirCANnect%08lx",
             static_cast<unsigned long>(millis()));

    LargeTextBuffer body;
    if (!body.reserve(512)) {
        set_error("request_alloc_failed");
        return false;
    }
    char part[256];
    int len = snprintf(part, sizeof(part),
                       "--%s\r\n"
                       "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
                       "%s\r\n",
                       boundary, attach.name);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part)) {
        set_error("multipart_size_failed");
        return false;
    }
    body += part;
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
                   "%s\r\n",
                   boundary, attach.path);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part)) {
        set_error("multipart_size_failed");
        return false;
    }
    body += part;
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"content_hash\"\r\n"
                   "\r\n%s\r\n--%s--\r\n",
                   boundary, attach.content_hash, boundary);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part)) {
        set_error("multipart_size_failed");
        return false;
    }
    body += part;

    char content_type[96];
    len = snprintf(content_type, sizeof(content_type),
                   "multipart/form-data; boundary=%s", boundary);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(content_type)) {
        set_error("multipart_size_failed");
        return false;
    }
    SleepHqHttpResponse response;
    const bool request_ok = request("POST", api_path, body.c_str(),
                                    content_type, true, response,
                                    nullptr, nullptr, operation);
    disconnect();
    if (!request_ok) {
        return false;
    }
    copy_cstr(out.content_hash, sizeof(out.content_hash),
              attach.content_hash);
    out.bytes = 0;
    return true;
}

bool SleepHqClient::upload_file_once(const SleepHqUploadRequest &request,
                                     SleepHqUploadResult &out,
                                     SleepHqHttpResponse &response) {
    out = SleepHqUploadResult{};
    response.status = 0;
    response.unauthorized = false;
    response.body.clear();
    set_error("");

    if (!request.import_id || !request.name || !request.name[0] ||
        !request.path || !request.path[0] || !request.read) {
        set_error("bad_upload_request");
        return false;
    }
    if (request.size > INT_MAX) {
        set_error("request_too_large");
        return false;
    }

    char api_path[64];
    snprintf(api_path, sizeof(api_path), "/api/v1/imports/%lu/files",
             static_cast<unsigned long>(request.import_id));
    char boundary[40];
    snprintf(boundary, sizeof(boundary), "----AirCANnect%08lx",
             static_cast<unsigned long>(millis()));

    const int name_len = snprintf(
        nullptr, 0,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
        "%s\r\n",
        boundary, request.name);
    const int path_len = snprintf(
        nullptr, 0,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
        "%s\r\n",
        boundary, request.path);
    const int file_head_len = snprintf(
        nullptr, 0,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        boundary, request.name);
    const int hash_head_len = snprintf(
        nullptr, 0,
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"content_hash\"\r\n\r\n",
        boundary);
    const int tail_len = snprintf(nullptr, 0, "\r\n--%s--\r\n", boundary);
    if (name_len < 0 || path_len < 0 || file_head_len < 0 ||
        hash_head_len < 0 || tail_len < 0) {
        set_error("multipart_size_failed");
        return false;
    }
    const uint64_t content_length =
        static_cast<uint64_t>(name_len) +
        static_cast<uint64_t>(path_len) +
        static_cast<uint64_t>(file_head_len) +
        request.size +
        static_cast<uint64_t>(hash_head_len) +
        32ULL +
        static_cast<uint64_t>(tail_len);

    char content_type[96];
    snprintf(content_type, sizeof(content_type),
             "multipart/form-data; boundary=%s", boundary);
    if (!open_request("POST", api_path, content_type, true, content_length,
                      request.operation, true)) {
        return false;
    }

    int len = 0;
    char part[384];
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
                   "%s\r\n",
                   boundary, request.name);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len), request.operation)) {
        disconnect();
        return false;
    }
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
                   "%s\r\n",
                   boundary, request.path);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len), request.operation)) {
        disconnect();
        return false;
    }
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"file\"; "
                   "filename=\"%s\"\r\n"
                   "Content-Type: application/octet-stream\r\n\r\n",
                   boundary, request.name);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len), request.operation)) {
        disconnect();
        return false;
    }

    uint8_t *buffer = static_cast<uint8_t *>(
        Memory::alloc_large(SLEEPHQ_UPLOAD_CHUNK, false));
    if (!buffer) {
        set_error("upload_buffer_alloc");
        disconnect();
        return false;
    }
    const bool hash_precomputed =
        request.content_hash && strlen(request.content_hash) == 32;
    md5_context_t md5;
    if (!hash_precomputed) esp_rom_md5_init(&md5);
    uint64_t sent = 0;
    const uint32_t upload_started_ms = millis();
    uint32_t next_progress_ms =
        upload_started_ms + SLEEPHQ_UPLOAD_PROGRESS_INTERVAL_MS;
    bool ok = true;
    while (sent < request.size) {
        if (!operation_allows(request.operation)) {
            ok = false;
            disconnect();
            break;
        }
        const uint64_t remaining = request.size - sent;
        const size_t wanted =
            remaining > SLEEPHQ_UPLOAD_CHUNK
                ? SLEEPHQ_UPLOAD_CHUNK
                : static_cast<size_t>(remaining);
        size_t read = 0;
        if (!request.read(request.ctx, buffer, wanted, read) ||
            read != wanted) {
            set_error("local_read_short");
            ok = false;
            break;
        }
        if (!hash_precomputed) esp_rom_md5_update(&md5, buffer, read);
        if (!write_bytes(buffer, read, request.operation)) {
            ok = false;
            disconnect();
            break;
        }
        sent += read;
        out.bytes = sent;

        const uint32_t now_ms = millis();
        if (static_cast<int32_t>(now_ms - next_progress_ms) >= 0) {
            Log::logf(CAT_EXPORT, LOG_INFO,
                      "[SLEEPHQ] upload progress path=%s bytes=%llu/%llu "
                      "elapsed_ms=%lu\n",
                      request.path,
                      static_cast<unsigned long long>(sent),
                      static_cast<unsigned long long>(request.size),
                      static_cast<unsigned long>(now_ms - upload_started_ms));
            next_progress_ms = now_ms + SLEEPHQ_UPLOAD_PROGRESS_INTERVAL_MS;
        }
        taskYIELD();
    }
    Memory::free(buffer);
    if (!ok) {
        disconnect();
        return false;
    }

    if (hash_precomputed) {
        copy_cstr(out.content_hash, sizeof(out.content_hash),
                  request.content_hash);
    } else {
        esp_rom_md5_update(&md5,
                           reinterpret_cast<const uint8_t *>(request.name),
                           strlen(request.name));
        uint8_t digest[16];
        esp_rom_md5_final(digest, &md5);
        (void)hex_encode(digest, sizeof(digest), out.content_hash,
                         sizeof(out.content_hash), HexCase::Lower);
    }

    len = snprintf(part, sizeof(part),
                   "\r\n--%s\r\n"
                   "Content-Disposition: form-data; name=\"content_hash\"\r\n"
                   "\r\n%s\r\n--%s--\r\n",
                   boundary, out.content_hash, boundary);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len), request.operation)) {
        disconnect();
        return false;
    }
    const bool response_ok = read_response(response, nullptr, nullptr,
                                           request.operation);
    const uint32_t upload_elapsed_ms = millis() - upload_started_ms;
    if (response_ok &&
        upload_elapsed_ms >= SLEEPHQ_UPLOAD_PROGRESS_INTERVAL_MS) {
        Log::logf(CAT_EXPORT, LOG_INFO,
                  "[SLEEPHQ] upload complete path=%s bytes=%llu elapsed_ms=%lu\n",
                  request.path,
                  static_cast<unsigned long long>(out.bytes),
                  static_cast<unsigned long>(upload_elapsed_ms));
    }
    disconnect();
    return response_ok;
}

bool SleepHqClient::upload_file(const SleepHqUploadRequest &request,
                                SleepHqUploadResult &out) {
    if (access_token_.length() == 0 && !authenticate(request.operation)) {
        return false;
    }

    SleepHqHttpResponse response;
    if (upload_file_once(request, out, response)) return true;
    if (!response.unauthorized) return false;

    access_token_.clear();
    disconnect();
    if (!request.reset || !request.reset(request.ctx) ||
        !authenticate(request.operation)) {
        return false;
    }
    return upload_file_once(request, out, response);
}

bool SleepHqClient::list_team_files(uint32_t team_id,
                                    uint32_t page,
                                    uint32_t per_page,
                                    SleepHqRemoteFileCallback callback,
                                    void *ctx,
                                    size_t &count,
                                    bool &has_more,
                                    const BackgroundOperationControl *operation) {
    count = 0;
    has_more = false;
    if (!team_id || !callback || per_page == 0 || per_page > 100) {
        set_error("bad_file_list_request");
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path),
             "/api/v1/teams/%lu/files?page=%lu&per_page=%lu",
             static_cast<unsigned long>(team_id),
             static_cast<unsigned long>(page ? page : 1),
             static_cast<unsigned long>(per_page));
    SleepHqRemoteFileListStreamParser parser;
    if (!parser.begin(per_page, callback, ctx)) {
        set_error("bad_file_list_request");
        return false;
    }
    auto feed_parser = [](void *parser_ctx, const uint8_t *data,
                          size_t size) -> bool {
        SleepHqRemoteFileListStreamParser *stream_parser =
            static_cast<SleepHqRemoteFileListStreamParser *>(parser_ctx);
        return stream_parser && stream_parser->feed(data, size);
    };

    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response,
                 feed_parser, &parser, operation)) {
        char parser_error[AC_SLEEPHQ_ERROR_MAX] = {};
        size_t ignored_count = 0;
        bool ignored_has_more = false;
        if (!parser.finish(ignored_count, ignored_has_more,
                           parser_error, sizeof(parser_error)) &&
            parser_error[0] &&
            strcmp(last_error_, "response_body_callback_failed") == 0) {
            set_error(parser_error);
        }
        return false;
    }
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    if (!parser.finish(count, has_more, error, sizeof(error))) {
        set_error(error);
        return false;
    }
    return true;
}

bool SleepHqClient::list_team_machines(uint32_t team_id,
                                       uint32_t page,
                                       uint32_t per_page,
                                       SleepHqMachineCallback callback,
                                       void *ctx,
                                       size_t &count,
                                       bool &has_more,
                                       const BackgroundOperationControl *operation) {
    count = 0;
    has_more = false;
    if (!team_id || !callback || per_page == 0 || per_page > 100) {
        set_error("bad_machine_list_request");
        return false;
    }
    char path[104];
    snprintf(path, sizeof(path),
             "/api/v1/teams/%lu/machines?page=%lu&per_page=%lu",
             static_cast<unsigned long>(team_id),
             static_cast<unsigned long>(page ? page : 1),
             static_cast<unsigned long>(per_page));
    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    return parse_machine_list(response, per_page, callback, ctx,
                              count, has_more);
}

bool SleepHqClient::get_machine_date(uint32_t machine_id,
                                     const char *date,
                                     SleepHqMachineDate &out,
                                     const BackgroundOperationControl *operation) {
    out = SleepHqMachineDate();
    if (!machine_id || !date || strlen(date) != 10) {
        set_error("bad_machine_date_request");
        return false;
    }
    char path[80];
    snprintf(path, sizeof(path),
             "/api/v1/machines/%lu/machine_dates/%s",
             static_cast<unsigned long>(machine_id),
             date);
    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    return parse_machine_date(response, out);
}

bool SleepHqClient::process_import(uint32_t import_id,
                                   SleepHqImportInfo *out,
                                   const BackgroundOperationControl *operation) {
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/imports/%lu/process_files",
             static_cast<unsigned long>(import_id));
    SleepHqHttpResponse response;
    if (!request("POST", path, nullptr, nullptr, true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    if (!out) return true;
    return parse_import(response, *out);
}

bool SleepHqClient::get_import(
    uint32_t import_id,
    SleepHqImportInfo &out,
    const BackgroundOperationControl *operation) {
    char path[48];
    snprintf(path, sizeof(path), "/api/v1/imports/%lu",
             static_cast<unsigned long>(import_id));
    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response,
                 nullptr, nullptr, operation)) {
        return false;
    }
    return parse_import(response, out);
}

bool SleepHqClient::parse_machine_list(const SleepHqHttpResponse &response,
                                       uint32_t per_page,
                                       SleepHqMachineCallback callback,
                                       void *ctx,
                                       size_t &count,
                                       bool &has_more) {
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    const bool ok = sleephq_parse_machine_list_json(response.body.c_str(),
                                                    per_page,
                                                    callback,
                                                    ctx,
                                                    count,
                                                    has_more,
                                                    error,
                                                    sizeof(error));
    if (!ok) set_error(error);
    return ok;
}

bool SleepHqClient::parse_machine_date(const SleepHqHttpResponse &response,
                                       SleepHqMachineDate &out) {
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    const bool ok = sleephq_parse_machine_date_json(response.body.c_str(),
                                                   out,
                                                   error,
                                                   sizeof(error));
    if (!ok) set_error(error);
    return ok;
}

bool SleepHqClient::parse_import(const SleepHqHttpResponse &response,
                                 SleepHqImportInfo &out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response.body.c_str());
    if (err) {
        set_error("import_json_parse");
        return false;
    }

    out = SleepHqImportInfo{};
    JsonVariantConst id = doc["data"]["id"];
    if (!json_variant_to_uint32(id, out.id)) {
        set_error("import_id_missing");
        return false;
    }
    copy_cstr(out.status, sizeof(out.status),
              json_string_or_empty(doc["data"]["attributes"]["status"]));
    copy_cstr(out.failed_reason, sizeof(out.failed_reason),
              json_string_or_empty(
                  doc["data"]["attributes"]["failed_reason"]));
    set_error("");
    return true;
}

}  // namespace aircannect
