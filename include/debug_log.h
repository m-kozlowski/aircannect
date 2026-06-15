#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#include <string>

enum log_level_t {
    LOG_ERROR = 0,
    LOG_WARN = 1,
    LOG_INFO = 2,
    LOG_DEBUG = 3,
};

enum log_cat_t {
    CAT_GENERAL = 0,
    CAT_CAN,
    CAT_RPC,
    CAT_TCP,
    CAT_CLI,
    CAT_WIFI,
    CAT_STREAM,
    CAT_OTA,
    CAT_OXI,
    CAT_STORAGE,
    CAT_BGWORKER,
    CAT_REPORT,
    CAT_EDF,
    CAT_CONFIG,
    CAT_COUNT,
};

namespace Log {

struct Stats {
    uint32_t emitted = 0;
    uint32_t filtered = 0;
    uint32_t truncated = 0;
    uint32_t syslog_enqueued = 0;
    uint32_t syslog_sent = 0;
    uint32_t syslog_drops = 0;
    uint32_t syslog_errors = 0;
};

void init();
void set_level(log_level_t level);
void set_cat_level(log_cat_t cat, log_level_t level);
log_level_t get_cat_level(log_cat_t cat);

const char *level_name(log_level_t level);
const char *cat_name(log_cat_t cat);
bool parse_level(String value, log_level_t &level);
bool parse_cat(String value, log_cat_t &cat);

void configure_syslog(bool enabled,
                      const String &host,
                      uint16_t port,
                      const String &hostname);
void poll(bool network_available);
bool syslog_enabled();
String syslog_host();
uint16_t syslog_port();
size_t syslog_queue_depth();

Stats stats();

void logf(log_cat_t cat, log_level_t level, const char *fmt, ...);
void log_payload(log_cat_t cat,
                 log_level_t level,
                 const char *prefix,
                 const std::string &payload);
void log_payload(log_cat_t cat,
                 log_level_t level,
                 const char *prefix,
                 const char *payload,
                 size_t payload_len);

}  // namespace Log
