#pragma once

#include <ESPAsyncWebServer.h>
#include "prepared_byte_transfer.h"

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

// Known-length response whose source must have its first bytes ready before
// HTTP starts. A streaming producer may pause only after yielding initial data;
// storage readiness, TCP ACK or poll can then resume it.
class AsyncPreparedResponse final : public AsyncWebServerResponse {
public:
    using StreamFiller = std::function<PreparedByteRead(uint8_t *, size_t, size_t)>;
    AsyncPreparedResponse(int status_code,
                          const char *content_type,
                          size_t content_length,
                          AwsResponseFiller content = {});
    AsyncPreparedResponse(const char *content_type,
                          size_t content_length,
                          AwsResponseFiller content);
    AsyncPreparedResponse(const char *content_type, size_t content_length, StreamFiller stream);
    ~AsyncPreparedResponse() override;

    bool _sourceValid() const override;
    void _respond(AsyncWebServerRequest *request) override;
    size_t _ack(AsyncWebServerRequest *request,
                size_t len,
                uint32_t time) override;

private:
    struct Wake;
    bool fill_pending(AsyncWebServerRequest *request);
    size_t pump(AsyncWebServerRequest *request);
    void fail(AsyncWebServerRequest *request);

    AwsResponseFiller content_;
    StreamFiller stream_;
    std::shared_ptr<Wake> wake_;
    String assembled_headers_;
    size_t written_headers_length_ = 0;

    uint8_t *buffer_ = nullptr;
    size_t buffer_capacity_ = 0;
    size_t buffer_offset_ = 0;
    size_t buffer_length_ = 0;
    bool source_ended_ = false;
};

}  // namespace aircannect
