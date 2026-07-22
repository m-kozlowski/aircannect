#include "line_protocol_server.h"

#include <errno.h>
#include <lwip/sockets.h>

#include "debug_log.h"

namespace aircannect {

bool LineProtocolServerBase::begin_line_server(uint16_t port,
                                               const char *label) {
    if (started_) return true;
    port_ = port;
    server_ = new WiFiServer(port_);
    if (!server_) return false;
    server_->begin();
    server_->setNoDelay(true);
    started_ = true;
    Log::logf(CAT_TCP, LOG_INFO, "[%s] listening on port %u\n",
              label, port_);
    return true;
}

void LineProtocolServerBase::stop_line_server() {
    if (server_) {
        delete server_;
        server_ = nullptr;
    }
    started_ = false;
}

WiFiClient LineProtocolServerBase::accept_line_client() {
    if (!server_) return WiFiClient();
    WiFiClient incoming = server_->accept();
    if (incoming) incoming.setNoDelay(true);
    return incoming;
}

size_t LineProtocolServerBase::write_line_nonblocking(WiFiClient &client,
                                                      size_t idx,
                                                      const char *label,
                                                      const uint8_t *data,
                                                      size_t len,
                                                      bool &fatal_error) {
    fatal_error = false;
    if (!data || len == 0) return 0;

    const int fd = client.fd();
    if (fd < 0) {
        fatal_error = true;
        return 0;
    }

    fd_set write_set;
    FD_ZERO(&write_set);
    FD_SET(fd, &write_set);

    struct timeval timeout = {};
    const int ready = select(fd + 1, nullptr, &write_set, nullptr, &timeout);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0;
        }
        fatal_error = true;
        Log::logf(CAT_TCP, LOG_WARN,
                  "[%s %u] write readiness failed errno=%d\n",
                  label, static_cast<unsigned>(idx), errno);
        return 0;
    }

    if (ready == 0 || !FD_ISSET(fd, &write_set)) {
        return 0;
    }

    const ssize_t sent = send(fd, data, len, MSG_DONTWAIT);
    if (sent > 0) return static_cast<size_t>(sent);

    if (sent == 0) {
        return 0;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return 0;
    }

    fatal_error = true;
    Log::logf(CAT_TCP, LOG_WARN, "[%s %u] write failed errno=%d\n",
              label, static_cast<unsigned>(idx), errno);
    return 0;
}

}  // namespace aircannect
