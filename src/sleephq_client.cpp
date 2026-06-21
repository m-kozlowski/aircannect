#include "sleephq_client.h"

#include <ArduinoJson.h>
#include <errno.h>
#include <ctype.h>
#include <esp_rom_md5.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr const char *SLEEPHQ_HOST = "sleephq.com";
static constexpr uint16_t SLEEPHQ_PORT = 443;
static constexpr uint32_t SLEEPHQ_HTTP_TIMEOUT_MS = 15000;
static constexpr size_t SLEEPHQ_IO_CHUNK = 384;
static constexpr size_t SLEEPHQ_UPLOAD_CHUNK = 4096;
static constexpr size_t SLEEPHQ_TOKEN_RESERVE = 2048;
static constexpr size_t SLEEPHQ_RESPONSE_BODY_INITIAL_RESERVE = 512;
static constexpr size_t SLEEPHQ_AUTH_BODY_RESERVE = 384;
static constexpr size_t SLEEPHQ_CREATE_IMPORT_BODY_RESERVE = 96;
static constexpr size_t SLEEPHQ_MIN_INTERNAL_FREE = 60 * 1024;
static constexpr size_t SLEEPHQ_MIN_INTERNAL_MAX_ALLOC = 36 * 1024;

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

bool header_value_has_token(const char *value, const char *token) {
    if (!value || !token) return false;
    const size_t token_len = strlen(token);
    for (const char *p = value; *p; ++p) {
        if (strncasecmp(p, token, token_len) == 0) return true;
    }
    return false;
}

void trim_ascii(char *text) {
    if (!text) return;
    char *start = text;
    while (*start && isspace(static_cast<unsigned char>(*start))) start++;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t len = strlen(text);
    while (len && isspace(static_cast<unsigned char>(text[len - 1]))) {
        text[--len] = 0;
    }
}

bool parse_u32_text(const char *text, uint32_t &out) {
    if (!text || !*text) return false;
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > UINT32_MAX) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool json_string_to_u32(JsonVariantConst value, uint32_t &out) {
    if (value.is<uint32_t>()) {
        out = value.as<uint32_t>();
        return true;
    }
    if (value.is<const char *>()) {
        return parse_u32_text(value.as<const char *>(), out);
    }
    return false;
}

const char *json_string_or_empty(JsonVariantConst value) {
    return value.is<const char *>() ? value.as<const char *>() : "";
}

void digest_to_hex(const uint8_t digest[16],
                   char out[AC_SLEEPHQ_CONTENT_HASH_MAX]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0F];
    }
    out[32] = '\0';
}

}  // namespace

SleepHqClient::~SleepHqClient() {
    disconnect();
}

void SleepHqClient::set_error(const char *error) {
    copy_cstr(last_error_, sizeof(last_error_), error ? error : "");
}

void SleepHqClient::copy_config_string(char *dst,
                                       size_t dst_size,
                                       const String &src) {
    copy_cstr(dst, dst_size, src.c_str());
}

bool SleepHqClient::configure(const AppConfigData &config) {
    SleepHqConfig snapshot;
    copy_config_string(snapshot.client_id, sizeof(snapshot.client_id),
                       config.sleephq_client_id);
    copy_config_string(snapshot.client_secret, sizeof(snapshot.client_secret),
                       config.sleephq_client_secret);
    copy_config_string(snapshot.team_id, sizeof(snapshot.team_id),
                       config.sleephq_team_id);
    copy_config_string(snapshot.device_id, sizeof(snapshot.device_id),
                       config.sleephq_device_id);
    return configure(snapshot);
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
    if (client_.connected()) {
        client_.stop();
    }
}

bool SleepHqClient::tls_heap_available() {
    const MemoryStatus mem = Memory::status();
    if (mem.heap_free >= SLEEPHQ_MIN_INTERNAL_FREE &&
        mem.heap_max_alloc >= SLEEPHQ_MIN_INTERNAL_MAX_ALLOC) {
        return true;
    }
    set_error("tls_heap_guard");
    Log::logf(CAT_SLEEPHQ, LOG_WARN,
              "TLS heap guard free=%u max_alloc=%u\n",
              static_cast<unsigned>(mem.heap_free),
              static_cast<unsigned>(mem.heap_max_alloc));
    return false;
}

