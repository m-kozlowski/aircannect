#include "sleephq_protocol.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "string_util.h"

namespace aircannect {
namespace {

bool parse_u32_text(const char *text, uint32_t &out) {
    if (!text || !*text) return false;
    char *end = nullptr;
    const unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > UINT32_MAX) return false;
    out = static_cast<uint32_t>(value);
    return true;
}

bool json_string_to_u32(JsonVariantConst value, uint32_t &out) {
    if (value.is<uint32_t>()) {
        out = value.as<uint32_t>();
        return true;
    }
    if (value.is<const char *>()) {
        return parse_u32_text(value.as<const char *>(), out);
    }
    return false;
}

bool json_string_to_u64(JsonVariantConst value, uint64_t &out) {
    if (value.is<uint64_t>()) {
        out = value.as<uint64_t>();
        return true;
    }
    if (value.is<const char *>()) {
        const char *text = value.as<const char *>();
        if (!text || !*text) return false;
        char *end = nullptr;
        const unsigned long long parsed = strtoull(text, &end, 10);
        if (!end || *end != '\0') return false;
        out = static_cast<uint64_t>(parsed);
        return true;
    }
    return false;
}

const char *json_string_or_empty(JsonVariantConst value) {
    return value.is<const char *>() ? value.as<const char *>() : "";
}

void set_error(char *error, size_t error_size, const char *message) {
    copy_cstr(error, error_size, message ? message : "sleephq_json_error");
}

bool status_is_one_of(const char *status,
                      const char *const *values,
                      size_t count) {
    if (!status || !status[0]) return false;
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(status, values[i]) == 0) return true;
    }
    return false;
}

bool status_contains_failure_token(const char *status) {
    if (!status || !status[0]) return false;
    char lower[AC_SLEEPHQ_STATUS_MAX] = {};
    copy_cstr(lower, sizeof(lower), status);
    for (char *p = lower; *p; ++p) {
        *p = static_cast<char>(tolower(static_cast<unsigned char>(*p)));
    }
    return strstr(lower, "fail") || strstr(lower, "error") ||
           strstr(lower, "reject") || strstr(lower, "cancel");
}

}  // namespace

SleepHqImportStatusKind sleephq_classify_import_status(const char *status) {
    static const char *const SUCCESS[] = {
        "processed",
        "complete",
        "completed",
        "finished",
        "success",
        "succeeded",
    };
    static const char *const TRANSIENT[] = {
        "created",
        "new",
        "pending",
        "queued",
        "ready",
        "uploading",
        "uploaded",
        "unpacking",
        "processing",
        "importing",
    };
    static const char *const FAILURE[] = {
        "failed",
        "failure",
        "error",
        "errored",
        "rejected",
        "cancelled",
        "canceled",
    };

    if (status_is_one_of(status, SUCCESS,
                         sizeof(SUCCESS) / sizeof(SUCCESS[0]))) {
        return SleepHqImportStatusKind::Success;
    }
    if (status_is_one_of(status, TRANSIENT,
                         sizeof(TRANSIENT) / sizeof(TRANSIENT[0]))) {
        return SleepHqImportStatusKind::Transient;
    }
    if (status_is_one_of(status, FAILURE,
                         sizeof(FAILURE) / sizeof(FAILURE[0])) ||
        status_contains_failure_token(status)) {
        return SleepHqImportStatusKind::Failure;
    }
    return SleepHqImportStatusKind::Unknown;
}

bool sleephq_parse_remote_file_list_json(const char *json,
                                         uint32_t per_page,
                                         SleepHqRemoteFileCallback callback,
                                         void *ctx,
                                         size_t &count,
                                         bool &has_more,
                                         char *error,
                                         size_t error_size) {
    count = 0;
    has_more = false;
    if (!json || !callback || per_page == 0) {
        set_error(error, error_size, "bad_file_list_request");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        set_error(error, error_size, "file_list_json_parse");
        return false;
    }
    JsonArrayConst data = doc["data"].as<JsonArrayConst>();
    if (data.isNull()) {
        set_error(error, error_size, "file_list_missing");
        return false;
    }

    for (JsonVariantConst item : data) {
        JsonVariantConst attr = item["attributes"];
        SleepHqRemoteFile file;
        json_string_to_u32(attr["id"], file.id);
        json_string_to_u64(attr["size"], file.size);
        copy_cstr(file.name,
                  sizeof(file.name),
                  json_string_or_empty(attr["name"]));
        copy_cstr(file.path,
                  sizeof(file.path),
                  json_string_or_empty(attr["path"]));
        copy_cstr(file.content_hash,
                  sizeof(file.content_hash),
                  json_string_or_empty(attr["content_hash"]));
        if (file.id == 0) {
            json_string_to_u32(item["id"], file.id);
        }
        if (!callback(ctx, file)) {
            set_error(error, error_size, "file_list_callback_failed");
            return false;
        }
        count++;
    }
    has_more = count >= per_page;
    return true;
}

bool sleephq_parse_machine_list_json(const char *json,
                                     uint32_t per_page,
                                     SleepHqMachineCallback callback,
                                     void *ctx,
                                     size_t &count,
                                     bool &has_more,
                                     char *error,
                                     size_t error_size) {
    count = 0;
    has_more = false;
    if (!json || !callback || per_page == 0) {
        set_error(error, error_size, "bad_machine_list_request");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        set_error(error, error_size, "machine_list_json_parse");
        return false;
    }
    JsonArrayConst data = doc["data"].as<JsonArrayConst>();
    if (data.isNull()) {
        set_error(error, error_size, "machine_list_missing");
        return false;
    }

    for (JsonObjectConst item : data) {
        SleepHqMachine machine;
        JsonVariantConst id = item["id"];
        if (!json_string_to_u32(id, machine.id)) {
            id = item["attributes"]["id"];
            (void)json_string_to_u32(id, machine.id);
        }
        copy_cstr(machine.serial_number,
                  sizeof(machine.serial_number),
                  json_string_or_empty(
                      item["attributes"]["serial_number"]));
        if (!callback(ctx, machine)) {
            set_error(error, error_size, "machine_list_callback_failed");
            return false;
        }
        count++;
    }

    has_more = count >= per_page;
    set_error(error, error_size, "");
    return true;
}

bool sleephq_parse_machine_date_json(const char *json,
                                     SleepHqMachineDate &out,
                                     char *error,
                                     size_t error_size) {
    out = SleepHqMachineDate();
    if (!json) {
        set_error(error, error_size, "bad_machine_date_request");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        set_error(error, error_size, "machine_date_json_parse");
        return false;
    }

    JsonObjectConst data = doc["data"].as<JsonObjectConst>();
    if (data.isNull()) {
        set_error(error, error_size, "machine_date_missing");
        return false;
    }

    JsonVariantConst id = data["id"];
    if (!json_string_to_u32(id, out.id)) {
        id = data["attributes"]["id"];
        (void)json_string_to_u32(id, out.id);
    }
    (void)json_string_to_u32(data["attributes"]["machine_id"],
                             out.machine_id);
    copy_cstr(out.date,
              sizeof(out.date),
              json_string_or_empty(data["attributes"]["date"]));
    if (!out.id || !out.date[0]) {
        set_error(error, error_size, "machine_date_incomplete");
        return false;
    }
    set_error(error, error_size, "");
    return true;
}

}  // namespace aircannect
