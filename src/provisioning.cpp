#include "provisioning.h"

#include <string.h>

#include <string>

#include "board.h"
#include "config_backup.h"
#include "config_service.h"
#include "debug_log.h"
#include "storage_path_port.h"
#include "storage_read_port.h"

namespace aircannect {

namespace {

static constexpr size_t PROVISION_FILE_MAX_BYTES = 64 * 1024;
static constexpr size_t PROVISION_READ_CHUNK_BYTES = 512;
static constexpr uint32_t PROVISION_STORAGE_TIMEOUT_MS = 10 * 1000;

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

bool parse_config_file(StorageReadPort &port,
                       StoragePreparedRead prepared,
                       ConfigRestore &restore) {
    uint8_t bytes[PROVISION_READ_CHUNK_BYTES];
    std::string line;
    line.reserve(AC_PROVISION_LINE_MAX + 1);
    bool too_long = false;
    size_t offset = 0;

    while (offset < prepared.length) {
        const PreparedByteRead read = port.read_prepared(
            prepared, offset, bytes, sizeof(bytes));
        if (read.state == PreparedByteReadState::Retry) {
            delay(1);
            continue;
        }
        if (read.state != PreparedByteReadState::Data) return false;

        for (size_t i = 0; i < read.bytes; ++i) {
            const char value = static_cast<char>(bytes[i]);
            if (value == '\n') {
                restore.add_line(line, too_long);
                line.clear();
                too_long = false;
                continue;
            }
            if (too_long) continue;
            if (line.size() >= AC_PROVISION_LINE_MAX) {
                line.clear();
                too_long = true;
                continue;
            }
            line += value;
        }
        offset += read.bytes;
    }
    if (!line.empty() || too_long) restore.add_line(line, too_long);
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
                         BleSensorSource *sensors,
                         StorageReadPort &read_port,
                         StoragePathPort &path_port,
                         size_t file_size) {
    StoragePreparedRead prepared;
    if (!prepare_config_file(read_port, file_size, prepared)) return;

    // Provisioning runs before WifiManager::begin(); a file without
    // passwords keeps the ones already saved for the same networks.
    wifi_manager.load_saved_profiles();

    ConfigRestore restore(config_service, wifi_manager, sensors);
    const bool parsed = parse_config_file(read_port, prepared, restore);
    read_port.release_prepared(prepared);

    const ConfigRestoreResult result = restore.finish();
    if (result.error) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] config transaction failed reason=%s\n",
                  result.error);
    }
    for (const ConfigRestoreIssue &issue : result.issues) {
        Log::logf(CAT_CONFIG, LOG_WARN,
                  "[PROVISION] rejected key=%s reason=%s\n",
                  issue.key.c_str(), issue.reason);
    }

    const bool ok = parsed && result.ok();
    const bool renamed = mark_config_consumed(path_port);
    Log::logf(CAT_CONFIG, ok ? LOG_INFO : LOG_WARN,
              "[PROVISION] config.txt applied=%u kept=%u rejected=%u "
              "status=%s\n",
              static_cast<unsigned>(result.applied),
              static_cast<unsigned>(result.kept),
              static_cast<unsigned>(result.rejected),
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
                                BleSensorSource *sensors,
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
    consume_config_file(config_service, wifi_manager, sensors, read_port,
                        path_port, static_cast<size_t>(metadata.size));
}

}  // namespace aircannect