bool SleepHqClient::ensure_connected() {
    if (client_.connected()) return true;
    if (!configured()) {
        set_error("not_configured");
        return false;
    }
    if (!tls_heap_available()) return false;

    client_.stop();
    client_.setCACert(SLEEPHQ_TRUST_ANCHOR_GTS_ROOT_R4_CA);
    client_.setTimeout(SLEEPHQ_HTTP_TIMEOUT_MS / 1000);
    if (!client_.connect(SLEEPHQ_HOST, SLEEPHQ_PORT)) {
        set_error("connect_failed");
        Log::logf(CAT_SLEEPHQ, LOG_WARN, "connect failed\n");
        return false;
    }
    configure_socket_options();
    return true;
}

void SleepHqClient::configure_socket_options() {
    const int fd = client_.fd();
    if (fd < 0) return;

    const timeval tv = {SLEEPHQ_HTTP_TIMEOUT_MS / 1000, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "setsockopt(SO_SNDTIMEO) errno=%d\n", errno);
    }
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "setsockopt(SO_RCVTIMEO) errno=%d\n", errno);
    }
    const int one = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) != 0) {
        Log::logf(CAT_SLEEPHQ, LOG_WARN,
                  "setsockopt(TCP_NODELAY) errno=%d\n", errno);
    }
}

bool SleepHqClient::write_all(const char *data, size_t len) {
    return write_bytes(reinterpret_cast<const uint8_t *>(data), len);
}

bool SleepHqClient::write_authorization_header() {
    static constexpr char PREFIX[] = "Authorization: Bearer ";
    static constexpr char CRLF[] = "\r\n";
    return write_all(PREFIX, sizeof(PREFIX) - 1) &&
           write_all(access_token_.c_str(), access_token_.length()) &&
           write_all(CRLF, sizeof(CRLF) - 1);
}

