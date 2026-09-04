#include "crash_http_controller.h"

#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include "async_prepared_response.h"
#include "crash_diagnostics.h"
#include "http_response_utils.h"
#include "json_util.h"
#include "large_byte_buffer.h"
#include "large_text_buffer.h"

namespace aircannect {
namespace {

void append_hex_address(LargeTextBuffer &json, uint32_t address) {
    char text[13] = {};
    snprintf(text, sizeof(text), "\"0x%08lx\"",
             static_cast<unsigned long>(address));
    json += text;
}

bool build_status_json(const CrashDiagnosticsSnapshot &snapshot,
                       LargeTextBuffer &json) {
    json = "{";
    json_add_bool(json, "ok", true, false);
    json_add_string(json, "state",
                    crash_dump_state_name(snapshot.dump_state));
    json_add_string(json, "relation",
                    crash_dump_relation_name(snapshot.dump_relation));
    json_add_int(json, "size", static_cast<long>(snapshot.dump_size));
    json_add_int(json, "stored_size",
                 static_cast<long>(snapshot.dump_stored_size));
    json_add_string(json, "error", snapshot.dump_error);
    json_add_bool(json, "summary_available", snapshot.summary_available);
    json_add_string(json, "occurred_at", snapshot.occurred_at);

    if (snapshot.summary_available) {
        json_add_string(json, "task", snapshot.task);
        json += ",\"pc\":";
        append_hex_address(json, snapshot.exception_pc);
        json_add_int(json, "cause",
                     static_cast<long>(snapshot.exception_cause));
        json += ",\"exception_address\":";
        append_hex_address(json, snapshot.exception_address);
        json_add_string(json, "reason", snapshot.reason);
        json_add_string(json, "elf_sha", snapshot.elf_sha);
        json_add_bool(json, "backtrace_corrupt",
                      snapshot.backtrace_corrupt);
        json += ",\"backtrace\":[";
        for (size_t i = 0; i < snapshot.backtrace_depth; ++i) {
            if (i) json += ',';
            append_hex_address(json, snapshot.backtrace[i]);
        }
        json += ']';
    }

    json += ",\"rtc\":{";
    json_add_bool(json, "available", snapshot.rtc_panic_available, false);
    if (snapshot.rtc_panic_available) {
        json_add_string(json, "source", snapshot.rtc_task_watchdog
                                           ? "task_watchdog"
                                           : "panic");
        json_add_string(json, "firmware", snapshot.rtc_firmware);
        json_add_int(json, "core", snapshot.rtc_core);
        json += ",\"pc\":";
        append_hex_address(json, snapshot.rtc_pc);
        json_add_bool(json, "backtrace_corrupt",
                      snapshot.rtc_backtrace_corrupt);
        json_add_bool(json, "backtrace_continues",
                      snapshot.rtc_backtrace_continues);
        json_add_string(json, "detail", snapshot.rtc_detail);
        json += ",\"backtrace\":[";
        for (size_t i = 0; i < snapshot.rtc_backtrace_depth; ++i) {
            if (i) json += ',';
            append_hex_address(json, snapshot.rtc_backtrace[i]);
        }
        json += ']';
    }
    json += "}}";
    return !json.overflowed();
}

}  // namespace

void CrashHttpController::register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/api/crash"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_status(request);
    });

    server.on(AsyncURIMatcher::exact("/api/crash/dump"), HTTP_GET,
              [this](AsyncWebServerRequest *request) {
        send_dump(request);
    });
}

void CrashHttpController::send_status(
    AsyncWebServerRequest *request) const {
    CrashDiagnosticsSnapshot snapshot;
    if (!diagnostics_.copy_snapshot(snapshot)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"busy\"}");
        return;
    }

    LargeTextBuffer json;
    if (!json.reserve(1024) || !build_status_json(snapshot, json)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }

    AsyncResponseStream *response = nullptr;
    if (!http_prepare_json_response(request, json, response)) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void CrashHttpController::send_dump(AsyncWebServerRequest *request) const {
    char error[AC_CRASH_ERROR_MAX] = {};
    const std::shared_ptr<const LargeByteBuffer> dump =
        diagnostics_.copy_dump(error, sizeof(error));
    if (!dump) {
        char body[128] = {};
        snprintf(body, sizeof(body),
                 "{\"ok\":false,\"error\":\"%s\"}",
                 error[0] ? error : "unavailable");
        request->send(404, "application/json", body);
        return;
    }

    AsyncWebServerResponse *response = new (std::nothrow) AsyncPreparedResponse(
        "application/octet-stream", dump->size(),
        [dump](uint8_t *buffer, size_t max_length, size_t offset) -> size_t {
            if (!buffer || offset >= dump->size()) return 0;

            const size_t bytes = std::min(max_length, dump->size() - offset);
            memcpy(buffer, dump->data() + offset, bytes);
            return bytes;
        });
    if (!response) {
        request->send(503, "application/json",
                      "{\"ok\":false,\"error\":\"response_alloc\"}");
        return;
    }

    response->addHeader(
        "Content-Disposition",
        "attachment; filename=\"aircannect-coredump.elf\"");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Accept-Ranges", "none");
    request->send(response);
}

}  // namespace aircannect
