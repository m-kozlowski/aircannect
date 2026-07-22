#include "async_prepared_response.h"

#include <algorithm>
#include <utility>

#include "memory_manager.h"

namespace aircannect {
namespace {

static constexpr size_t PREPARED_RESPONSE_BUFFER_BYTES = 4096;
static constexpr size_t CHUNK_FRAME_BYTES = 8;

}  // namespace

AsyncPreparedResponse::AsyncPreparedResponse(
    const char *content_type,
    size_t content_length,
    AwsResponseFiller content)
    : content_(std::move(content)) {
    _code = 200;
    _contentType = content_type ? content_type : "application/octet-stream";
    _contentLength = content_length;
    addHeader("Connection", "close", false);

    if (content_length > 0) {
        buffer_capacity_ = PREPARED_RESPONSE_BUFFER_BYTES;
        buffer_ = static_cast<uint8_t *>(
            Memory::alloc_large(buffer_capacity_, false));
    }
}

AsyncPreparedResponse::~AsyncPreparedResponse() {
    if (buffer_) Memory::free(buffer_);
}

bool AsyncPreparedResponse::_sourceValid() const {
    return static_cast<bool>(content_) &&
        (_contentLength == 0 || (buffer_ && buffer_capacity_ > 0));
}

void AsyncPreparedResponse::_respond(AsyncWebServerRequest *request) {
    if (!request || !_sourceValid()) {
        if (request) fail(request);
        return;
    }

    _assembleHead(assembled_headers_, request->version());
    _state = RESPONSE_HEADERS;
    (void)_ack(request, 0, 0);
}

bool AsyncPreparedResponse::fill_pending() {
    if (buffer_length_ > buffer_offset_) return true;
    if (_sentLength >= _contentLength) return true;

    const size_t remaining = _contentLength - _sentLength;
    const size_t requested = std::min(buffer_capacity_, remaining);
    const size_t filled = content_(buffer_, requested, _sentLength);
    if (filled == RESPONSE_TRY_AGAIN) return false;
    if (filled == 0 || filled > requested) {
        source_ended_ = true;
        return false;
    }

    buffer_offset_ = 0;
    buffer_length_ = filled;
    _sentLength += filled;
    return true;
}

void AsyncPreparedResponse::fail(AsyncWebServerRequest *request) {
    _state = RESPONSE_FAILED;
    if (request && request->client()) request->client()->close();
}

size_t AsyncPreparedResponse::_ack(AsyncWebServerRequest *request,
                                   size_t len,
                                   uint32_t time) {
    (void)time;
    if (!request || !request->client() || !_sourceValid()) {
        if (request) fail(request);
        return 0;
    }

    _ackedLength += len;
    size_t queued = 0;

    if (_state == RESPONSE_HEADERS) {
        const size_t remaining =
            assembled_headers_.length() - written_headers_length_;
        const size_t added = request->client()->add(
            assembled_headers_.c_str() + written_headers_length_, remaining);
        written_headers_length_ += added;
        _writtenLength += added;
        queued += added;

        if (written_headers_length_ < assembled_headers_.length()) {
            if (queued > 0 && !request->client()->send()) fail(request);
            return queued;
        }

        assembled_headers_ = String();
        _state = RESPONSE_CONTENT;
    }

    while (_state == RESPONSE_CONTENT && request->client()->space() > 0) {
        if (buffer_offset_ >= buffer_length_) {
            buffer_offset_ = 0;
            buffer_length_ = 0;

            if (_sentLength >= _contentLength) {
                _state = RESPONSE_END;
                break;
            }
            if (!fill_pending()) {
                if (source_ended_) fail(request);
                break;
            }
        }

        const size_t pending = buffer_length_ - buffer_offset_;
        const size_t added = request->client()->add(
            reinterpret_cast<const char *>(buffer_ + buffer_offset_), pending);
        if (added == 0) break;

        buffer_offset_ += added;
        _writtenLength += added;
        queued += added;
    }

    if (_sentLength >= _contentLength && buffer_offset_ >= buffer_length_) {
        _state = RESPONSE_END;
    }

    if (queued > 0 && !request->client()->send()) fail(request);
    return queued;
}

AsyncPreparedChunkedResponse::AsyncPreparedChunkedResponse(
    const char *content_type,
    AwsResponseFiller content)
    : content_(std::move(content)) {
    _code = 200;
    _contentType = content_type ? content_type : "application/octet-stream";
    _sendContentLength = false;
    _chunked = true;
    addHeader("Connection", "close", false);

    buffer_capacity_ = PREPARED_RESPONSE_BUFFER_BYTES + CHUNK_FRAME_BYTES;
    buffer_ = static_cast<uint8_t *>(
        Memory::alloc_large(buffer_capacity_, false));
}

AsyncPreparedChunkedResponse::~AsyncPreparedChunkedResponse() {
    if (buffer_) Memory::free(buffer_);
}

bool AsyncPreparedChunkedResponse::_sourceValid() const {
    return static_cast<bool>(content_) && buffer_ &&
           buffer_capacity_ > CHUNK_FRAME_BYTES;
}

void AsyncPreparedChunkedResponse::_respond(AsyncWebServerRequest *request) {
    if (!request || !_sourceValid()) {
        if (request) fail(request);
        return;
    }

    _assembleHead(assembled_headers_, request->version());
    _state = RESPONSE_HEADERS;
    (void)_ack(request, 0, 0);
}

bool AsyncPreparedChunkedResponse::fill_chunk() {
    const size_t payload_capacity = buffer_capacity_ - CHUNK_FRAME_BYTES;
    const size_t filled = content_(buffer_ + 6, payload_capacity, _sentLength);
    if (filled == RESPONSE_TRY_AGAIN) return false;
    if (filled > payload_capacity) {
        source_failed_ = true;
        return false;
    }

    static constexpr char HEX_DIGITS[] = "0123456789abcdef";
    buffer_[0] = HEX_DIGITS[(filled >> 12) & 0x0f];
    buffer_[1] = HEX_DIGITS[(filled >> 8) & 0x0f];
    buffer_[2] = HEX_DIGITS[(filled >> 4) & 0x0f];
    buffer_[3] = HEX_DIGITS[filled & 0x0f];
    buffer_[4] = '\r';
    buffer_[5] = '\n';
    buffer_[filled + 6] = '\r';
    buffer_[filled + 7] = '\n';

    buffer_offset_ = 0;
    buffer_length_ = filled + CHUNK_FRAME_BYTES;
    final_chunk_ = filled == 0;
    _sentLength += filled;
    return true;
}

void AsyncPreparedChunkedResponse::fail(AsyncWebServerRequest *request) {
    _state = RESPONSE_FAILED;
    if (request && request->client()) request->client()->close();
}

size_t AsyncPreparedChunkedResponse::_ack(AsyncWebServerRequest *request,
                                          size_t len,
                                          uint32_t time) {
    (void)time;
    if (!request || !request->client() || !_sourceValid()) {
        if (request) fail(request);
        return 0;
    }

    _ackedLength += len;
    size_t queued = 0;

    if (_state == RESPONSE_HEADERS) {
        const size_t remaining =
            assembled_headers_.length() - written_headers_length_;
        const size_t added = request->client()->add(
            assembled_headers_.c_str() + written_headers_length_, remaining);
        written_headers_length_ += added;
        _writtenLength += added;
        queued += added;

        if (written_headers_length_ < assembled_headers_.length()) {
            if (queued > 0 && !request->client()->send()) fail(request);
            return queued;
        }

        assembled_headers_ = String();
        _state = RESPONSE_CONTENT;
    }

    while (_state == RESPONSE_CONTENT && request->client()->space() > 0) {
        if (buffer_offset_ < buffer_length_) {
            const size_t pending = buffer_length_ - buffer_offset_;
            const size_t added = request->client()->add(
                reinterpret_cast<const char *>(buffer_ + buffer_offset_),
                pending);
            if (added == 0) break;

            buffer_offset_ += added;
            _writtenLength += added;
            queued += added;
            if (buffer_offset_ < buffer_length_) break;

            buffer_offset_ = 0;
            buffer_length_ = 0;
            if (final_chunk_) {
                _state = RESPONSE_END;
                break;
            }
        }

        if (!fill_chunk()) {
            if (source_failed_) fail(request);
            break;
        }
    }

    if (queued > 0 && !request->client()->send()) fail(request);
    return queued;
}

}  // namespace aircannect
