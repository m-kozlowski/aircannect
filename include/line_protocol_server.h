#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <algorithm>
#include <stdint.h>

#include "board.h"
#include "fixed_queue.h"

namespace aircannect {

struct LineProtocolIoStats {
    uint32_t bytes_out = 0;
    uint32_t write_errors = 0;
};

struct LineOutputPumpResult {
    bool fatal_error = false;
    bool completed = false;
    size_t written = 0;
};

class LineProtocolServerBase {
protected:
    bool begin_line_server(uint16_t port, const char *label);
    void stop_line_server();
    WiFiClient accept_line_client();

    bool line_server_started() const { return started_; }
    uint16_t line_server_port() const { return port_; }
    const LineProtocolIoStats &line_io_stats() const { return io_stats_; }

    template <size_t QueueDepth>
    LineOutputPumpResult pump_line_output(WiFiClient &client,
                                          FixedQueue<String, QueueDepth> &queue,
                                          String &current,
                                          size_t &pos,
                                          size_t idx,
                                          const char *label,
                                          bool append_lf) {
        LineOutputPumpResult result;
        if (!client || !client.connected()) return result;

        if (!current.length()) {
            String next;
            if (!queue.pop(next)) return result;
            current = next;
            if (append_lf) current += '\n';
            pos = 0;
        }

        if (pos >= current.length()) {
            current = "";
            pos = 0;
            result.completed = true;
            return result;
        }

        const size_t remaining = current.length() - pos;
        const size_t chunk = std::min<size_t>(remaining, AC_TCP_WRITE_CHUNK);
        if (chunk == 0) return result;

        const uint8_t *data = reinterpret_cast<const uint8_t *>(
            current.c_str() + pos);
        result.written = write_line_nonblocking(client, idx, label, data,
                                                chunk, result.fatal_error);
        if (result.fatal_error || result.written == 0) return result;

        pos += result.written;
        io_stats_.bytes_out += result.written;
        if (pos >= current.length()) {
            current = "";
            pos = 0;
            result.completed = true;
        }
        return result;
    }

private:
    size_t write_line_nonblocking(WiFiClient &client,
                                  size_t idx,
                                  const char *label,
                                  const uint8_t *data,
                                  size_t len,
                                  bool &fatal_error);

    WiFiServer *server_ = nullptr;
    bool started_ = false;
    uint16_t port_ = 0;
    LineProtocolIoStats io_stats_ = {};
};

}  // namespace aircannect
