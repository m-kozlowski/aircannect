#pragma once

#include <ESPAsyncWebServer.h>

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

// Known-length response whose producer may temporarily have no bytes ready.
// A later TCP poll retries without consuming AsyncAbstractResponse credit.
class AsyncPreparedResponse final : public AsyncWebServerResponse {
public:
    AsyncPreparedResponse(const char *content_type,
                          size_t content_length,
                          AwsResponseFiller content);
    ~AsyncPreparedResponse() override;

    bool _sourceValid() const override;
    void _respond(AsyncWebServerRequest *request) override;
    size_t _ack(AsyncWebServerRequest *request,
                size_t len,
                uint32_t time) override;

private:
    bool fill_pending();
    void fail(AsyncWebServerRequest *request);

    AwsResponseFiller content_;
    String assembled_headers_;
    size_t written_headers_length_ = 0;

    uint8_t *buffer_ = nullptr;
    size_t buffer_capacity_ = 0;
    size_t buffer_offset_ = 0;
    size_t buffer_length_ = 0;
    bool source_ended_ = false;
};

// Chunked response whose producer may temporarily have no bytes ready.
// TCP polls retry the producer without the AsyncAbstractResponse credit gate.
class AsyncPreparedChunkedResponse final : public AsyncWebServerResponse {
public:
    AsyncPreparedChunkedResponse(const char *content_type,
                                 AwsResponseFiller content);
    ~AsyncPreparedChunkedResponse() override;

    bool _sourceValid() const override;
    void _respond(AsyncWebServerRequest *request) override;
    size_t _ack(AsyncWebServerRequest *request,
                size_t len,
                uint32_t time) override;

private:
    bool fill_chunk();
    void fail(AsyncWebServerRequest *request);

    AwsResponseFiller content_;
    String assembled_headers_;
    size_t written_headers_length_ = 0;

    uint8_t *buffer_ = nullptr;
    size_t buffer_capacity_ = 0;
    size_t buffer_offset_ = 0;
    size_t buffer_length_ = 0;
    bool final_chunk_ = false;
    bool source_failed_ = false;
};

}  // namespace aircannect
