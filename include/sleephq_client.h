#pragma once

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <stddef.h>
#include <stdint.h>

#include "app_config.h"
#include "background_operation_control.h"
#include "large_text_buffer.h"
#include "sleephq_protocol.h"

namespace aircannect {

using SleepHqUploadReadCallback =
    bool (*)(void *ctx, uint8_t *out, size_t len, size_t &read);
using SleepHqUploadResetCallback = bool (*)(void *ctx);
using SleepHqResponseBodyCallback =
    bool (*)(void *ctx, const uint8_t *data, size_t size);

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
    const char *content_hash = nullptr;
    uint64_t size = 0;
    SleepHqUploadReadCallback read = nullptr;
    SleepHqUploadResetCallback reset = nullptr;
    void *ctx = nullptr;
    const BackgroundOperationControl *operation = nullptr;
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

    bool authenticate(const BackgroundOperationControl *operation = nullptr);
    bool resolve_team_id(
        uint32_t &team_id,
        const BackgroundOperationControl *operation = nullptr);
    bool create_import(
        uint32_t team_id,
        SleepHqImportInfo &out,
        const BackgroundOperationControl *operation = nullptr);
    bool attach_file(const SleepHqAttachRequest &request,
                     SleepHqUploadResult &out,
                     const BackgroundOperationControl *operation = nullptr);
    bool upload_file(const SleepHqUploadRequest &request,
                     SleepHqUploadResult &out);
    bool list_team_files(uint32_t team_id,
                         uint32_t page,
                         uint32_t per_page,
                         SleepHqRemoteFileCallback callback,
                         void *ctx,
                         size_t &count,
                         bool &has_more,
                         const BackgroundOperationControl *operation = nullptr);
    bool list_team_machines(uint32_t team_id,
                            uint32_t page,
                            uint32_t per_page,
                            SleepHqMachineCallback callback,
                            void *ctx,
                            size_t &count,
                            bool &has_more,
                            const BackgroundOperationControl *operation = nullptr);
    bool get_machine_date(uint32_t machine_id,
                          const char *date,
                          SleepHqMachineDate &out,
                          const BackgroundOperationControl *operation = nullptr);
    bool process_import(
        uint32_t import_id,
        SleepHqImportInfo *out = nullptr,
        const BackgroundOperationControl *operation = nullptr);
    bool get_import(
        uint32_t import_id,
        SleepHqImportInfo &out,
        const BackgroundOperationControl *operation = nullptr);

private:
    bool request(const char *method,
                 const char *path,
                 const char *body,
                 const char *content_type,
                 bool authorize,
                 SleepHqHttpResponse &out,
                 SleepHqResponseBodyCallback body_callback = nullptr,
                 void *body_ctx = nullptr,
                 const BackgroundOperationControl *operation = nullptr);
    bool raw_request(const char *method,
                     const char *path,
                     const char *body,
                     const char *content_type,
                     bool authorize,
                     SleepHqHttpResponse &out,
                     SleepHqResponseBodyCallback body_callback = nullptr,
                     void *body_ctx = nullptr,
                     const BackgroundOperationControl *operation = nullptr);
    bool read_response(SleepHqHttpResponse &out,
                       SleepHqResponseBodyCallback body_callback = nullptr,
                       void *body_ctx = nullptr,
                       const BackgroundOperationControl *operation = nullptr);

    bool ensure_connected(const BackgroundOperationControl *operation);
    bool tls_heap_available();
    void configure_socket_options();
    bool operation_allows(const BackgroundOperationControl *operation);
    bool write_all(const char *data, size_t len,
                   const BackgroundOperationControl *operation);
    bool write_bytes(const uint8_t *data, size_t len,
                     const BackgroundOperationControl *operation);
    bool write_authorization_header(
        const BackgroundOperationControl *operation);
    bool read_line(char *out, size_t out_size,
                   const BackgroundOperationControl *operation);
    bool read_header_line(char *out, size_t out_size, bool &truncated,
                          const BackgroundOperationControl *operation);
    bool read_exact(uint8_t *out, size_t len,
                    const BackgroundOperationControl *operation);
    bool read_response_body(size_t content_length,
                            bool has_content_length,
                            bool chunked,
                            SleepHqHttpResponse &out,
                            SleepHqResponseBodyCallback body_callback,
                            void *body_ctx,
                            bool buffer_body,
                            const BackgroundOperationControl *operation);
    bool read_chunked_body(SleepHqHttpResponse &out,
                           SleepHqResponseBodyCallback body_callback,
                           void *body_ctx,
                           bool buffer_body,
                           const BackgroundOperationControl *operation);
    bool consume_body(SleepHqHttpResponse &out, const uint8_t *data,
                      size_t len, SleepHqResponseBodyCallback body_callback,
                      void *body_ctx, bool buffer_body);
    bool append_body(SleepHqHttpResponse &out, const char *data, size_t len);

    bool parse_token(const SleepHqHttpResponse &response);
    bool parse_team_id(const SleepHqHttpResponse &response,
                       uint32_t &team_id);
    bool parse_import(const SleepHqHttpResponse &response,
                      SleepHqImportInfo &out);
    bool parse_machine_list(const SleepHqHttpResponse &response,
                            uint32_t per_page,
                            SleepHqMachineCallback callback,
                            void *ctx,
                            size_t &count,
                            bool &has_more);
    bool parse_machine_date(const SleepHqHttpResponse &response,
                            SleepHqMachineDate &out);
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
