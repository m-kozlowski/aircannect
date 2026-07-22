#include "provisioning.h"

#include <string.h>

#include "board.h"
#include "config_service.h"
#include "debug_log.h"
#include "storage_path_port.h"
#include "storage_read_port.h"

namespace aircannect {

namespace {

static constexpr size_t PROVISION_FILE_MAX_BYTES = 64 * 1024;
static constexpr size_t PROVISION_READ_CHUNK_BYTES = 512;
static constexpr uint32_t PROVISION_STORAGE_TIMEOUT_MS = 10 * 1000;

struct ProvisionState {
    bool wifi_profile_touched = false;
    WifiProfile wifi[AC_WIFI_PROFILE_MAX];
    bool wifi_touched[AC_WIFI_PROFILE_MAX] = {};
};

struct ProvisionCounts {
    size_t source_lines = 0;
    size_t applied = 0;
    size_t rejected = 0;
};

String parse_key(String key) {
    key.trim();
    return key;
}

void strip_line_ending(String &line) {
    while (line.length() > 0 && line[line.length() - 1] == '\r') {
        line.remove(line.length() - 1);
    }
}

bool parse_wifi_key(const String &key, size_t &index, String &field) {
    index = 0;
    field = "";
    if (key.startsWith("ssid_")) field = "ssid";
    else if (key.startsWith("pass_")) field = "pass";
    else return false;

    String index_text = key.substring(5);
    if (!index_text.length()) return false;
    for (size_t i = 0; i < index_text.length(); ++i) {
        if (!isDigit(index_text[i])) return false;
    }
    const int parsed = index_text.toInt();
    if (parsed < 0 || parsed >= static_cast<int>(AC_WIFI_PROFILE_MAX)) {
        return false;
    }
    index = static_cast<size_t>(parsed);
    return true;
}

bool apply_wifi_key(ProvisionState &state,
                    const String &key,
                    const String &value,
                    bool &known) {
    size_t index = 0;
    String field;
    known = parse_wifi_key(key, index, field);
    if (!known) return true;

    if (index >= AC_WIFI_PROFILE_MAX) return false;
    if (field == "ssid") {
        state.wifi_profile_touched = true;
        state.wifi_touched[index] = true;
        state.wifi[index].ssid = value;
        return true;
    }
    if (field == "pass") {
        state.wifi[index].password = value;
        return true;
    }
    return false;
}

bool apply_app_key(ConfigService &config_service,
                   const String &key,
                   const String &value,
                   bool &known) {
    const ConfigFieldUpdate update = config_service.set_transaction_value(
        key.c_str(), value, false, true);
    known = update.disposition != ConfigFieldDisposition::UnknownKey;
    return update.accepted();
}

bool commit_wifi(WifiManager &wifi_manager, ProvisionState &state) {
    if (!state.wifi_profile_touched) return true;

    WifiProfile profiles[AC_WIFI_PROFILE_MAX];
    size_t count = 0;
    for (size_t i = 0; i < AC_WIFI_PROFILE_MAX; ++i) {
        if (!state.wifi_touched[i]) continue;
        profiles[count++] = state.wifi[i];
    }
    return wifi_manager.replace_profiles(profiles, count, false);
}

bool wait_for_read(StorageReadPort &port,
                   OperationTicket ticket,
                   StorageReadCompletion &completion) {
    const uint32_t started_ms = millis();
    while (static_cast<uint32_t>(millis() - started_ms) <
           PROVISION_STORAGE_TIMEOUT_MS) {
        if (port.take_completion(ticket, completion)) return true;
        delay(1);
    }
    (void)port.abandon(ticket);
    return false;
}

bool wait_for_path(StoragePathPort &port,
                   OperationTicket ticket,
                   StoragePathCompletion &completion) {
    const uint32_t started_ms = millis();
    while (static_cast<uint32_t>(millis() - started_ms) <
           PROVISION_STORAGE_TIMEOUT_MS) {
        if (port.take_completion(ticket, completion)) return true;
        delay(1);
    }
    (void)port.abandon(ticket);
    return false;
}

bool inspect_config_file(StoragePathPort &port,
                         StoragePathCompletion &completion) {
    StoragePathCommand command;
    command.operation = StoragePathOperation::Stat;
    command.source = AC_PROVISION_CONFIG_PATH;
    command.generation = 1;

    const OperationSubmission submission = port.request(command);
    if (!submission.accepted()) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] storage metadata request rejected\n");
        return false;
    }
    if (!wait_for_path(port, submission.ticket, completion)) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] storage metadata request timed out\n");
        return false;
    }
    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        if (strcmp(completion.error, "storage_not_mounted") == 0) {
            return false;
        }
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] failed to inspect %s error=%s\n",
                  AC_PROVISION_CONFIG_PATH,
                  completion.error[0] ? completion.error : "storage_error");
        return false;
    }
    return true;
}

bool prepare_config_file(StorageReadPort &port,
                         size_t expected_size,
                         StoragePreparedRead &prepared) {
    StorageReadCommand command;
    command.path = AC_PROVISION_CONFIG_PATH;
    command.length = expected_size > 0 ? expected_size : 1;
    command.lane = StorageReadLane::Foreground;
    command.generation = 2;

    const OperationSubmission submission = port.request_read(command);
    if (!submission.accepted()) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] storage read request rejected\n");
        return false;
    }

    StorageReadCompletion completion;
    if (!wait_for_read(port, submission.ticket, completion)) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] storage read request timed out\n");
        return false;
    }
    if (completion.outcome.disposition !=
            OperationDisposition::Succeeded ||
        !completion.prepared.valid()) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] failed to read %s\n",
                  AC_PROVISION_CONFIG_PATH);
        return false;
    }
    if (completion.prepared.length != expected_size) {
        port.release_prepared(completion.prepared);
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] %s changed while being read\n",
                  AC_PROVISION_CONFIG_PATH);
        return false;
    }

    prepared = completion.prepared;
    return true;
}

