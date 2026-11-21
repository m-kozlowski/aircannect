#include "async_prepared_response.h"

#include <algorithm>
#include <utility>

#include "memory_manager.h"
#include "async_tcp_dispatch.h"

namespace aircannect {
namespace {

static constexpr size_t PREPARED_RESPONSE_BUFFER_BYTES = 4096;

}  // namespace

struct AsyncPreparedResponse::Wake : PreparedReadSignal, std::enable_shared_from_this<Wake> {
    struct Message { std::shared_ptr<Wake> wake; uint32_t generation; };
    AsyncPreparedResponse *owner = nullptr;
    AsyncWebServerRequest *request = nullptr;
    uint32_t generation = 0;
    std::atomic<uint32_t> waiting{0};

    void notify(uint32_t ticket) override {
        if (waiting.load() != ticket) return;
        auto *message = new (std::nothrow) Message{shared_from_this(), ticket};
        if (message && !ac_async_tcp_dispatch(resume, message)) delete message;
    }

    static void resume(void *context) {
        std::unique_ptr<Message> message(static_cast<Message *>(context));
        Wake &state = *message->wake;
        if (!state.owner || state.waiting != message->generation) return;
        state.owner->pump(state.request);
    }
};

AsyncPreparedResponse::AsyncPreparedResponse(
    int status_code,
    const char *content_type,
    size_t content_length,
    AwsResponseFiller content)
    : content_(std::move(content)) {
    _code = status_code;
    _contentType = content_type ? content_type : "";
    _contentLength = content_length;
    addHeader("Connection", "close", false);

    if (content_length > 0) {
        buffer_capacity_ = PREPARED_RESPONSE_BUFFER_BYTES;
        buffer_ = static_cast<uint8_t *>(
            Memory::alloc_large(buffer_capacity_, false));
    }
}

AsyncPreparedResponse::AsyncPreparedResponse(
    const char *content_type,
    size_t content_length,
    AwsResponseFiller content)
    : AsyncPreparedResponse(
          200, content_type, content_length, std::move(content)) {}

AsyncPreparedResponse::~AsyncPreparedResponse() {
    if (wake_) {
        wake_->waiting = 0;
        wake_->owner = nullptr;
        wake_->request = nullptr;
    }
    if (buffer_) Memory::free(buffer_);
}

AsyncPreparedResponse::AsyncPreparedResponse(
    const char *type, size_t length, StreamFiller stream)
    : AsyncPreparedResponse(200, type, length) {
    stream_ = std::move(stream);
}

bool AsyncPreparedResponse::_sourceValid() const {
    return _contentLength == 0 ||
        ((content_ || stream_) && buffer_ && buffer_capacity_ > 0);
}

void AsyncPreparedResponse::_respond(AsyncWebServerRequest *request) {
    if (!request || !_sourceValid()) {
        if (request) fail(request);
        return;
    }

    if (_contentLength > 0 && !fill_pending(request)) {
        fail(request);
        return;
    }

    _assembleHead(assembled_headers_, request->version());
    _state = RESPONSE_HEADERS;
    (void)_ack(request, 0, 0);
}

bool AsyncPreparedResponse::fill_pending(AsyncWebServerRequest *request) {
    if (buffer_length_ > buffer_offset_) return true;
    if (_sentLength >= _contentLength) return true;

    const size_t remaining = _contentLength - _sentLength;
    const size_t requested = std::min(buffer_capacity_, remaining);
    size_t filled;
    if (stream_) {
        const auto read = stream_(buffer_, requested, _sentLength);
        if (read.state == PreparedByteReadState::Retry && read.wait && _sentLength) {
            if (!wake_) {
                try { wake_ = std::make_shared<Wake>(); }
                catch (const std::bad_alloc &) { return false; } // ACK/poll fallback.
                wake_->owner = this;
                wake_->request = request;
            }

            if (!++wake_->generation) ++wake_->generation;
            wake_->waiting = wake_->generation;
            read.wait->arm(wake_, wake_->waiting);
        }
        filled = read.state == PreparedByteReadState::Retry ? RESPONSE_TRY_AGAIN : read.bytes;
    } else {
        filled = content_(buffer_, requested, _sentLength);
    }
    if (filled == RESPONSE_TRY_AGAIN) {
        if (_sentLength == 0) source_ended_ = true;
        return false;
    }
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
    // close() destroys the request inline, before its ACK callback returns.
    if (request && request->client()) request->client()->abort();
}

size_t AsyncPreparedResponse::_ack(AsyncWebServerRequest *request,
                                   size_t len,
                                   uint32_t time) {
    (void)time;
    _ackedLength += len;
    return pump(request);
}

size_t AsyncPreparedResponse::pump(AsyncWebServerRequest *request) {
    if (wake_) wake_->waiting = 0;
    if (!request || !request->client() || !_sourceValid()) {
        if (request) fail(request);
        return 0;
    }

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
            if (!fill_pending(request)) {
                if (source_ended_) {
                    fail(request);
                    return 0;
                }
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

}  // namespace aircannect
