#include "debug_log.h"

#include <IPAddress.h>
#include <WiFiUdp.h>
#include <stdarg.h>
#include <string.h>

#include "board.h"
#include "fixed_queue.h"
#include "string_util.h"

using aircannect::FixedQueue;
using aircannect::to_lower_inplace;
using aircannect::trim_inplace;

namespace {

log_level_t levels[CAT_COUNT];
WiFiUDP syslog_udp;

struct LogRecord {
    uint32_t ms = 0;
    log_cat_t cat = CAT_GENERAL;
    log_level_t level = LOG_INFO;
    char text[AC_LOG_LINE_MAX] = {};
};

FixedQueue<LogRecord, AC_SYSLOG_QUEUE_DEPTH> syslog_queue;
Log::Stats log_stats;
IPAddress syslog_ip;
String syslog_host_text;
uint16_t syslog_port_value = AC_SYSLOG_PORT;
bool syslog_enabled_value = false;
char syslog_hostname[64] = "aircannect";

int syslog_severity(log_level_t level) {
    switch (level) {
        case LOG_ERROR: return 3;
        case LOG_WARN: return 4;
        case LOG_INFO: return 6;
        case LOG_DEBUG: return 7;
        default: return 6;
    }
}

void clean_message(char *text) {
    if (!text) return;
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\r' || text[len - 1] == '\n')) {
        text[--len] = 0;
    }
}

int format_message(char *buf, size_t size, const char *fmt, va_list args) {
    if (!buf || size == 0) return 0;
    int len = vsnprintf(buf, size, fmt, args);
    if (len <= 0) return 0;
    if (len >= static_cast<int>(size)) {
        len = size - 1;
        log_stats.truncated++;
    }
    return len;
}

void serial_dispatch(const char *buf, int len) {
    if (!buf || len <= 0) return;
    Serial.write(reinterpret_cast<const uint8_t *>(buf), len);
}

void enqueue_syslog(log_cat_t cat, log_level_t level, const char *buf) {
    if (!syslog_enabled_value || !buf) return;
    LogRecord record;
    record.ms = millis();
    record.cat = cat;
    record.level = level;
    strncpy(record.text, buf, sizeof(record.text) - 1);
    record.text[sizeof(record.text) - 1] = 0;
    clean_message(record.text);
    if (!record.text[0]) return;
    if (syslog_queue.push(record)) {
        log_stats.syslog_enqueued++;
    }
}

void dispatch_structured(log_cat_t cat,
                         log_level_t level,
                         const char *buf,
                         int len) {
    log_stats.emitted++;
    serial_dispatch(buf, len);
    enqueue_syslog(cat, level, buf);
}

void send_syslog_record(const LogRecord &record) {
    char payload[AC_LOG_LINE_MAX + 96];
    const int facility_local0 = 16;
    const int pri = facility_local0 * 8 + syslog_severity(record.level);
    snprintf(payload, sizeof(payload),
             "<%d>1 - %s aircannect - - - [%s %s uptime_ms=%lu] %s",
             pri,
             syslog_hostname,
             Log::level_name(record.level),
             Log::cat_name(record.cat),
             static_cast<unsigned long>(record.ms),
             record.text);

    if (!syslog_udp.beginPacket(syslog_ip, syslog_port_value)) {
        log_stats.syslog_errors++;
        return;
    }
    syslog_udp.write(reinterpret_cast<const uint8_t *>(payload),
                     strlen(payload));
    if (!syslog_udp.endPacket()) {
        log_stats.syslog_errors++;
        return;
    }
    log_stats.syslog_sent++;
}

}  // namespace