bool SleepHqClient::write_bytes(const uint8_t *data, size_t len) {
    if (!data && len) return false;
    size_t offset = 0;
    const uint32_t started_ms = millis();
    while (offset < len) {
        const size_t chunk = len - offset > SLEEPHQ_IO_CHUNK
                                 ? SLEEPHQ_IO_CHUNK
                                 : len - offset;
        const size_t written = client_.write(
            data + offset, chunk);
        if (written > 0) {
            offset += written;
            continue;
        }
        if (!client_.connected() ||
            static_cast<uint32_t>(millis() - started_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("write_timeout");
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

bool SleepHqClient::read_line(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    size_t len = 0;
    const uint32_t started_ms = millis();
    while (true) {
        if (client_.available()) {
            const int c = client_.read();
            if (c < 0) continue;
            if (c == '\r') continue;
            if (c == '\n') {
                out[len] = 0;
                return true;
            }
            if (len + 1 >= out_size) {
                set_error("header_line_too_long");
                return false;
            }
            out[len++] = static_cast<char>(c);
            continue;
        }
        if (!client_.connected() ||
            static_cast<uint32_t>(millis() - started_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("read_timeout");
            return false;
        }
        vTaskDelay(1);
    }
}

bool SleepHqClient::read_header_line(char *out,
                                     size_t out_size,
                                     bool &truncated) {
    if (!out || out_size == 0) return false;
    size_t len = 0;
    truncated = false;
    const uint32_t started_ms = millis();
    while (true) {
        if (client_.available()) {
            const int c = client_.read();
            if (c < 0) continue;
            if (c == '\r') continue;
            if (c == '\n') {
                out[len] = 0;
                return true;
            }
            if (len + 1 < out_size) {
                out[len++] = static_cast<char>(c);
            } else {
                truncated = true;
            }
            continue;
        }
        if (!client_.connected() ||
            static_cast<uint32_t>(millis() - started_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("read_timeout");
            return false;
        }
        vTaskDelay(1);
    }
}

bool SleepHqClient::read_exact(uint8_t *out, size_t len) {
    if (!out && len) return false;
    size_t offset = 0;
    const uint32_t started_ms = millis();
    while (offset < len) {
        if (client_.available()) {
            const int read_now = client_.read(out + offset, len - offset);
            if (read_now > 0) {
                offset += static_cast<size_t>(read_now);
                continue;
            }
        }
        if (!client_.connected() ||
            static_cast<uint32_t>(millis() - started_ms) >=
                SLEEPHQ_HTTP_TIMEOUT_MS) {
            set_error("read_timeout");
            return false;
        }
        vTaskDelay(1);
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

bool SleepHqClient::read_response_body(size_t content_length,
                                       bool has_content_length,
                                       bool chunked,
                                       SleepHqHttpResponse &out) {
    out.body.clear();
    if (!out.body.reserve(SLEEPHQ_RESPONSE_BODY_INITIAL_RESERVE)) {
        set_error("response_alloc_failed");
        return false;
    }
    if (chunked) return read_chunked_body(out);

    uint8_t buf[SLEEPHQ_IO_CHUNK];
    if (has_content_length) {
        if (content_length > AC_SLEEPHQ_HTTP_RESPONSE_MAX) {
            set_error("response_too_large");
            return false;
        }
        size_t remaining = content_length;
        while (remaining > 0) {
            const size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            if (!read_exact(buf, n)) return false;
            if (!append_body(out, reinterpret_cast<const char *>(buf), n)) {
                return false;
            }
            remaining -= n;
        }
        return true;
    }

    const uint32_t started_ms = millis();
    while (client_.connected() || client_.available()) {
        if (client_.available()) {
            const int n = client_.read(buf, sizeof(buf));
            if (n > 0 &&
                !append_body(out, reinterpret_cast<const char *>(buf),
                             static_cast<size_t>(n))) {
                return false;
            }
            continue;
        }
        if (static_cast<uint32_t>(millis() - started_ms) >=
            SLEEPHQ_HTTP_TIMEOUT_MS) {
            break;
        }
        vTaskDelay(1);
    }
    return true;
}

bool SleepHqClient::read_chunked_body(SleepHqHttpResponse &out) {
    uint8_t buf[SLEEPHQ_IO_CHUNK];
    char line[48];
    while (true) {
        if (!read_line(line, sizeof(line))) return false;
        char *end = nullptr;
        const unsigned long chunk_len = strtoul(line, &end, 16);
        if (end == line) {
            set_error("bad_chunk_header");
            return false;
        }
        if (chunk_len == 0) {
            do {
                if (!read_line(line, sizeof(line))) return false;
            } while (line[0] != 0);
            return true;
        }
        size_t remaining = static_cast<size_t>(chunk_len);
        while (remaining > 0) {
            const size_t n = remaining > sizeof(buf) ? sizeof(buf) : remaining;
            if (!read_exact(buf, n)) return false;
            if (!append_body(out, reinterpret_cast<const char *>(buf), n)) {
                return false;
            }
            remaining -= n;
        }
        uint8_t crlf[2];
        if (!read_exact(crlf, sizeof(crlf))) return false;
    }
}

bool SleepHqClient::raw_request(const char *method,
                                const char *path,
                                const char *body,
                                const char *content_type,
                                bool authorize,
                                SleepHqHttpResponse &out) {
    out.status = 0;
    out.unauthorized = false;
    out.body.clear();
    set_error("");

    if (!ensure_connected()) return false;

    const size_t body_len = body ? strlen(body) : 0;
    char request_head[384];
    int len = snprintf(
        request_head, sizeof(request_head),
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: AirCANnect\r\n"
        "Accept: application/vnd.api+json, application/json\r\n"
        "Connection: keep-alive\r\n",
        method ? method : "GET",
        path ? path : "/",
        SLEEPHQ_HOST);
    if (len <= 0 || len >= static_cast<int>(sizeof(request_head))) {
        set_error("request_header_too_long");
        return false;
    }
    if (!write_all(request_head, static_cast<size_t>(len))) {
        disconnect();
        return false;
    }
    if (authorize && access_token_.length()) {
        if (!write_authorization_header()) {
            disconnect();
            return false;
        }
    }
    if (body_len) {
        char content_header[128];
        len = snprintf(content_header, sizeof(content_header),
                       "Content-Type: %s\r\n"
                       "Content-Length: %u\r\n",
                       content_type ? content_type : "application/json",
                       static_cast<unsigned>(body_len));
        if (len <= 0 || len >= static_cast<int>(sizeof(content_header))) {
            set_error("request_header_too_long");
            return false;
        }
        if (!write_all(content_header, static_cast<size_t>(len))) {
            disconnect();
            return false;
        }
    }
    static constexpr char HEADER_END[] = "\r\n";
    if (!write_all(HEADER_END, sizeof(HEADER_END) - 1) ||
        (body_len && !write_all(body, body_len))) {
        disconnect();
        return false;
    }

    return read_response(out);
}

bool SleepHqClient::read_response(SleepHqHttpResponse &out) {
    char line[256];
    if (!read_line(line, sizeof(line))) {
        disconnect();
        return false;
    }
    if (strncmp(line, "HTTP/", 5) != 0) {
        set_error("bad_status_line");
        disconnect();
        return false;
    }
    out.status = atoi(line + 9);
    out.unauthorized = out.status == 401;

    bool has_content_length = false;
    bool chunked = false;
    bool connection_close = false;
    size_t content_length = 0;
    while (true) {
        bool truncated = false;
        if (!read_header_line(line, sizeof(line), truncated)) {
            disconnect();
            return false;
        }
        if (line[0] == 0) break;
        if (truncated) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *value = colon + 1;
        trim_ascii(line);
        trim_ascii(value);
        if (strcasecmp(line, "Content-Length") == 0) {
            content_length = static_cast<size_t>(strtoul(value, nullptr, 10));
            has_content_length = true;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            chunked = header_value_has_token(value, "chunked");
        } else if (strcasecmp(line, "Connection") == 0) {
            connection_close = header_value_has_token(value, "close");
        }
    }

    const bool ok = read_response_body(content_length, has_content_length,
                                       chunked, out);
    if (connection_close || !ok) disconnect();
    if (!ok) return false;

    if (out.status < 200 || out.status >= 300) {
        snprintf(last_error_, sizeof(last_error_), "http_%d", out.status);
        return false;
    }
    return true;
}

bool SleepHqClient::request(const char *method,
                            const char *path,
                            const char *body,
                            const char *content_type,
                            bool authorize,
                            SleepHqHttpResponse &out) {
    if (authorize && access_token_.length() == 0 && !authenticate()) {
        return false;
    }
    if (raw_request(method, path, body, content_type, authorize, out)) {
        return true;
    }
    if (!authorize || !out.unauthorized) return false;

    access_token_.clear();
    disconnect();
    if (!authenticate()) return false;
    return raw_request(method, path, body, content_type, true, out);
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

bool SleepHqClient::authenticate() {
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
                     "application/x-www-form-urlencoded", false, response)) {
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
    if (!text || !*text) return false;
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > UINT32_MAX) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool SleepHqClient::resolve_team_id(uint32_t &team_id) {
    if (parse_uint32_field(config_.team_id, team_id)) return true;

    SleepHqHttpResponse response;
    if (!request("GET", "/api/v1/me", nullptr, nullptr, true, response)) {
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
    if (json_string_to_u32(v, team_id)) return true;
    v = doc["data"]["attributes"]["current_team_id"];
    if (json_string_to_u32(v, team_id)) return true;
    v = doc["data"]["current_team_id"];
    if (json_string_to_u32(v, team_id)) return true;
    v = doc["current_team_id"];
    if (json_string_to_u32(v, team_id)) return true;

    set_error("team_id_missing");
    return false;
}

bool SleepHqClient::create_import(uint32_t team_id, SleepHqImportInfo &out) {
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
                 "application/x-www-form-urlencoded", true, response)) {
        return false;
    }
    return parse_import(response, out);
}

bool SleepHqClient::attach_file(const SleepHqAttachRequest &attach,
                                SleepHqUploadResult &out) {
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
    if (!request("POST", api_path, body.c_str(), content_type, true,
                 response)) {
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
    if (!ensure_connected()) return false;

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

    char request_head[384];
    int len = snprintf(request_head, sizeof(request_head),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "User-Agent: AirCANnect\r\n"
                       "Accept: application/vnd.api+json, application/json\r\n",
                       api_path,
                       SLEEPHQ_HOST);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(request_head)) {
        set_error("request_header_too_long");
        return false;
    }
    if (!write_all(request_head, static_cast<size_t>(len)) ||
        !write_authorization_header()) {
        disconnect();
        return false;
    }
    char request_tail[192];
    len = snprintf(request_tail, sizeof(request_tail),
                   "Content-Type: multipart/form-data; boundary=%s\r\n"
                   "Content-Length: %llu\r\n"
                   "Connection: keep-alive\r\n\r\n",
                   boundary,
                   static_cast<unsigned long long>(content_length));
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(request_tail)) {
        set_error("request_header_too_long");
        return false;
    }
    if (!write_all(request_tail, static_cast<size_t>(len))) {
        disconnect();
        return false;
    }

    char part[384];
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
                   "%s\r\n",
                   boundary, request.name);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len))) {
        disconnect();
        return false;
    }
    len = snprintf(part, sizeof(part),
                   "--%s\r\n"
                   "Content-Disposition: form-data; name=\"path\"\r\n\r\n"
                   "%s\r\n",
                   boundary, request.path);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len))) {
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
        !write_all(part, static_cast<size_t>(len))) {
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
    bool ok = true;
    while (sent < request.size) {
        if (request.should_abort && request.should_abort(request.ctx)) {
            set_error("preempted");
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
        if (!write_bytes(buffer, read)) {
            ok = false;
            disconnect();
            break;
        }
        sent += read;
        out.bytes = sent;
        taskYIELD();
    }
    Memory::free(buffer);
    if (!ok) return false;

    if (hash_precomputed) {
        copy_cstr(out.content_hash, sizeof(out.content_hash),
                  request.content_hash);
    } else {
        esp_rom_md5_update(&md5,
                           reinterpret_cast<const uint8_t *>(request.name),
                           strlen(request.name));
        uint8_t digest[16];
        esp_rom_md5_final(digest, &md5);
        digest_to_hex(digest, out.content_hash);
    }

    len = snprintf(part, sizeof(part),
                   "\r\n--%s\r\n"
                   "Content-Disposition: form-data; name=\"content_hash\"\r\n"
                   "\r\n%s\r\n--%s--\r\n",
                   boundary, out.content_hash, boundary);
    if (len <= 0 || static_cast<size_t>(len) >= sizeof(part) ||
        !write_all(part, static_cast<size_t>(len))) {
        disconnect();
        return false;
    }
    return read_response(response);
}

bool SleepHqClient::upload_file(const SleepHqUploadRequest &request,
                                SleepHqUploadResult &out) {
    if (access_token_.length() == 0 && !authenticate()) return false;

    SleepHqHttpResponse response;
    if (upload_file_once(request, out, response)) return true;
    if (!response.unauthorized) return false;

    access_token_.clear();
    disconnect();
    if (!request.reset || !request.reset(request.ctx) || !authenticate()) {
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
                                    bool &has_more) {
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
    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response)) {
        return false;
    }
    return parse_file_list(response, per_page, callback, ctx, count, has_more);
}

