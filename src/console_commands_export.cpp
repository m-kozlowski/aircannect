#include "console_commands.h"

#include <stdio.h>

#include "export_coordinator.h"
#include "management_console_utils.h"
#include "storage_export_plan.h"
#include "string_util.h"

namespace aircannect {
namespace {

void print_uint64(Print &out, uint64_t value) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%llu",
             static_cast<unsigned long long>(value));
    out.print(buffer);
}

void handle_smb(Print &out, String rest, ExportCoordinator &exports) {
    trim_inplace(rest);
    to_lower_inplace(rest);

    if (!rest.length() || rest == "status") {
        const StorageSyncStatus status = exports.smb_status();
        out.print("[SMB] state=");
        out.print(storage_sync_state_name(status.state));
        out.print(" configured=");
        out.print(status.configured ? "yes" : "no");
        out.print(" network=");
        out.print(status.network_available ? "yes" : "no");
        out.print(" pending=");
        out.print(status.pending ? "yes" : "no");
        out.print(" reason=");
        out.print(status.pending_reason[0] ? status.pending_reason : "--");
        out.print(" endpoint=");
        out.print(status.endpoint_set ? "set" : "--");
        out.print(" user=");
        out.print(status.user_set ? "set" : "--");
        out.print(" last_verify=");
        print_uint64(out, status.last_verify_epoch);
        out.print(" last_sync=");
        print_uint64(out, status.last_sync_epoch);
        out.print(" last_failure=");
        print_uint64(out, status.last_failure_epoch);
        out.print(" error=");
        if (status.last_error[0]) {
            out.print(status.last_error);
        } else if (status.last_failure_error[0]) {
            out.print(status.last_failure_error);
        } else {
            out.print("--");
        }
        out.print(" files=");
        out.print(static_cast<unsigned>(status.files_uploaded));
        out.print("/");
        out.print(static_cast<unsigned>(status.files_seen));
        out.print(" skipped=");
        out.print(static_cast<unsigned>(status.files_skipped));
        out.print(" failed=");
        out.print(static_cast<unsigned>(status.files_failed));
        out.print(" bytes=");
        print_uint64(out, status.bytes_uploaded);
        out.print(" current=");
        out.print(status.current_path[0] ? status.current_path : "--");
        out.println();
        return;
    }

    if (rest == "verify" || rest == "check") {
        const bool queued = exports.request_smb_verify();
        out.print("[SMB] verify ");
        out.println(queued ? "queued" : "rejected");
        return;
    }

    if (rest == "sync") {
        const bool queued = exports.request_smb_sync();
        out.print("[SMB] sync ");
        out.println(queued ? "queued" : "rejected");
        return;
    }

    print_unknown_command(out, "SMB", "smb status, smb verify, smb sync");
}

void handle_sleephq(Print &out,
                    String rest,
                    ExportCoordinator &exports) {
    trim_inplace(rest);
    to_lower_inplace(rest);

    if (!rest.length() || rest == "status") {
        const SleepHqSyncStatus status = exports.sleephq_status();
        out.print("[SLEEPHQ] state=");
        out.print(sleephq_sync_state_name(status.state));
        out.print(" configured=");
        out.print(status.configured ? "yes" : "no");
        out.print(" network=");
        out.print(status.network_available ? "yes" : "no");
        out.print(" pending=");
        out.print(status.pending ? "yes" : "no");
        out.print(" reason=");
        out.print(status.pending_reason[0] ? status.pending_reason : "--");
        out.print(" team_id=");
        if (status.team_id) {
            out.print(static_cast<unsigned long>(status.team_id));
        } else {
            out.print("--");
        }
        out.print(" last_check=");
        print_uint64(out, status.last_check_epoch);
        out.print(" last_sync=");
        print_uint64(out, status.last_sync_epoch);
        out.print(" last_failure=");
        print_uint64(out, status.last_failure_epoch);
        out.print(" error=");
        out.print(status.last_error[0] ? status.last_error : "--");
        out.print(" import=");
        if (status.import_id) {
            out.print(static_cast<unsigned long>(status.import_id));
        } else {
            out.print("--");
        }
        out.print(" files=");
        out.print(static_cast<unsigned>(status.files_uploaded));
        out.print("/");
        out.print(static_cast<unsigned>(status.files_seen));
        out.print(" skipped=");
        out.print(static_cast<unsigned>(status.files_skipped));
        out.print(" bytes=");
        print_uint64(out, status.bytes_uploaded);
        out.print(" current=");
        out.print(status.current_path[0] ? status.current_path : "--");
        out.println();
        return;
    }

    if (rest == "check" || rest == "verify") {
        const bool queued = exports.request_sleephq_check();
        out.print("[SLEEPHQ] check ");
        out.println(queued ? "queued" : "rejected");
        return;
    }

    if (rest == "sync") {
        const bool queued = exports.request_sleephq_sync();
        out.print("[SLEEPHQ] sync ");
        out.println(queued ? "queued" : "rejected");
        return;
    }

    if (rest.startsWith("sync ")) {
        String day = rest.substring(5);
        trim_inplace(day);
        if (!storage_export_is_datalog_day_name(day.c_str())) {
            out.println("[SLEEPHQ] bad day; use YYYYMMDD");
            return;
        }

        const bool queued = exports.request_sleephq_sync_day(day.c_str());
        out.print("[SLEEPHQ] sync day=");
        out.print(day);
        out.print(" ");
        out.println(queued ? "queued" : "rejected");
        return;
    }

    print_unknown_command(out, "SLEEPHQ",
                          "sleephq status, sleephq check, sleephq sync, "
                          "sleephq sync YYYYMMDD");
}

}  // namespace

ExportConsoleCommands::ExportConsoleCommands(ExportCoordinator &exports)
    : exports_(exports) {}

bool ExportConsoleCommands::execute(const String &command,
                                    const String &rest,
                                    Print &out,
                                    ConsoleCommandSession &session) {
    (void)session;

    if (command == "smb") {
        handle_smb(out, rest, exports_);
        return true;
    }
    if (command == "sleephq") {
        handle_sleephq(out, rest, exports_);
        return true;
    }
    return false;
}

}  // namespace aircannect
