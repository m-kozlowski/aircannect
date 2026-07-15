#include "ota_url_client.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_tls_errors.h>

#include "board_net.h"

namespace aircannect {
namespace {

static constexpr char OTA_URL_USER_AGENT[] = "AirCANnect OTA";

struct OtaUrlStreamContext {
    size_t expected_size = 0;
    size_t offset = 0;
    bool size_checked = false;
    OtaUrlWriteCallback write_callback = nullptr;
    OtaUrlContinueCallback continue_callback = nullptr;
    void *callback_ctx = nullptr;
    OtaUrlError *error = nullptr;
};

void clear_error(OtaUrlError &error) {
    error = OtaUrlError();
}

void set_error_code(OtaUrlError &error, const char *code) {
    snprintf(error.code, sizeof(error.code), "%s", code ? code : "url_error");
}

void set_http_error(OtaUrlError &error, int status) {
    error.http_status = status;
    snprintf(error.code, sizeof(error.code), "url_http_%d", status);
}

bool operation_allowed(OtaUrlContinueCallback callback, void *ctx) {
    return !callback || callback(ctx);
}

esp_http_client_handle_t create_client(const char *url,
                                       esp_http_client_method_t method,
                                       http_event_handle_cb event_handler,
                                       void *user_data) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.user_agent = OTA_URL_USER_AGENT;
    config.method = method;
    config.timeout_ms = AC_OTA_URL_HTTP_TIMEOUT_MS;
    config.disable_auto_redirect = false;
    config.max_redirection_count = AC_OTA_URL_REDIRECT_LIMIT;
    config.max_authorization_retries = -1;
    config.event_handler = event_handler;
    config.buffer_size = AC_OTA_URL_HTTP_BUFFER_BYTES;
    config.buffer_size_tx = AC_OTA_URL_HTTP_TX_BUFFER_BYTES;
    config.user_data = user_data;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_header(client, "Connection", "close");
    }
    return client;
}

void capture_transport_error(esp_http_client_handle_t client,
                             esp_err_t result,
                             OtaUrlError &error) {
    error.esp_error = static_cast<int>(result);
    error.socket_error = esp_http_client_get_errno(client);

    int tls_error = 0;
    int tls_flags = 0;
    const esp_err_t tls_result = esp_http_client_get_and_clear_last_tls_error(
        client, &tls_error, &tls_flags);
    error.tls_error = tls_error;
    error.tls_flags = tls_flags;

    if (error.code[0]) return;

    if (result == ESP_ERR_HTTP_MAX_REDIRECT) {
        set_error_code(error, "url_too_many_redirects");
    } else if (tls_result == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
        set_error_code(error, "url_dns_failed");
    } else if (tls_result != ESP_OK || tls_error != 0 || tls_flags != 0) {
        set_error_code(error, "url_tls_failed");
    } else if (result == ESP_ERR_HTTP_READ_TIMEOUT ||
               result == ESP_ERR_HTTP_EAGAIN ||
               error.socket_error == ETIMEDOUT) {
        set_error_code(error, "url_timeout");
    } else if (result == ESP_ERR_HTTP_CONNECT) {
        set_error_code(error, "url_connect_failed");
    } else if (result == ESP_ERR_HTTP_FETCH_HEADER) {
        set_error_code(error, "url_header_failed");
    } else if (result == ESP_ERR_HTTP_INCOMPLETE_DATA ||
               result == ESP_ERR_HTTP_CONNECTION_CLOSED) {
        set_error_code(error, "url_incomplete_response");
    } else {
        set_error_code(error, "url_transport_failed");
    }
}