namespace Log {

void init() {
    for (int i = 0; i < CAT_COUNT; ++i) levels[i] = LOG_INFO;
}

void set_level(log_level_t level) {
    if (level < LOG_ERROR || level > LOG_DEBUG) return;
    for (int i = 0; i < CAT_COUNT; ++i) levels[i] = level;
}

void set_cat_level(log_cat_t cat, log_level_t level) {
    if (cat >= 0 && cat < CAT_COUNT &&
        level >= LOG_ERROR && level <= LOG_DEBUG) {
        levels[cat] = level;
    }
}

log_level_t get_cat_level(log_cat_t cat) {
    if (cat >= 0 && cat < CAT_COUNT) return levels[cat];
    return LOG_INFO;
}

const char *level_name(log_level_t level) {
    switch (level) {
        case LOG_ERROR: return "ERROR";
        case LOG_WARN: return "WARN";
        case LOG_INFO: return "INFO";
        case LOG_DEBUG: return "DEBUG";
        default: return "?";
    }
}

const char *cat_name(log_cat_t cat) {
    switch (cat) {
        case CAT_GENERAL: return "GENERAL";
        case CAT_CAN: return "CAN";
        case CAT_RPC: return "RPC";
        case CAT_TCP: return "TCP";
        case CAT_CLI: return "CLI";
        case CAT_WIFI: return "WIFI";
        case CAT_STREAM: return "STREAM";
        case CAT_OTA: return "OTA";
        default: return "?";
    }
}

bool parse_level(String value, log_level_t &level) {
    trim_inplace(value);
    to_lower_inplace(value);
    if (value == "error" || value == "err" || value == "0") {
        level = LOG_ERROR;
        return true;
    }
    if (value == "warn" || value == "warning" || value == "1") {
        level = LOG_WARN;
        return true;
    }
    if (value == "info" || value == "2") {
        level = LOG_INFO;
        return true;
    }
    if (value == "debug" || value == "dbg" || value == "3") {
        level = LOG_DEBUG;
        return true;
    }
    return false;
}

bool parse_cat(String value, log_cat_t &cat) {
    trim_inplace(value);
    to_lower_inplace(value);
    if (value == "general" || value == "gen") {
        cat = CAT_GENERAL;
        return true;
    }
    if (value == "can") {
        cat = CAT_CAN;
        return true;
    }
    if (value == "rpc") {
        cat = CAT_RPC;
        return true;
    }
    if (value == "tcp" || value == "telnet") {
        cat = CAT_TCP;
        return true;
    }
    if (value == "cli" || value == "console") {
        cat = CAT_CLI;
        return true;
    }
    if (value == "wifi" || value == "wi-fi") {
        cat = CAT_WIFI;
        return true;
    }
    if (value == "stream") {
        cat = CAT_STREAM;
        return true;
    }
    if (value == "ota") {
        cat = CAT_OTA;
        return true;
    }
    return false;
}

void configure_syslog(bool enabled,
                      const String &host,
                      uint16_t port,
                      const String &hostname) {
    syslog_enabled_value = false;
    syslog_queue.clear();
    syslog_host_text = "";
    syslog_port_value = port ? port : AC_SYSLOG_PORT;
    if (hostname.length()) {
        strncpy(syslog_hostname, hostname.c_str(), sizeof(syslog_hostname) - 1);
        syslog_hostname[sizeof(syslog_hostname) - 1] = 0;
    }
    if (!enabled) return;

    IPAddress parsed;
    if (!parsed.fromString(host)) return;
    syslog_ip = parsed;
    syslog_host_text = host;
    syslog_enabled_value = true;
}

void poll(bool network_available) {
    if (!syslog_enabled_value || !network_available) return;
    for (size_t i = 0; i < AC_SYSLOG_SEND_BUDGET; ++i) {
        LogRecord record;
        if (!syslog_queue.pop(record)) return;
        send_syslog_record(record);
    }
}

bool syslog_enabled() {
    return syslog_enabled_value;
}

String syslog_host() {
    return syslog_host_text;
}

uint16_t syslog_port() {
    return syslog_port_value;
}

Stats stats() {
    Stats out = log_stats;
    out.syslog_drops = syslog_queue.dropped();
    return out;
}

void print_status(Print &out) {
    out.print("[LOG] levels");
    for (int i = 0; i < CAT_COUNT; ++i) {
        out.print(' ');
        out.print(cat_name(static_cast<log_cat_t>(i)));
        out.print('=');
        out.print(level_name(levels[i]));
    }
    out.println();
    out.print("[SYSLOG] enabled=");
    out.print(syslog_enabled_value ? "yes" : "no");
    out.print(" host=");
    out.print(syslog_host_text.length() ? syslog_host_text : "--");
    out.print(" port=");
    out.print(syslog_port_value);
    out.print(" queued=");
    out.print(syslog_queue.count());
    out.print(" sent=");
    out.print(log_stats.syslog_sent);
    out.print(" drops=");
    out.print(syslog_queue.dropped());
    out.print(" errors=");
    out.println(log_stats.syslog_errors);
}

void print_stats(Print &out) {
    Stats s = stats();
    out.print(" log_emitted=");
    out.print(s.emitted);
    out.print(" log_filtered=");
    out.print(s.filtered);
    out.print(" log_truncated=");
    out.print(s.truncated);
    out.print(" syslog_enabled=");
    out.print(syslog_enabled_value ? "yes" : "no");
    out.print(" syslog_q=");
    out.print(syslog_queue.count());
    out.print(" syslog_enqueued=");
    out.print(s.syslog_enqueued);
    out.print(" syslog_sent=");
    out.print(s.syslog_sent);
    out.print(" syslog_drops=");
    out.print(s.syslog_drops);
    out.print(" syslog_errors=");
    out.print(s.syslog_errors);
}

void logf(log_cat_t cat, log_level_t level, const char *fmt, ...) {
    if (cat < 0 || cat >= CAT_COUNT ||
        level < LOG_ERROR || level > LOG_DEBUG ||
        level > levels[cat]) {
        log_stats.filtered++;
        return;
    }
    char buf[AC_LOG_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    const int len = format_message(buf, sizeof(buf), fmt, args);
    va_end(args);
    dispatch_structured(cat, level, buf, len);
}

void log_payload(log_cat_t cat,
                 log_level_t level,
                 const char *prefix,
                 const std::string &payload) {
    if (cat < 0 || cat >= CAT_COUNT ||
        level < LOG_ERROR || level > LOG_DEBUG ||
        level > levels[cat]) {
        log_stats.filtered++;
        return;
    }

    if (!prefix) prefix = "";
    log_stats.emitted++;
    serial_dispatch(prefix, strlen(prefix));
    if (!payload.empty()) {
        serial_dispatch(payload.data(), static_cast<int>(payload.size()));
    }
    serial_dispatch("\n", 1);

    if (!syslog_enabled_value) return;
    char record[AC_LOG_LINE_MAX];
    const size_t prefix_len = strlen(prefix);
    const size_t room =
        prefix_len < sizeof(record) ? sizeof(record) - prefix_len - 1 : 0;
    const int len = snprintf(record, sizeof(record), "%s%.*s",
                             prefix,
                             static_cast<int>(room),
                             payload.c_str());
    if (len >= static_cast<int>(sizeof(record))) log_stats.truncated++;
    record[sizeof(record) - 1] = 0;
    enqueue_syslog(cat, level, record);
}

}  // namespace Log