void apply_config_line(ConfigService &config_service,
                       ProvisionState &state,
                       ProvisionCounts &counts,
                       String line,
                       bool too_long) {
    counts.source_lines++;
    if (too_long) {
        counts.rejected++;
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] line %u is too long\n",
                  static_cast<unsigned>(counts.source_lines));
        return;
    }

    strip_line_ending(line);
    String line_for_control = line;
    line_for_control.trim();
    if (!line_for_control.length() || line_for_control[0] == '#' ||
        line_for_control[0] == ';') {
        return;
    }

    const int eq = line.indexOf('=');
    if (eq <= 0) {
        counts.rejected++;
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] invalid line %u\n",
                  static_cast<unsigned>(counts.source_lines));
        return;
    }

    String key = parse_key(line.substring(0, eq));
    String value = line.substring(eq + 1);

    bool known = false;
    bool ok = apply_wifi_key(state, key, value, known);
    if (!known) ok = apply_app_key(config_service, key, value, known);
    if (known && ok) {
        counts.applied++;
        return;
    }

    counts.rejected++;
    Log::logf(CAT_CONFIG, LOG_WARN,
              "[PROVISION] rejected key=%s\n", key.c_str());
}

bool parse_config_file(StorageReadPort &port,
                       ConfigService &config_service,
                       StoragePreparedRead prepared,
                       ProvisionState &state,
                       ProvisionCounts &counts) {
    uint8_t bytes[PROVISION_READ_CHUNK_BYTES];
    String line;
    line.reserve(AC_PROVISION_LINE_MAX + 1);
    bool too_long = false;
    size_t offset = 0;

    while (offset < prepared.length) {
        const size_t received = port.read_prepared(
            prepared, offset, bytes, sizeof(bytes));
        if (received == 0) return false;

        for (size_t i = 0; i < received; ++i) {
            const char value = static_cast<char>(bytes[i]);
            if (value == '\n') {
                apply_config_line(config_service, state, counts, line,
                                  too_long);
                line = "";
                too_long = false;
                continue;
            }
            if (too_long) continue;
            if (line.length() >= AC_PROVISION_LINE_MAX) {
                line = "";
                too_long = true;
                continue;
            }
            line += value;
        }
        offset += received;
    }
    if (line.length() > 0 || too_long) {
        apply_config_line(config_service, state, counts, line, too_long);
    }
    return true;
}

bool mark_config_consumed(StoragePathPort &port) {
    StoragePathCommand command;
    command.operation = StoragePathOperation::MoveReplacing;
    command.source = AC_PROVISION_CONFIG_PATH;
    command.destination = AC_PROVISION_OK_PATH;
    command.generation = 3;

    const OperationSubmission submission = port.request(command);
    if (!submission.accepted()) return false;

    StoragePathCompletion completion;
    return wait_for_path(port, submission.ticket, completion) &&
           completion.outcome.disposition ==
               OperationDisposition::Succeeded;
}

void consume_config_file(ConfigService &config_service,
                         WifiManager &wifi_manager,
                         StorageReadPort &read_port,
                         StoragePathPort &path_port,
                         size_t file_size) {
    StoragePreparedRead prepared;
    if (!prepare_config_file(read_port, file_size, prepared)) return;

    ProvisionState state;
    ProvisionCounts counts;

    if (!config_service.begin_transaction()) {
        read_port.release_prepared(prepared);
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] config transaction busy\n");
        return;
    }

    const bool parsed = parse_config_file(
        read_port, config_service, prepared, state, counts);
    read_port.release_prepared(prepared);

    const ConfigTransactionResult transaction =
        config_service.commit_transaction();
    bool ok = parsed && transaction.persisted;
    ok = commit_wifi(wifi_manager, state) && ok;

    const bool renamed = mark_config_consumed(path_port);
    Log::logf(CAT_CONFIG, ok ? LOG_INFO : LOG_WARN,
              "[PROVISION] config.txt applied=%u rejected=%u status=%s\n",
              static_cast<unsigned>(counts.applied),
              static_cast<unsigned>(counts.rejected),
              ok ? "ok" : "partial");
    if (!renamed) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] failed to rename %s to %s\n",
                  AC_PROVISION_CONFIG_PATH, AC_PROVISION_OK_PATH);
    }
}

}  // namespace

void apply_storage_provisioning(ConfigService &config_service,
                                WifiManager &wifi_manager,
                                StorageReadPort &read_port,
                                StoragePathPort &path_port) {
    StoragePathCompletion metadata;
    if (!inspect_config_file(path_port, metadata) || !metadata.exists) {
        return;
    }
    if (metadata.directory) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] %s is not a file\n",
                  AC_PROVISION_CONFIG_PATH);
        return;
    }
    if (metadata.size > PROVISION_FILE_MAX_BYTES) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] %s exceeds %u bytes\n",
                  AC_PROVISION_CONFIG_PATH,
                  static_cast<unsigned>(PROVISION_FILE_MAX_BYTES));
        return;
    }

    Log::logf(CAT_CONFIG, LOG_INFO, "[PROVISION] found %s\n",
              AC_PROVISION_CONFIG_PATH);
    consume_config_file(config_service, wifi_manager, read_port, path_port,
                        static_cast<size_t>(metadata.size));
}

}  // namespace aircannect