esp_err_t stream_event(esp_http_client_event_t *event) {
    if (!event || !event->user_data) return ESP_OK;
    OtaUrlStreamContext &ctx =
        *static_cast<OtaUrlStreamContext *>(event->user_data);

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0) {
        return ESP_OK;
    }

    const int status = esp_http_client_get_status_code(event->client);
    if (status != 200) return ESP_OK;

    if (!operation_allowed(ctx.continue_callback, ctx.callback_ctx)) {
        set_error_code(*ctx.error, "url_cancelled");
        return ESP_FAIL;
    }

    if (!ctx.size_checked) {
        const int64_t content_length =
            esp_http_client_get_content_length(event->client);
        if (content_length > 0 &&
            static_cast<uint64_t>(content_length) != ctx.expected_size) {
            set_error_code(*ctx.error, "url_size_changed");
            return ESP_FAIL;
        }
        ctx.size_checked = true;
    }

    const size_t len = static_cast<size_t>(event->data_len);
    if (ctx.offset > ctx.expected_size ||
        len > ctx.expected_size - ctx.offset) {
        set_error_code(*ctx.error, "url_response_too_large");
        return ESP_FAIL;
    }
    if (!ctx.write_callback ||
        !ctx.write_callback(ctx.callback_ctx, ctx.offset,
                            static_cast<const uint8_t *>(event->data), len)) {
        if (!ctx.error->code[0]) {
            set_error_code(*ctx.error, "url_ota_write_failed");
        }
        return ESP_FAIL;
    }

    ctx.offset += len;
    return ESP_OK;
}

}  // namespace

bool ota_url_supported(const char *url) {
    if (!url || !*url) return false;

    const bool http = strncasecmp(url, "http://", 7) == 0;
    const bool https = strncasecmp(url, "https://", 8) == 0;
    if (!http && !https) return false;

    const char *host = url + (https ? 8 : 7);
    if (!*host) return false;

    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(url);
         *p; ++p) {
        if (*p <= 0x20 || *p == 0x7F) return false;
    }
    return true;
}

bool ota_url_probe(const char *url,
                   OtaUrlMetadata &metadata,
                   OtaUrlError &error,
                   OtaUrlContinueCallback continue_callback,
                   void *callback_ctx) {
    metadata = OtaUrlMetadata();
    clear_error(error);

    if (!ota_url_supported(url)) {
        set_error_code(error, "url_invalid");
        return false;
    }
    if (!operation_allowed(continue_callback, callback_ctx)) {
        set_error_code(error, "url_cancelled");
        return false;
    }

    esp_http_client_handle_t client =
        create_client(url, HTTP_METHOD_HEAD, nullptr, nullptr);
    if (!client) {
        set_error_code(error, "url_client_alloc_failed");
        return false;
    }

    const esp_err_t result = esp_http_client_perform(client);
    metadata.http_status = esp_http_client_get_status_code(client);
    error.http_status = metadata.http_status;

    if (result != ESP_OK) {
        capture_transport_error(client, result, error);
        esp_http_client_cleanup(client);
        return false;
    }
    if (!operation_allowed(continue_callback, callback_ctx)) {
        set_error_code(error, "url_cancelled");
        esp_http_client_cleanup(client);
        return false;
    }
    if (metadata.http_status != 200) {
        set_http_error(error, metadata.http_status);
        esp_http_client_cleanup(client);
        return false;
    }

    const int64_t content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0 ||
        static_cast<uint64_t>(content_length) > SIZE_MAX) {
        set_error_code(error, "url_content_length_missing");
        esp_http_client_cleanup(client);
        return false;
    }

    metadata.content_length = static_cast<size_t>(content_length);
    esp_http_client_cleanup(client);
    return true;
}

bool ota_url_stream(const char *url,
                    size_t expected_size,
                    OtaUrlWriteCallback write_callback,
                    OtaUrlContinueCallback continue_callback,
                    void *callback_ctx,
                    OtaUrlError &error) {
    clear_error(error);
    if (!ota_url_supported(url) || expected_size == 0 || !write_callback) {
        set_error_code(error, "url_invalid_request");
        return false;
    }

    OtaUrlStreamContext context;
    context.expected_size = expected_size;
    context.write_callback = write_callback;
    context.continue_callback = continue_callback;
    context.callback_ctx = callback_ctx;
    context.error = &error;

    esp_http_client_handle_t client =
        create_client(url, HTTP_METHOD_GET, stream_event, &context);
    if (!client) {
        set_error_code(error, "url_client_alloc_failed");
        return false;
    }

    const esp_err_t result = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    error.http_status = status;

    if (result != ESP_OK) {
        capture_transport_error(client, result, error);
        esp_http_client_cleanup(client);
        return false;
    }
    if (status != 200) {
        set_http_error(error, status);
        esp_http_client_cleanup(client);
        return false;
    }
    if (context.offset != expected_size) {
        set_error_code(error, "url_incomplete_response");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_cleanup(client);
    return true;
}

}  // namespace aircannect
