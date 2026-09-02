#include "console_commands.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config_registry.h"
#include "board.h"
#include "config_service.h"
#include "debug_log.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "storage_manager.h"
#include "storage_service.h"
#include "string_util.h"

namespace aircannect {
namespace {

const AppConfigFieldDescriptor *log_level_config_field(log_cat_t category) {
    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    for (size_t i = 0; i < count; ++i) {
        if (fields[i].id == AppConfigFieldId::LogLevel &&
            fields[i].index == static_cast<int16_t>(category)) {
            return &fields[i];
        }
    }
    return nullptr;
}

bool split_storage_console_path(const char *path,
                                char *parent,
                                size_t parent_size,
                                const char *&name) {
    name = nullptr;
    if (!path || strcmp(path, "/") == 0 || !parent || !parent_size) {
        return false;
    }

    const char *slash = strrchr(path, '/');
    if (!slash || !storage_valid_child_name(slash + 1)) return false;

    const size_t parent_length = slash == path
        ? 1
        : static_cast<size_t>(slash - path);
    if (parent_length >= parent_size) return false;

    memcpy(parent, path, parent_length);
    parent[parent_length] = '\0';
    name = slash + 1;
    return true;
}

void print_storage_listing_entry(Print &out,
                                 const StorageDirectoryEntryView &entry) {
    out.print(entry.directory ? "[DIR] " : "[FILE] ");
    out.print(entry.name);
    if (entry.directory) {
        out.println("/");
        return;
    }

    out.print(" bytes=");
    char size[24] = {};
    snprintf(size, sizeof(size), "%llu",
             static_cast<unsigned long long>(entry.size));
    out.println(size);
}

void print_storage_listing_header(
    Print &out,
    const StorageDirectorySnapshot &snapshot) {
    out.print("[STORAGE] path=");
    out.print(snapshot.path());
    out.print(" entries=");
    out.println(static_cast<unsigned long>(snapshot.size()));
}

void handle_log(Print &out,
                String rest,
                ConfigService &config,
                StorageReadPort &storage_read,
                OperationTicket &tail_ticket,
                StoragePreparedRead &tail_prepared,
                size_t &tail_offset,
                uint32_t &tail_generation) {
    rest.trim();

    int action_position = 0;
    String action;
    if (!parse_console_arg(rest, action_position, action)) action = "status";
    action.toLowerCase();

    String args = rest.substring(action_position);
    args.trim();
    if (action == "status" && !args.length()) {
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (action == "level") {
        int pos = 0;
        String first;
        if (!parse_console_arg(args, pos, first)) {
            out.println(
                "[LOG] usage: log level LEVEL | "
                "log level CATEGORY LEVEL");
            return;
        }

        log_level_t level = LOG_INFO;
        log_cat_t category = CAT_GENERAL;
        if (Log::parse_level(first, level)) {
            if (!config.begin_transaction()) {
                out.println("[LOG] config transaction busy");
                return;
            }

            bool accepted = true;
            size_t field_count = 0;
            const AppConfigFieldDescriptor *fields =
                app_config_fields(field_count);
            for (size_t i = 0; i < field_count; ++i) {
                if (fields[i].id != AppConfigFieldId::LogLevel) continue;
                accepted = config.set_transaction_value(
                    fields[i].key, Log::level_name(level), false).accepted() &&
                    accepted;
            }
            const ConfigTransactionResult transaction =
                config.commit_transaction();
            if (!accepted || !transaction.persisted) {
                out.println("[LOG] failed to store level");
                return;
            }
            ConsoleFormat::print_log_status(out);
            return;
        }

        if (!Log::parse_cat(first, category)) {
            out.println("[LOG] unknown category or level");
            return;
        }

        String level_text;
        if (!parse_console_arg(args, pos, level_text) ||
            !Log::parse_level(level_text, level)) {
            out.println("[LOG] usage: log level CATEGORY LEVEL");
            return;
        }

        const AppConfigFieldDescriptor *field =
            log_level_config_field(category);
        ConfigTransactionResult transaction;
        const ConfigFieldUpdate update = field
            ? config.set_value(field->key, Log::level_name(level), false,
                               &transaction)
            : ConfigFieldUpdate{};
        if (!update.accepted() || !transaction.persisted) {
            out.println("[LOG] failed to store level");
            return;
        }
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (action == "syslog") {
        String args_lower = args;
        args_lower.toLowerCase();
        if (!args.length() || args_lower == "status") {
            ConsoleFormat::print_log_status(out);
            return;
        }

        int pos = 0;
        String host;
        if (!parse_console_arg(args, pos, host)) {
            out.println("[LOG] usage: log syslog off|HOST [PORT]");
            return;
        }
        String host_lower = host;
        host_lower.toLowerCase();
        if (host_lower == "off" || host_lower == "disable" ||
            host_lower == "disabled" || host_lower == "0") {
            ConfigTransactionResult transaction;
            const ConfigFieldUpdate update = config.set_value(
                "syslog_en", "0", false, &transaction);
            if (!update.accepted() || !transaction.persisted) {
                out.println("[LOG] failed to store syslog config");
                return;
            }
            ConsoleFormat::print_log_status(out);
            return;
        }

        uint16_t port = config.data().syslog_port;
        String port_text;
        if (parse_console_arg(args, pos, port_text) &&
            !parse_uint16_arg(port_text, port)) {
            out.println("[LOG] invalid syslog port");
            return;
        }
        if (!config.begin_transaction()) {
            out.println("[LOG] config transaction busy");
            return;
        }

        const ConfigFieldUpdate host_update =
            config.set_transaction_value("syslog_host", host, false);
        const ConfigFieldUpdate port_update = config.set_transaction_value(
            "syslog_port", String(port), false);
        const ConfigFieldUpdate enabled_update =
            config.set_transaction_value("syslog_en", "1", false);
        const ConfigTransactionResult transaction =
            config.commit_transaction();
        if (!host_update.accepted() || !port_update.accepted() ||
            !enabled_update.accepted() || !transaction.persisted) {
            out.println("[LOG] syslog host must be an IPv4 address");
            return;
        }
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (action == "tail") {
        size_t lines = AC_FILE_LOG_TAIL_DEFAULT_LINES;
        if (args.length()) {
            int pos = 0;
            String lines_text;
            if (!parse_console_arg(args, pos, lines_text)) {
                out.println("[LOG] usage: log tail [LINES]");
                return;
            }

            char *end = nullptr;
            const unsigned long parsed =
                strtoul(lines_text.c_str(), &end, 10);
            if (!end || *end != '\0' || !parsed ||
                parsed > AC_FILE_LOG_TAIL_MAX_LINES) {
                out.print("[LOG] lines must be 1..");
                out.println(static_cast<unsigned long>(
                    AC_FILE_LOG_TAIL_MAX_LINES));
                return;
            }
            lines = static_cast<size_t>(parsed);
        }
        if (!Log::filelog_enabled()) {
            out.println("[LOG] file log unavailable");
            return;
        }
        if (tail_ticket.valid() || tail_prepared.valid()) {
            out.println("[LOG] tail already pending");
            return;
        }

        ++tail_generation;
        if (!tail_generation) ++tail_generation;

        StorageReadCommand read;
        read.path = AC_FILE_LOG_PATH;
        read.mode = StorageReadMode::TailLines;
        read.length = AC_STORAGE_PREPARED_READ_MAX_BYTES;
        read.tail_lines = lines;
        read.lane = StorageReadLane::Foreground;
        read.generation = tail_generation;

        const OperationSubmission submission = storage_read.request_read(read);
        if (!submission.accepted()) {
            out.println("[LOG] file log busy");
            return;
        }

        tail_ticket = submission.ticket;
        tail_prepared = {};
        tail_offset = 0;
        out.println("[LOG] tail queued");
        return;
    }

    if (action == "test") {
        String text = args.length() ? args : "test";
        text.trim();
        if (!text.length()) text = "test";
        Log::logf(CAT_CLI, LOG_INFO, "[LOG] %s\n", text.c_str());
        out.println("[LOG] test emitted");
        return;
    }

    print_unknown_command(out, "LOG",
                          "log status, level, syslog, tail, test");
}

}  // namespace

StorageConsoleCommands::StorageConsoleCommands(
    ConfigService &config,
    StorageReadPort &storage_read,
    StorageBrowserPort &storage_browser,
    StoragePathPort &storage_path,
    StorageDeletePort &storage_delete,
    StorageStatusPort &storage_status)
    : config_(config),
      storage_read_(storage_read),
      storage_browser_(storage_browser),
      storage_path_(storage_path),
      storage_delete_(storage_delete),
      storage_status_(storage_status) {}

StorageConsoleCommands::CommandSessionState *
StorageConsoleCommands::command_session(uint32_t session_id, bool create) {
    CommandSessionState *empty = nullptr;
    for (CommandSessionState &session : command_sessions_) {
        if (session.session_id == session_id) return &session;
        if (!session.session_id && !empty) empty = &session;
    }
    if (!create || !empty) return nullptr;

    *empty = {};
    empty->session_id = session_id;
    copy_cstr(empty->cwd, sizeof(empty->cwd), "/");
    return empty;
}

const StorageConsoleCommands::CommandSessionState *
StorageConsoleCommands::command_session(uint32_t session_id) const {
    for (const CommandSessionState &session : command_sessions_) {
        if (session.session_id == session_id) return &session;
    }
    return nullptr;
}

void StorageConsoleCommands::execute_storage(
    String rest,
    Print &out,
    CommandSessionState &state) {
    rest.trim();

    int position = 0;
    String command;
    if (!parse_console_arg(rest, position, command)) command = "status";
    command.toLowerCase();

    String first;
    String second;
    String extra;
    const bool has_first = parse_console_arg(rest, position, first);
    const bool has_second = parse_console_arg(rest, position, second);
    const bool has_extra = parse_console_arg(rest, position, extra);

    if (command == "status" && !has_first) {
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (command == "mount" && !has_first) {
        out.print("[STORAGE] mount ");
        out.println(StorageService::request_mount()
                        ? "queued"
                        : "rejected");
        return;
    }
    if (command == "pwd" && !has_first) {
        out.println(state.cwd);
        return;
    }
    if (command == "ls" && has_second) {
        out.println("[STORAGE] usage: storage ls [PATH]");
        return;
    }
    if (command == "cd" && (!has_first || has_second)) {
        out.println("[STORAGE] usage: storage cd PATH");
        return;
    }
    if (command != "ls" && command != "cd" && command != "rm" &&
        command != "rename") {
        print_unknown_command(
            out, "STORAGE",
            "storage status, mount, pwd, ls [PATH], cd PATH, "
            "rm PATH, rename PATH NEW_NAME");
        return;
    }

    if (state.storage_operation != StorageOperation::None) {
        out.println("[STORAGE] operation pending");
        return;
    }
    if (!storage_status_.mounted()) {
        out.println("[STORAGE] storage unavailable");
        return;
    }

    if (command == "ls" && !has_second) {
        const String requested = has_first ? first : String();
        char path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_resolve_user_path(
                state.cwd, requested.c_str(), path, sizeof(path))) {
            out.println("[STORAGE] bad path");
            return;
        }

        std::shared_ptr<const StorageDirectorySnapshot> snapshot;
        char error[AC_STORAGE_ERROR_MAX] = {};
        const StorageListingRead read = storage_browser_.listing(
            path, true, snapshot, error, sizeof(error));
        if (read == StorageListingRead::Error) {
            out.print("[STORAGE] list failed: ");
            out.println(error[0] ? error : "error");
            return;
        }
        state.storage_operation = StorageOperation::List;
        state.listing_snapshot = snapshot;
        state.listing_offset = 0;
        copy_cstr(state.operation_path, sizeof(state.operation_path), path);
        if (read == StorageListingRead::Preparing) {
            out.print("[STORAGE] listing ");
            out.println(path);
        } else if (state.listing_snapshot) {
            print_storage_listing_header(out, *state.listing_snapshot);
        }
        return;
    }

    if (command == "cd" && has_first && !has_second) {
        char path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_resolve_user_path(
                state.cwd, first.c_str(), path, sizeof(path))) {
            out.println("[STORAGE] bad path");
            return;
        }

        state.storage_generation++;
        if (!state.storage_generation) state.storage_generation++;

        StoragePathCommand request;
        request.operation = StoragePathOperation::Stat;
        request.source = path;
        request.generation = state.storage_generation;
        const OperationSubmission submission = storage_path_.request(request);
        if (!submission.accepted()) {
            out.println("[STORAGE] path lookup rejected");
            return;
        }

        state.storage_operation = StorageOperation::ChangeDirectory;
        state.storage_ticket = submission.ticket;
        copy_cstr(state.operation_path, sizeof(state.operation_path), path);
        return;
    }

    if ((command == "rm" || command == "rename") &&
        (!has_first || has_extra ||
         (command == "rm" && has_second) ||
         (command == "rename" && !has_second))) {
        out.println(command == "rm"
            ? "[STORAGE] usage: storage rm PATH"
            : "[STORAGE] usage: storage rename PATH NEW_NAME");
        return;
    }

    if (command == "rm" || command == "rename") {
        const StorageWorkloadSnapshot workload =
            storage_status_.workload_snapshot();
        if (!workload.valid || !workload.available || workload.busy ||
            workload.maintenance_active || workload.edf_queued > 0 ||
            workload.open_file_count > 0) {
            out.println("[STORAGE] storage busy");
            return;
        }

        char path[AC_STORAGE_PATH_MAX] = {};
        if (!storage_resolve_user_path(
                state.cwd, first.c_str(), path, sizeof(path))) {
            out.println("[STORAGE] bad path");
            return;
        }

        char parent[AC_STORAGE_PATH_MAX] = {};
        const char *name = nullptr;
        if (!split_storage_console_path(
                path, parent, sizeof(parent), name)) {
            out.println("[STORAGE] cannot modify root");
            return;
        }

        if (command == "rm") {
            const char *names[] = {name};
            char error[AC_STORAGE_ERROR_MAX] = {};
            uint32_t id = 0;
            if (!storage_delete_.start_selected(
                    parent, names, 1, &id, error, sizeof(error))) {
                out.print("[STORAGE] remove rejected: ");
                out.println(error[0] ? error : "error");
                return;
            }

            state.storage_operation = StorageOperation::Delete;
            state.delete_id = id;
            copy_cstr(state.operation_path,
                      sizeof(state.operation_path), path);
            out.print("[STORAGE] removing ");
            out.println(path);
            return;
        }

        if (!storage_valid_child_name(second.c_str()) ||
            second.length() >= AC_STORAGE_NAME_MAX) {
            out.println("[STORAGE] bad new name");
            return;
        }

        char destination[AC_STORAGE_PATH_MAX] = {};
        if (!storage_append_child_path(
                parent, second.c_str(), destination, sizeof(destination)) ||
            strcmp(path, destination) == 0) {
            out.println("[STORAGE] bad destination");
            return;
        }

        state.storage_generation++;
        if (!state.storage_generation) state.storage_generation++;

        StoragePathCommand request;
        request.operation = StoragePathOperation::Move;
        request.source = path;
        request.destination = destination;
        request.generation = state.storage_generation;
        const OperationSubmission submission = storage_path_.request(request);
        if (!submission.accepted()) {
            out.println("[STORAGE] rename rejected");
            return;
        }

        state.storage_operation = StorageOperation::Rename;
        state.storage_ticket = submission.ticket;
        copy_cstr(state.operation_path, sizeof(state.operation_path), path);
        copy_cstr(state.operation_destination,
                  sizeof(state.operation_destination), destination);
        out.print("[STORAGE] renaming ");
        out.print(path);
        out.print(" -> ");
        out.println(destination);
        return;
    }

}

void StorageConsoleCommands::poll_storage(
    Print &out,
    CommandSessionState &state) {
    if (state.storage_operation == StorageOperation::None) return;

    if (state.storage_operation == StorageOperation::List) {
        if (!state.listing_snapshot) {
            char error[AC_STORAGE_ERROR_MAX] = {};
            const StorageListingRead read = storage_browser_.listing(
                state.operation_path,
                false,
                state.listing_snapshot,
                error,
                sizeof(error));
            if (read == StorageListingRead::Preparing) return;
            if (read != StorageListingRead::Ready ||
                !state.listing_snapshot) {
                out.print("[STORAGE] list failed: ");
                out.println(error[0] ? error : "error");
                state.storage_operation = StorageOperation::None;
                state.operation_path[0] = '\0';
                return;
            }

            print_storage_listing_header(out, *state.listing_snapshot);
        }

        static constexpr size_t ENTRIES_PER_POLL = 8;
        const size_t end = std::min(
            state.listing_offset + ENTRIES_PER_POLL,
            state.listing_snapshot->size());
        while (state.listing_offset < end) {
            StorageDirectoryEntryView entry;
            if (state.listing_snapshot->entry(
                    state.listing_offset, entry)) {
                print_storage_listing_entry(out, entry);
            }
            state.listing_offset++;
        }
        if (state.listing_offset < state.listing_snapshot->size()) return;

        state.storage_operation = StorageOperation::None;
        state.listing_snapshot.reset();
        state.listing_offset = 0;
        state.operation_path[0] = '\0';
        return;
    }

    if (state.storage_operation == StorageOperation::Delete) {
        StorageDeleteStatus status;
        if (!storage_delete_.status(status)) return;
        if (status.id != state.delete_id) {
            out.println("[STORAGE] remove status unavailable");
            state.storage_operation = StorageOperation::None;
            state.delete_id = 0;
            state.operation_path[0] = '\0';
            return;
        }
        if (status.state == StorageDeleteState::Deleting) {
            return;
        }

        if (status.state == StorageDeleteState::Done) {
            out.print("[STORAGE] removed ");
            out.print(state.operation_path);
            out.print(" files=");
            out.print(status.files_deleted);
            out.print(" dirs=");
            out.println(status.dirs_deleted);
        } else {
            out.print("[STORAGE] remove failed: ");
            out.println(status.error[0] ? status.error : "error");
        }

        state.storage_operation = StorageOperation::None;
        state.delete_id = 0;
        state.operation_path[0] = '\0';
        return;
    }

    StoragePathCompletion completion;
    if (!storage_path_.take_completion(
            state.storage_ticket, completion)) {
        return;
    }

    if (state.storage_operation == StorageOperation::ChangeDirectory) {
        if (completion.outcome.disposition ==
                OperationDisposition::Succeeded &&
            completion.exists && completion.directory) {
            copy_cstr(state.cwd, sizeof(state.cwd), state.operation_path);
            out.println(state.cwd);
        } else {
            out.print("[STORAGE] cd failed: ");
            out.println(completion.error[0]
                ? completion.error
                : completion.exists ? "not_directory" : "not_found");
        }
    } else if (state.storage_operation == StorageOperation::Rename) {
        if (completion.outcome.disposition ==
            OperationDisposition::Succeeded) {
            out.print("[STORAGE] renamed ");
            out.println(state.operation_destination);
        } else {
            out.print("[STORAGE] rename failed: ");
            out.println(completion.error[0] ? completion.error : "error");
        }
    }

    state.storage_operation = StorageOperation::None;
    state.storage_ticket = {};
    state.operation_path[0] = '\0';
    state.operation_destination[0] = '\0';
}

void StorageConsoleCommands::cancel_storage(CommandSessionState &state) {
    if (state.storage_ticket.valid()) {
        (void)storage_path_.abandon(state.storage_ticket);
    }

    state.storage_operation = StorageOperation::None;
    state.storage_ticket = {};
    state.delete_id = 0;
    state.listing_snapshot.reset();
    state.listing_offset = 0;
    state.operation_path[0] = '\0';
    state.operation_destination[0] = '\0';
}

bool StorageConsoleCommands::execute(const String &command,
                                     const String &rest,
                                     Print &out,
                                     ConsoleCommandSession &session) {
    if (command != "storage" && command != "log") return false;

    CommandSessionState *state = command_session(session.id, true);
    if (!state) {
        out.println("[CLI] console session table full");
        return true;
    }

    if (command == "storage") {
        execute_storage(rest, out, *state);
        return true;
    }
    if (command == "log") {
        handle_log(out, rest, config_, storage_read_, state->tail_ticket,
                   state->tail_prepared, state->tail_offset,
                   state->tail_generation);
        return true;
    }
    return true;
}

void StorageConsoleCommands::poll_pending(Print &out,
                                          ConsoleCommandSession &session) {
    CommandSessionState *state = command_session(session.id, false);
    if (!state) return;

    poll_storage(out, *state);

    if (state->tail_ticket.valid()) {
        StorageReadCompletion completion;
        if (!storage_read_.take_completion(
                state->tail_ticket, completion)) {
            return;
        }

        state->tail_ticket = {};
        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.prepared.valid()) {
            out.println("[LOG] file log unavailable");
            return;
        }
        state->tail_prepared = completion.prepared;
        state->tail_offset = 0;
        if (!state->tail_prepared.length) {
            out.println("[LOG] file log empty");
            storage_read_.release_prepared(state->tail_prepared);
            state->tail_prepared = {};
            return;
        }
    }

    if (!state->tail_prepared.valid()) return;

    uint8_t buffer[AC_FILE_LOG_TAIL_READ_CHUNK];
    const PreparedByteRead read = storage_read_.read_prepared(
        state->tail_prepared, state->tail_offset, buffer, sizeof(buffer));
    if (read.state == PreparedByteReadState::Retry) return;
    if (read.state != PreparedByteReadState::Data) {
        storage_read_.release_prepared(state->tail_prepared);
        state->tail_prepared = {};
        state->tail_offset = 0;
        return;
    }

    out.write(buffer, read.bytes);
    state->tail_offset += read.bytes;
    if (state->tail_offset >= state->tail_prepared.length) {
        storage_read_.release_prepared(state->tail_prepared);
        state->tail_prepared = {};
        state->tail_offset = 0;
    }
}

bool StorageConsoleCommands::pending_output(
    const ConsoleCommandSession &session) const {
    const CommandSessionState *state = command_session(session.id);
    return state && state->pending();
}

void StorageConsoleCommands::cancel_pending(
    ConsoleCommandSession &session) {
    CommandSessionState *state = command_session(session.id, false);
    if (!state) return;

    cancel_storage(*state);

    if (state->tail_ticket.valid()) {
        (void)storage_read_.abandon(state->tail_ticket);
    }
    if (state->tail_prepared.valid()) {
        storage_read_.release_prepared(state->tail_prepared);
    }

    state->tail_ticket = {};
    state->tail_prepared = {};
    state->tail_offset = 0;
}

void StorageConsoleCommands::stop(ConsoleCommandSession &session) {
    cancel_pending(session);

    CommandSessionState *state = command_session(session.id, false);
    if (state) *state = {};
}

}  // namespace aircannect