bool SleepHqClient::list_team_machines(uint32_t team_id,
                                       uint32_t page,
                                       uint32_t per_page,
                                       SleepHqMachineCallback callback,
                                       void *ctx,
                                       size_t &count,
                                       bool &has_more) {
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
    if (!request("GET", path, nullptr, nullptr, true, response)) {
        return false;
    }
    return parse_machine_list(response, per_page, callback, ctx,
                              count, has_more);
}

bool SleepHqClient::get_machine_date(uint32_t machine_id,
                                     const char *date,
                                     SleepHqMachineDate &out) {
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
    if (!request("GET", path, nullptr, nullptr, true, response)) {
        return false;
    }
    return parse_machine_date(response, out);
}

bool SleepHqClient::process_import(uint32_t import_id,
                                   SleepHqImportInfo *out) {
    char path[64];
    snprintf(path, sizeof(path), "/api/v1/imports/%lu/process_files",
             static_cast<unsigned long>(import_id));
    SleepHqHttpResponse response;
    if (!request("POST", path, nullptr, nullptr, true, response)) {
        return false;
    }
    if (!out) return true;
    return parse_import(response, *out);
}

bool SleepHqClient::get_import(uint32_t import_id, SleepHqImportInfo &out) {
    char path[48];
    snprintf(path, sizeof(path), "/api/v1/imports/%lu",
             static_cast<unsigned long>(import_id));
    SleepHqHttpResponse response;
    if (!request("GET", path, nullptr, nullptr, true, response)) {
        return false;
    }
    return parse_import(response, out);
}

bool SleepHqClient::parse_file_list(const SleepHqHttpResponse &response,
                                    uint32_t per_page,
                                    SleepHqRemoteFileCallback callback,
                                    void *ctx,
                                    size_t &count,
                                    bool &has_more) {
    char error[AC_SLEEPHQ_ERROR_MAX] = {};
    const bool ok = sleephq_parse_remote_file_list_json(response.body.c_str(),
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
    if (!json_string_to_u32(id, out.id)) {
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
