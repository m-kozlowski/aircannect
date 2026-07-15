#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t OTA_URL_ERROR_CODE_MAX = 48;

struct OtaUrlMetadata {
    size_t content_length = 0;
    int http_status = 0;
};

struct OtaUrlError {
    char code[OTA_URL_ERROR_CODE_MAX] = {};
    int http_status = 0;
    int socket_error = 0;
    int esp_error = 0;
    int tls_error = 0;
    int tls_flags = 0;
};

using OtaUrlWriteCallback =
    bool (*)(void *ctx, size_t offset, const uint8_t *data, size_t len);
using OtaUrlContinueCallback = bool (*)(void *ctx);

bool ota_url_supported(const char *url);

bool ota_url_probe(const char *url,
                   OtaUrlMetadata &metadata,
                   OtaUrlError &error,
                   OtaUrlContinueCallback continue_callback = nullptr,
                   void *callback_ctx = nullptr);

bool ota_url_stream(const char *url,
                    size_t expected_size,
                    OtaUrlWriteCallback write_callback,
                    OtaUrlContinueCallback continue_callback,
                    void *callback_ctx,
                    OtaUrlError &error);

}  // namespace aircannect
