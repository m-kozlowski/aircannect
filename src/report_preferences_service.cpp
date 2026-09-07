#include "report_preferences_service.h"

#include <ArduinoJson.h>

#include <stdio.h>
#include <string.h>

#include "debug_log.h"
#include "json_util.h"
#include "storage_read_port.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr const char *REPORT_PREFERENCES_PATH =
    "/aircannect/report/preferences.json";
constexpr size_t REPORT_PREFERENCES_MAX_BYTES = 2048;
constexpr size_t REPORT_PREFERENCES_JSON_RESERVE = 3072;
constexpr size_t REPORT_PREFERENCES_MAX_KEYS = 32;
constexpr size_t REPORT_PREFERENCES_MAX_KEY_LENGTH = 31;

bool valid_key(const char *key) {
    if (!key) return false;

    const size_t length = strlen(key);
    if (length == 0 || length > REPORT_PREFERENCES_MAX_KEY_LENGTH) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        const char ch = key[i];
        if ((ch < 'a' || ch > 'z') &&
            (ch < '0' || ch > '9') && ch != '_') {
            return false;
        }
    }
    return true;
}

bool valid_key_array(JsonArrayConst values) {
    if (values.isNull() || values.size() > REPORT_PREFERENCES_MAX_KEYS) {
        return false;
    }

    size_t index = 0;
    for (JsonVariantConst value : values) {
        if (!value.is<const char *>() || !valid_key(value.as<const char *>())) {
            return false;
        }

        size_t previous = 0;
        for (JsonVariantConst candidate : values) {
            if (previous++ >= index) break;
            if (strcmp(value.as<const char *>(),
                       candidate.as<const char *>()) == 0) {
                return false;
            }
        }
        index++;
    }
    return true;
}

