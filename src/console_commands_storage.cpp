#include "console_commands.h"

#include <stdlib.h>

#include "app_config_registry.h"
#include "board.h"
#include "config_service.h"
#include "debug_log.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "storage_manager.h"
#include "storage_service.h"

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

void print_storage_test_status(Print &out,
                               const StorageDiagnosticStatus &status) {
    out.print("[STORAGE_TEST] state=");
    out.print(storage_diagnostic_state_name(status.state));
    out.print(" path=");
    out.print(status.path[0] ? status.path : "--");
    out.print(" bytes=");
    out.print(status.bytes);
    if (status.error[0]) {
        out.print(" error=");
        out.print(status.error);
    }
    out.println();
}

void handle_storage(Print &out, String rest) {
    rest.trim();
    String lower = rest;
    lower.toLowerCase();
    if (!lower.length() || lower == "status") {
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (lower == "retry") {
        out.print("[STORAGE] mount retry ");
        out.println(StorageService::request_mount_retry()
                        ? "queued"
                        : "rejected");
        ConsoleFormat::print_storage_status(out, Storage::status());
        return;
    }
    if (lower == "write-test status") {
        print_storage_test_status(out, StorageService::diagnostic_status());
        return;
    }
    if (lower == "write-test" || lower.startsWith("write-test ")) {
        String args = rest;
        args.remove(0, String("write-test").length());
        int pos = 0;
        String path = "/aircannect-write-test.txt";
        String text = "AirCANnect storage write test";
        String parsed;
        if (parse_console_arg(args, pos, parsed)) {
            path = parsed;
            if (parse_console_arg(args, pos, parsed)) text = parsed;
        }
        text += '\n';

        const bool queued = StorageService::request_diagnostic_append(
            path.c_str(), reinterpret_cast<const uint8_t *>(text.c_str()),
            text.length());
        out.print("[STORAGE_TEST] ");
        out.println(queued ? "queued" : "rejected");
        print_storage_test_status(out, StorageService::diagnostic_status());
        return;
    }

    print_unknown_command(out, "STORAGE",
                          "storage status, retry, "
                          "write-test [status|P T]");
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
    if (!rest.length() || rest == "status") {
        ConsoleFormat::print_log_status(out);
        return;
    }

    if (rest.startsWith("level ")) {
        String args = rest.substring(6);
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

    if (rest.startsWith("syslog")) {
        String args = rest.length() > 6 ? rest.substring(6) : "";
        args.trim();
        if (!args.length() || args == "status") {
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

    if (rest == "tail" || rest.startsWith("tail ")) {
        size_t lines = AC_FILE_LOG_TAIL_DEFAULT_LINES;
        if (rest.length() > 4) {
            String args = rest.substring(4);
            args.trim();
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

    if (rest == "test" || rest.startsWith("test ")) {
        String text = rest.length() > 4 ? rest.substring(5) : "test";
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
    StorageReadPort &storage_read)
    : config_(config), storage_read_(storage_read) {}

StorageConsoleCommands::TailSessionState *
StorageConsoleCommands::tail_session(uint32_t session_id, bool create) {
    TailSessionState *empty = nullptr;
    for (TailSessionState &session : tail_sessions_) {
        if (session.session_id == session_id) return &session;
        if (!session.session_id && !empty) empty = &session;
    }
    if (!create || !empty) return nullptr;

    empty->session_id = session_id;
    return empty;
}

const StorageConsoleCommands::TailSessionState *
StorageConsoleCommands::tail_session(uint32_t session_id) const {
    for (const TailSessionState &session : tail_sessions_) {
        if (session.session_id == session_id) return &session;
    }
    return nullptr;
}

bool StorageConsoleCommands::execute(const String &command,
                                     const String &rest,
                                     Print &out,
                                     ConsoleCommandSession &session) {
    if (command == "storage") {
        handle_storage(out, rest);
        return true;
    }
    if (command == "log") {
        TailSessionState *tail = tail_session(session.id, true);
        if (!tail) {
            out.println("[LOG] console session table full");
            return true;
        }

        handle_log(out, rest, config_, storage_read_, tail->ticket,
                   tail->prepared, tail->offset, tail->generation);
        return true;
    }
    return false;
}

void StorageConsoleCommands::poll_pending(Print &out,
                                          ConsoleCommandSession &session) {
    TailSessionState *tail = tail_session(session.id, false);
    if (!tail) return;

    if (tail->ticket.valid()) {
        StorageReadCompletion completion;
        if (!storage_read_.take_completion(tail->ticket, completion)) {
            return;
        }

        tail->ticket = {};
        if (completion.outcome.disposition !=
                OperationDisposition::Succeeded ||
            !completion.prepared.valid()) {
            out.println("[LOG] file log unavailable");
            return;
        }
        tail->prepared = completion.prepared;
        tail->offset = 0;
        if (!tail->prepared.length) {
            out.println("[LOG] file log empty");
            storage_read_.release_prepared(tail->prepared);
            tail->prepared = {};
            return;
        }
    }

    if (!tail->prepared.valid()) return;

    uint8_t buffer[AC_FILE_LOG_TAIL_READ_CHUNK];
    const PreparedByteRead read = storage_read_.read_prepared(
        tail->prepared, tail->offset, buffer, sizeof(buffer));
    if (read.state == PreparedByteReadState::Retry) return;
    if (read.state != PreparedByteReadState::Data) {
        storage_read_.release_prepared(tail->prepared);
        tail->prepared = {};
        tail->offset = 0;
        return;
    }

    out.write(buffer, read.bytes);
    tail->offset += read.bytes;
    if (tail->offset >= tail->prepared.length) {
        storage_read_.release_prepared(tail->prepared);
        tail->prepared = {};
        tail->offset = 0;
    }
}

bool StorageConsoleCommands::pending_output(
    const ConsoleCommandSession &session) const {
    const TailSessionState *tail = tail_session(session.id);
    return tail && tail->pending();
}

void StorageConsoleCommands::cancel_pending(
    ConsoleCommandSession &session) {
    TailSessionState *tail = tail_session(session.id, false);
    if (!tail) return;

    if (tail->ticket.valid()) {
        (void)storage_read_.abandon(tail->ticket);
    }
    if (tail->prepared.valid()) {
        storage_read_.release_prepared(tail->prepared);
    }

    tail->ticket = {};
    tail->prepared = {};
    tail->offset = 0;
}

void StorageConsoleCommands::stop(ConsoleCommandSession &session) {
    cancel_pending(session);

    TailSessionState *tail = tail_session(session.id, false);
    if (tail) *tail = {};
}

void StorageConsoleCommands::print_stats(Print &out) {
    ConsoleFormat::print_storage_status(out, Storage::status());
}

}  // namespace aircannect
