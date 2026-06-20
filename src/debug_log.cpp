#include "debug_log.h"

#include <IPAddress.h>
#include <WiFiUdp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <string.h>

#include "board.h"
#include "fixed_queue.h"
#include "memory_manager.h"
#include "storage_manager.h"
#include "storage_writer.h"
#include "string_util.h"

using aircannect::FixedQueue;
using aircannect::to_lower_inplace;
using aircannect::trim_inplace;

namespace {

log_level_t levels[CAT_COUNT];
WiFiUDP syslog_udp;
StaticSemaphore_t log_mutex_storage;
SemaphoreHandle_t log_mutex = nullptr;

struct LogRecord {
    log_cat_t cat = CAT_GENERAL;
    log_level_t level = LOG_INFO;
    char text[AC_LOG_LINE_MAX] = {};
};

using SyslogQueue = FixedQueue<LogRecord, AC_SYSLOG_QUEUE_DEPTH>;
using FileLogQueue = FixedQueue<LogRecord, AC_FILE_LOG_QUEUE_DEPTH>;

SyslogQueue *syslog_queue = nullptr;
FileLogQueue *file_log_queue = nullptr;
Log::Stats log_stats;
IPAddress syslog_ip;
String syslog_host_text;
uint16_t syslog_port_value = AC_SYSLOG_PORT;
bool syslog_enabled_value = false;
bool file_log_dir_ready = false;
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

size_t append_bytes(char *buf,
                    size_t size,
                    size_t pos,
                    const char *text,
                    size_t len) {
    if (!buf || size == 0 || !text || len == 0) return pos;
    if (pos < size - 1) {
        const size_t room = size - 1 - pos;
        const size_t copy_len = len < room ? len : room;
        memcpy(buf + pos, text, copy_len);
        buf[pos + copy_len] = 0;
    }
    return pos + len;
}

size_t append_text(char *buf, size_t size, size_t pos, const char *text) {
    return append_bytes(buf, size, pos, text, text ? strlen(text) : 0);
}

size_t append_char(char *buf, size_t size, size_t pos, char value) {
    return append_bytes(buf, size, pos, &value, 1);
}

int compose_line(log_cat_t cat,
                 log_level_t level,
                 const char *message,
                 bool payload_follows,
                 char *buf,
                 size_t size) {
    if (!buf || size == 0) return 0;
    buf[0] = 0;

    char prefix[48];
    const int prefix_len = snprintf(prefix, sizeof(prefix), "[%s][%s]",
                                    Log::level_name(level),
                                    Log::cat_name(cat));
    const size_t prefix_bytes =
        prefix_len > 0
            ? (prefix_len < static_cast<int>(sizeof(prefix))
                   ? static_cast<size_t>(prefix_len)
                   : sizeof(prefix) - 1)
            : 0;
    size_t pos = append_bytes(buf,
                              size,
                              0,
                              prefix,
                              prefix_bytes);

    const char *text = message ? message : "";
    if (text[0]) {
        if (text[0] != '[') pos = append_char(buf, size, pos, ' ');
        pos = append_text(buf, size, pos, text);
    } else if (payload_follows) {
        pos = append_char(buf, size, pos, ' ');
    }

    if (pos >= size) {
        log_stats.truncated++;
        return static_cast<int>(size - 1);
    }
    return static_cast<int>(pos);
}

void serial_dispatch(const char *buf, int len) {
    if (!buf || len <= 0) return;
    Serial.write(reinterpret_cast<const uint8_t *>(buf), len);
}

bool ensure_syslog_queue() {
    if (syslog_queue) return true;
    void *memory = aircannect::Memory::alloc_large(sizeof(SyslogQueue));
    if (!memory) {
        log_stats.syslog_errors++;
        return false;
    }
    syslog_queue = new (memory) SyslogQueue();
    return true;
}

void release_syslog_queue() {
    if (!syslog_queue) return;
    syslog_queue->~SyslogQueue();
    aircannect::Memory::free(syslog_queue);
    syslog_queue = nullptr;
}

bool make_log_record(log_cat_t cat,
                     log_level_t level,
                     const char *buf,
                     LogRecord &record) {
    if (!buf) return false;
    record.cat = cat;
    record.level = level;
    strncpy(record.text, buf, sizeof(record.text) - 1);
    record.text[sizeof(record.text) - 1] = 0;
    clean_message(record.text);
    return record.text[0] != 0;
}

void enqueue_syslog_record(const LogRecord &record) {
    if (!syslog_enabled_value) return;
    if (!syslog_queue) {
        log_stats.syslog_drops++;
        return;
    }
    if (syslog_queue->push(record)) {
        log_stats.syslog_enqueued++;
    }
}

bool ensure_file_log_queue() {
#if AC_FILE_LOG_ENABLED
    if (file_log_queue) return true;
    void *memory = aircannect::Memory::alloc_large(sizeof(FileLogQueue));
    if (!memory) {
        log_stats.file_errors++;
        return false;
    }
    file_log_queue = new (memory) FileLogQueue();
    return true;
#else
    return false;
#endif
}

bool ensure_file_log_dir() {
#if AC_FILE_LOG_ENABLED
    if (file_log_dir_ready) return true;
    if (!aircannect::Storage::mounted()) return false;
    if (!aircannect::Storage::ensure_dir("/aircannect") ||
        !aircannect::Storage::ensure_dir(AC_FILE_LOG_DIR)) {
        log_stats.file_errors++;
        return false;
    }
    file_log_dir_ready = true;
    return true;
#else
    return false;
#endif
}

void enqueue_file_log_record(const LogRecord &record) {
#if AC_FILE_LOG_ENABLED
    if (!file_log_queue) return;
    if (file_log_queue->push(record)) {
        log_stats.file_enqueued++;
    }
#else
    (void)record;
#endif
}

void enqueue_log_sinks(log_cat_t cat, log_level_t level, const char *buf) {
    if (!syslog_enabled_value && !file_log_queue) return;
    LogRecord record;
    if (!make_log_record(cat, level, buf, record)) return;
    enqueue_syslog_record(record);
    enqueue_file_log_record(record);
}

void dispatch_structured(log_cat_t cat,
                         log_level_t level,
                         const char *buf,
                         int len,
                         const char *syslog_text) {
    log_stats.emitted++;
    serial_dispatch(buf, len);
    enqueue_log_sinks(cat, level, syslog_text);
}

void lock_log() {
    if (log_mutex) {
        xSemaphoreTake(log_mutex, portMAX_DELAY);
    }
}

void unlock_log() {
    if (log_mutex) {
        xSemaphoreGive(log_mutex);
    }
}

void send_syslog_record(const LogRecord &record) {
    char payload[AC_LOG_LINE_MAX + 96];
    const int facility_local0 = 16;
    const int pri = facility_local0 * 8 + syslog_severity(record.level);
    snprintf(payload, sizeof(payload),
             "<%d>1 - %s aircannect - - - - %s",
             pri,
             syslog_hostname,
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

void poll_file_log(size_t max_records) {
#if AC_FILE_LOG_ENABLED
    if (max_records == 0) max_records = AC_FILE_LOG_DRAIN_BUDGET;
    lock_log();
    const bool empty = !file_log_queue || file_log_queue->empty();
    unlock_log();
    if (empty) return;
    if (!ensure_file_log_dir()) return;

    for (size_t i = 0; i < max_records; ++i) {
        LogRecord record;
        lock_log();
        const bool have_record = file_log_queue &&
                                 file_log_queue->pop(record);
        unlock_log();
        if (!have_record) return;

        char line[AC_LOG_LINE_MAX + 2] = {};
        const size_t len = strnlen(record.text, sizeof(record.text));
        memcpy(line, record.text, len);
        line[len] = '\n';
        line[len + 1] = 0;

        if (aircannect::StorageWriter::enqueue_rotating_append(
                AC_FILE_LOG_PATH,
                reinterpret_cast<const uint8_t *>(line),
                len + 1,
                AC_FILE_LOG_ROTATE_BYTES,
                AC_FILE_LOG_ARCHIVES,
                false)) {
            log_stats.file_dequeued++;
            continue;
        }

        log_stats.file_backpressure++;
        lock_log();
        if (!file_log_queue || !file_log_queue->push_front(record)) {
            log_stats.file_drops++;
        }
        unlock_log();
        return;
    }
#else
    (void)max_records;
#endif
}

}  // namespace

namespace Log {

void init() {
    if (!log_mutex) {
        log_mutex = xSemaphoreCreateMutexStatic(&log_mutex_storage);
    }
    for (int i = 0; i < CAT_COUNT; ++i) levels[i] = LOG_INFO;
    lock_log();
    ensure_file_log_queue();
    unlock_log();
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
        case CAT_OXI: return "OXI";
        case CAT_STORAGE: return "STORAGE";
        case CAT_BGWORKER: return "BGWORKER";
        case CAT_REPORT: return "REPORT";
        case CAT_EDF: return "EDF";
        case CAT_CONFIG: return "CONFIG";
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
    if (value == "oxi" || value == "oximetry") {
        cat = CAT_OXI;
        return true;
    }
    if (value == "storage") {
        cat = CAT_STORAGE;
        return true;
    }
    if (value == "bgworker") {
        cat = CAT_BGWORKER;
        return true;
    }
    if (value == "report") {
        cat = CAT_REPORT;
        return true;
    }
    if (value == "edf") {
        cat = CAT_EDF;
        return true;
    }
    if (value == "config") {
        cat = CAT_CONFIG;
        return true;
    }
    return false;
}

void configure_syslog(bool enabled,
                      const String &host,
                      uint16_t port,
                      const String &hostname) {
    lock_log();
    syslog_enabled_value = false;
    release_syslog_queue();
    syslog_host_text = "";
    syslog_port_value = port ? port : AC_SYSLOG_PORT;
    if (hostname.length()) {
        strncpy(syslog_hostname, hostname.c_str(), sizeof(syslog_hostname) - 1);
        syslog_hostname[sizeof(syslog_hostname) - 1] = 0;
    }
    if (!enabled) {
        unlock_log();
        return;
    }

    IPAddress parsed;
    if (!parsed.fromString(host)) {
        unlock_log();
        return;
    }
    if (!ensure_syslog_queue()) {
        unlock_log();
        return;
    }
    syslog_ip = parsed;
    syslog_host_text = host;
    syslog_enabled_value = true;
    unlock_log();
}

void poll(bool network_available,
          bool storage_draining_allowed,
          size_t file_log_drain_budget) {
    if (storage_draining_allowed) {
        poll_file_log(file_log_drain_budget);
    }
    if (!syslog_enabled_value || !network_available) return;
    for (size_t i = 0; i < AC_SYSLOG_SEND_BUDGET; ++i) {
        LogRecord record;
        lock_log();
        const bool have_record = syslog_enabled_value &&
                                 syslog_queue &&
                                 syslog_queue->pop(record);
        unlock_log();
        if (!have_record) return;
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

size_t syslog_queue_depth() {
    lock_log();
    const size_t out = syslog_queue ? syslog_queue->count() : 0;
    unlock_log();
    return out;
}

bool filelog_enabled() {
#if AC_FILE_LOG_ENABLED
    return true;
#else
    return false;
#endif
}

const char *filelog_path() {
#if AC_FILE_LOG_ENABLED
    return AC_FILE_LOG_PATH;
#else
    return "";
#endif
}

size_t filelog_queue_depth() {
    lock_log();
    const size_t out = file_log_queue ? file_log_queue->count() : 0;
    unlock_log();
    return out;
}

Stats stats() {
    lock_log();
    Stats out = log_stats;
    if (syslog_queue) out.syslog_drops += syslog_queue->dropped();
    if (file_log_queue) out.file_drops += file_log_queue->dropped();
    unlock_log();
    return out;
}

void logf(log_cat_t cat, log_level_t level, const char *fmt, ...) {
    if (cat < 0 || cat >= CAT_COUNT ||
        level < LOG_ERROR || level > LOG_DEBUG ||
        level > levels[cat]) {
        log_stats.filtered++;
        return;
    }
    char buf[AC_LOG_LINE_MAX] = {};
    va_list args;
    va_start(args, fmt);
    format_message(buf, sizeof(buf), fmt, args);
    va_end(args);

    char line[AC_LOG_LINE_MAX];
    const int len = compose_line(cat, level, buf, false, line, sizeof(line));
    lock_log();
    dispatch_structured(cat, level, line, len, line);
    unlock_log();
}

void log_payload(log_cat_t cat,
                 log_level_t level,
                 const char *prefix,
                 const std::string &payload) {
    log_payload(cat, level, prefix, payload.data(), payload.size());
}

void log_payload(log_cat_t cat,
                 log_level_t level,
                 const char *prefix,
                 const char *payload,
                 size_t payload_len) {
    if (cat < 0 || cat >= CAT_COUNT ||
        level < LOG_ERROR || level > LOG_DEBUG ||
        level > levels[cat]) {
        log_stats.filtered++;
        return;
    }

    if (!prefix) prefix = "";
    char header[AC_LOG_LINE_MAX];
    const int header_len = compose_line(cat,
                                        level,
                                        prefix,
                                        payload && payload_len,
                                        header,
                                        sizeof(header));

    lock_log();
    log_stats.emitted++;
    serial_dispatch(header, header_len);
    if (payload && payload_len) {
        serial_dispatch(payload, static_cast<int>(payload_len));
    }
    serial_dispatch("\n", 1);

    char record[AC_LOG_LINE_MAX] = {};
    const size_t prefix_len = append_bytes(record,
                                           sizeof(record),
                                           0,
                                           header,
                                           static_cast<size_t>(header_len));
    const size_t room =
        prefix_len < sizeof(record) ? sizeof(record) - prefix_len - 1 : 0;
    const size_t payload_room =
        payload_len < room ? payload_len : room;
    if (payload && payload_room) {
        memcpy(record + prefix_len, payload, payload_room);
        record[prefix_len + payload_room] = 0;
    }
    if (payload_len > payload_room) log_stats.truncated++;
    record[sizeof(record) - 1] = 0;
    enqueue_log_sinks(cat, level, record);
    unlock_log();
}

}  // namespace Log