bool array_is_subset(JsonArrayConst values, JsonArrayConst order) {
    for (JsonVariantConst value : values) {
        bool found = false;
        for (JsonVariantConst candidate : order) {
            if (strcmp(value.as<const char *>(),
                       candidate.as<const char *>()) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

void append_key_array(LargeTextBuffer &out, JsonArrayConst values) {
    out += '[';
    size_t index = 0;
    for (JsonVariantConst value : values) {
        if (index++) out += ',';
        out += '"';
        const char *key = value.as<const char *>();
        append_json_escaped(out, key, strlen(key));
        out += '"';
    }
    out += ']';
}

bool canonicalize(const uint8_t *bytes,
                  size_t length,
                  bool update,
                  uint32_t assigned_revision,
                  uint32_t &base_revision,
                  uint32_t &stored_revision,
                  LargeTextBuffer &out,
                  const char *&error) {
    JsonDocument document;
    const DeserializationError parse_error =
        deserializeJson(document, bytes, length);
    if (parse_error || !document.is<JsonObjectConst>()) {
        error = "bad_json";
        return false;
    }

    const JsonObjectConst root = document.as<JsonObjectConst>();
    const uint32_t version = root["version"] | 1u;
    if (version != 1) {
        error = "unsupported_version";
        return false;
    }

    const JsonArrayConst order = root["order"].as<JsonArrayConst>();
    const JsonArrayConst hidden = root["hidden"].as<JsonArrayConst>();
    const JsonArrayConst collapsed = root["collapsed"].as<JsonArrayConst>();
    if (!valid_key_array(order) || !valid_key_array(hidden) ||
        !valid_key_array(collapsed) || !array_is_subset(hidden, order) ||
        !array_is_subset(collapsed, order)) {
        error = "invalid_preferences";
        return false;
    }

    if (update) {
        if (!root["base_revision"].is<uint32_t>()) {
            error = "missing_base_revision";
            return false;
        }
        base_revision = root["base_revision"].as<uint32_t>();
        stored_revision = assigned_revision;
    } else {
        if (!root["revision"].is<uint32_t>() ||
            root["revision"].as<uint32_t>() == 0) {
            error = "invalid_revision";
            return false;
        }
        stored_revision = root["revision"].as<uint32_t>();
    }

    out.clear();
    out = "{\"version\":1,\"revision\":";
    char number[12] = {};
    snprintf(number, sizeof(number), "%lu",
             static_cast<unsigned long>(stored_revision));
    out += number;
    out += ",\"order\":";
    append_key_array(out, order);
    out += ",\"hidden\":";
    append_key_array(out, hidden);
    out += ",\"collapsed\":";
    append_key_array(out, collapsed);
    out += '}';
    if (out.overflowed()) {
        error = "preferences_too_large";
        return false;
    }
    return true;
}

}  // namespace

bool ReportPreferencesService::begin(StorageReadPort &read_port,
                                     StorageAtomicWritePort &write_port) {
    write_port_ = &write_port;
    loader_.begin(read_port);
    if (!preferences_json_.reserve(REPORT_PREFERENCES_JSON_RESERVE) ||
        !writing_json_.reserve(REPORT_PREFERENCES_JSON_RESERVE) ||
        !snapshot_.begin(REPORT_PREFERENCES_JSON_RESERVE)) {
        return false;
    }

    phase_ = Phase::LoadStart;
    return set_defaults();
}

bool ReportPreferencesService::ready_for_update() const {
    return phase_ == Phase::Ready;
}

uint32_t ReportPreferencesService::next_storage_generation() {
    storage_generation_++;
    if (storage_generation_ == 0) storage_generation_ = 1;
    return storage_generation_;
}

bool ReportPreferencesService::set_defaults(const char *load_error) {
    preferences_revision_ = 1;
    stored_ = false;
    preferences_json_ =
        "{\"version\":1,\"revision\":1,\"order\":[],"
        "\"hidden\":[],\"collapsed\":[]}";
    copy_cstr(load_error_, sizeof(load_error_), load_error ? load_error : "");
    snapshot_dirty_ = true;
    return !preferences_json_.overflowed();
}

bool ReportPreferencesService::accept_loaded(const uint8_t *bytes,
                                             size_t length) {
    const char *error = nullptr;
    uint32_t ignored_base = 0;
    uint32_t stored_revision = 0;
    LargeTextBuffer parsed;
    if (!parsed.reserve(REPORT_PREFERENCES_JSON_RESERVE) ||
        !canonicalize(bytes, length, false, 0, ignored_base,
                      stored_revision, parsed, error)) {
        Log::logf(CAT_REPORT, LOG_WARN,
                  "report preferences ignored error=%s\n",
                  error ? error : "allocation_failed");
        return set_defaults(error ? error : "allocation_failed");
    }

    preferences_json_.swap(parsed);
    preferences_revision_ = stored_revision;
    stored_ = true;
    load_error_[0] = '\0';
    snapshot_dirty_ = true;
    return true;
}

bool ReportPreferencesService::prepare_update(const char *json,
                                              size_t length,
                                              uint32_t request_id,
                                              const char *&error) {
    uint32_t next_revision = preferences_revision_ + 1;
    if (next_revision == 0) next_revision = 1;

    uint32_t base_revision = 0;
    uint32_t stored_revision = 0;
    writing_json_.clear();
    if (!canonicalize(reinterpret_cast<const uint8_t *>(json), length,
                      true, next_revision, base_revision, stored_revision,
                      writing_json_, error)) {
        return false;
    }
    if (base_revision != preferences_revision_) {
        error = "revision_conflict";
        return false;
    }

    write_bytes_ = LargeByteBuffer::copy_and_freeze(
        writing_json_.c_str(), writing_json_.length());
    if (!write_bytes_) {
        error = "allocation_failed";
        return false;
    }

    pending_revision_ = stored_revision;
    update_request_id_ = request_id;
    return true;
}

OperationAdmission ReportPreferencesService::update(const char *json,
                                                    size_t length,
                                                    uint32_t request_id) {
    if (phase_ != Phase::Ready) return OperationAdmission::Busy;
    if (!json || length == 0 || length > REPORT_PREFERENCES_MAX_BYTES ||
        request_id == 0) {
        return OperationAdmission::Rejected;
    }

    const char *error = nullptr;
    if (!prepare_update(json, length, request_id, error)) {
        update_request_id_ = request_id;
        complete_update(false, error ? error : "invalid_preferences");
        return OperationAdmission::Rejected;
    }

    last_update_valid_ = false;
    update_error_[0] = '\0';
    phase_ = Phase::WriteStart;
    return OperationAdmission::Accepted;
}

void ReportPreferencesService::complete_update(bool success,
                                               const char *error) {
    if (success) {
        preferences_json_.swap(writing_json_);
        preferences_revision_ = pending_revision_;
        stored_ = true;
        load_error_[0] = '\0';
    } else {
        write_bytes_.reset();
    }

    last_update_valid_ = true;
    last_update_ok_ = success;
    copy_cstr(update_error_, sizeof(update_error_), error ? error : "");
    snapshot_dirty_ = true;
    pending_revision_ = 0;
    phase_ = Phase::Ready;
}

bool ReportPreferencesService::publish_snapshot() {
    LargeTextBuffer next;
    if (!next.reserve(REPORT_PREFERENCES_JSON_RESERVE) ||
        preferences_json_.length() < 2) {
        return false;
    }

    next.append(preferences_json_.c_str(), preferences_json_.length() - 1);
    json_add_bool(next, "stored", stored_);
    json_add_string(next, "state",
                    phase_ == Phase::LoadStart || phase_ == Phase::Loading
                        ? "loading"
                        : phase_ == Phase::WriteStart || phase_ == Phase::Writing
                            ? "saving"
                            : "ready");
    if (load_error_[0]) json_add_string(next, "load_error", load_error_);
    if (last_update_valid_) {
        json_add_uint64(next, "update", update_request_id_);
        json_add_bool(next, "update_ok", last_update_ok_);
        if (!last_update_ok_) {
            json_add_string(next, "update_error",
                            update_error_[0] ? update_error_ : "save_failed");
        }
    }
    next += '}';
    if (next.overflowed() || !snapshot_.replace(next)) return false;

    snapshot_dirty_ = false;
    return true;
}

void ReportPreferencesService::poll() {
    if (snapshot_dirty_) (void)publish_snapshot();

    switch (phase_) {
        case Phase::LoadStart: {
            const OperationAdmission admission = loader_.start(
                REPORT_PREFERENCES_PATH, REPORT_PREFERENCES_MAX_BYTES,
                next_storage_generation(), StorageReadLane::Maintenance);
            if (admission == OperationAdmission::Busy) return;
            if (admission != OperationAdmission::Accepted) {
                phase_ = Phase::Ready;
                (void)set_defaults("load_rejected");
                return;
            }
            phase_ = Phase::Loading;
            snapshot_dirty_ = true;
            return;
        }

        case Phase::Loading: {
            (void)loader_.poll();
            const StorageBoundedFileLoadStatus status = loader_.status();
            if (!status.terminal()) return;

            if (status.state == StorageBoundedFileLoadState::Ready) {
                const std::shared_ptr<const LargeByteBuffer> bytes =
                    loader_.take_completed();
                if (bytes) (void)accept_loaded(bytes->data(), bytes->size());
                else (void)set_defaults("load_result_missing");
            } else if (status.state == StorageBoundedFileLoadState::Missing) {
                loader_.reset();
                (void)set_defaults();
            } else {
                const char *error = status.error[0]
                    ? status.error : "load_failed";
                loader_.reset();
                (void)set_defaults(error);
            }
            phase_ = Phase::Ready;
            snapshot_dirty_ = true;
            return;
        }

        case Phase::WriteStart: {
            if (!write_port_ || !write_bytes_) {
                complete_update(false, "write_not_ready");
                return;
            }

            StorageAtomicWriteCommand command;
            command.path = REPORT_PREFERENCES_PATH;
            command.bytes = write_bytes_;
            command.lane = StorageAtomicWriteLane::Foreground;
            command.generation = next_storage_generation();

            const OperationSubmission submission =
                write_port_->request_write(command);
            if (submission.admission == OperationAdmission::Busy) return;
            if (!submission.accepted()) {
                complete_update(false, "write_rejected");
                return;
            }

            write_ticket_ = submission.ticket;
            phase_ = Phase::Writing;
            snapshot_dirty_ = true;
            return;
        }

        case Phase::Writing: {
            StorageAtomicWriteCompletion completion;
            if (!write_port_->take_completion(write_ticket_, completion)) {
                return;
            }
            write_ticket_ = {};

            const bool success = completion.outcome.disposition ==
                                     OperationDisposition::Succeeded &&
                                 write_bytes_ &&
                                 completion.bytes_written == write_bytes_->size();
            write_bytes_.reset();
            complete_update(success,
                            success ? nullptr
                                    : completion.error[0]
                                        ? completion.error
                                        : "write_failed");
            return;
        }

        case Phase::Ready:
            return;
    }
}

}  // namespace aircannect
