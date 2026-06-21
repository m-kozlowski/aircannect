#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "large_text_buffer.h"

namespace aircannect {

static constexpr size_t AC_SLEEPHQ_SECRET_MAX = 193;
static constexpr size_t AC_SLEEPHQ_ID_MAX = 21;
static constexpr size_t AC_SLEEPHQ_ERROR_MAX = 96;
static constexpr size_t AC_SLEEPHQ_STATUS_MAX = 32;
static constexpr size_t AC_SLEEPHQ_HTTP_RESPONSE_MAX = 16 * 1024;
static constexpr size_t AC_SLEEPHQ_CONTENT_HASH_MAX = 33;

using SleepHqUploadReadCallback =
    bool (*)(void *ctx, uint8_t *out, size_t len, size_t &read);
using SleepHqUploadResetCallback = bool (*)(void *ctx);
using SleepHqUploadAbortCallback = bool (*)(void *ctx);

struct SleepHqConfig {
    char client_id[AC_SLEEPHQ_SECRET_MAX] = {};
    char client_secret[AC_SLEEPHQ_SECRET_MAX] = {};
    char team_id[AC_SLEEPHQ_ID_MAX] = {};
    char device_id[AC_SLEEPHQ_ID_MAX] = {};
};

struct SleepHqImportInfo {
    uint32_t id = 0;
    char status[AC_SLEEPHQ_STATUS_MAX] = {};
    char failed_reason[AC_SLEEPHQ_ERROR_MAX] = {};
};

struct SleepHqUploadRequest {
    uint32_t import_id = 0;
    const char *name = nullptr;
    const char *path = nullptr;
    uint64_t size = 0;
    SleepHqUploadReadCallback read = nullptr;
    SleepHqUploadResetCallback reset = nullptr;
    SleepHqUploadAbortCallback should_abort = nullptr;
    void *ctx = nullptr;
};

struct SleepHqAttachRequest {
    uint32_t import_id = 0;
    const char *name = nullptr;
    const char *path = nullptr;
    const char *content_hash = nullptr;
};

struct SleepHqUploadResult {
    uint64_t bytes = 0;
    char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
};

struct SleepHqHttpResponse {
    int status = 0;
    bool unauthorized = false;
    LargeTextBuffer body;
};

class SleepHqClient {
public:
    SleepHqClient() = default;
    ~SleepHqClient();

    bool configure(const AppConfigData &config);
    bool configure(const SleepHqConfig &config);
    bool configured() const;

    void disconnect();
    const char *last_error() const { return last_error_; }

    bool authenticate();
    bool resolve_team_id(uint32_t &team_id);
    bool create_import(uint32_t team_id, SleepHqImportInfo &out);
    bool attach_file(const SleepHqAttachRequest &request,
                     SleepHqUploadResult &out);
    bool upload_file(const SleepHqUploadRequest &request,
                     SleepHqUploadResult &out);
    bool process_import(uint32_t import_id, SleepHqImportInfo *out = nullptr);
    bool get_import(uint32_t import_id, SleepHqImportInfo &out);

private:
    bool request(const char *method,
                 const char *path,
                 const char *body,
                 const char *content_type,
                 bool authorize,
                 SleepHqHttpResponse &out);
    bool raw_request(const char *method,
                     const char *path,
                     const char *body,
                     const char *content_type,
                     bool authorize,
                     SleepHqHttpResponse &out);
    bool read_response(SleepHqHttpResponse &out);

    bool ensure_connected();
    bool tls_heap_available();
    void configure_socket_options();
    bool write_all(const char *data, size_t len);
    bool write_bytes(const uint8_t *data, size_t len);
    bool read_line(char *out, size_t out_size);
    bool read_header_line(char *out, size_t out_size, bool &truncated);
    bool read_exact(uint8_t *out, size_t len);
    bool read_response_body(size_t content_length,
                            bool has_content_length,
                            bool chunked,
                            SleepHqHttpResponse &out);
    bool read_chunked_body(SleepHqHttpResponse &out);
    bool append_body(SleepHqHttpResponse &out, const char *data, size_t len);

    bool parse_token(const SleepHqHttpResponse &response);
    bool parse_team_id(const SleepHqHttpResponse &response,
                       uint32_t &team_id);
    bool parse_import(const SleepHqHttpResponse &response,
                      SleepHqImportInfo &out);
    bool upload_file_once(const SleepHqUploadRequest &request,
                          SleepHqUploadResult &out,
                          SleepHqHttpResponse &response);

    static bool form_encode_append(LargeTextBuffer &out, const char *value);
    static bool parse_uint32_field(const char *text, uint32_t &out);
    static void copy_config_string(char *dst, size_t dst_size,
                                   const String &src);

    void set_error(const char *error);

    SleepHqConfig config_;
    WiFiClientSecure client_;
    LargeTextBuffer access_token_;
    char last_error_[AC_SLEEPHQ_ERROR_MAX] = {};
};

}  // namespace aircannect
