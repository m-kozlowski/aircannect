#include "console_commands.h"

#include "board.h"
#include "management_console_utils.h"
#include "ota_manager.h"
#include "resmed_ota_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

void print_ota_status(Print &out, const OtaManagerStatus &status) {
    out.print("[OTA] arduino=");
    out.print(status.arduino_started ? "started" : "stopped");
    if (status.arduino_active) out.print("/active");
    out.print(" port=");
    out.print(status.arduino_port);
    out.print(" auth=");
    out.print(status.auth_enabled ? "on" : "off");
    out.print(" http=");
    out.print(status.http_prepare_pending
                  ? "preparing"
                  : (status.http_prepared
                         ? "prepared"
                         : (status.http_active
                                ? "active"
                                : (status.http_ready ? "ready" : "idle"))));
    out.print(" method=");
    out.print(status.method);
    if (status.encoding != "plain") {
        out.print(" encoding=");
        out.print(status.encoding);
    }
    out.print(" bytes=");
    out.print(static_cast<unsigned long>(status.bytes));
    out.print("/");
    out.print(static_cast<unsigned long>(status.total_size));
    if (status.encoding != "plain") {
        out.print(" wire=");
        out.print(static_cast<unsigned long>(status.wire_bytes));
        out.print("/");
        out.print(static_cast<unsigned long>(status.wire_total_size));
    }
    out.print(" progress=");
    out.print(status.progress_percent);
    out.print("% partition=");
    out.print(status.partition.length() ? status.partition : "--");
    if (status.reboot_pending) out.print(" restart=pending");
    if (status.last_error.length()) {
        out.print(" error=");
        out.print(status.last_error);
    }
    out.println();

    const char *update_state = "unknown";
    if (!status.update_check_enabled) update_state = "disabled";
    else if (status.update_check_active) update_state = "checking";
    else if (status.update_check_pending) update_state = "queued";
    else if (status.update_available) update_state = "available";
    else if (status.update_checked) update_state = "current";

    out.print("[OTA update] state=");
    out.print(update_state);
    if (status.update_version.length()) {
        out.print(" latest=");
        out.print(status.update_version);
    }
    if (status.update_available) {
        out.print(" installable=");
        out.print(status.update_installable ? "yes" : "no");
    }
    if (status.update_check_attempted) {
        out.print(" checked_age_ms=");
        out.print(status.update_last_check_age_ms);
    }
    if (status.update_error.length()) {
        out.print(" error=");
        out.print(status.update_error);
    }
    out.println();
}

void handle_ota(Print &out,
                String rest,
                OtaManager &ota,
                ResmedOtaManager &resmed_ota) {
    trim_inplace(rest);
    int pos = 0;
    String command;
    if (!parse_console_arg(rest, pos, command)) command = "status";
    to_lower_inplace(command);

    if (command == "status") {
        print_ota_status(out, ota.status());
        return;
    }

    if (command == "check") {
        if (!ota.request_update_check()) {
            const OtaManagerStatus status = ota.status();
            out.print("[OTA] update check rejected: ");
            out.println(status.update_error.length()
                            ? status.update_error
                            : "error");
            return;
        }
        out.println("[OTA] update check queued");
        return;
    }

    if (command == "install") {
        if (!ota.request_available_update()) {
            const OtaManagerStatus status = ota.status();
            out.print("[OTA] update install rejected: ");
            out.println(status.last_error.length()
                            ? status.last_error
                            : "error");
            return;
        }
        out.println("[OTA] update install queued");
        return;
    }

    if (command == "abort") {
        ota.request_abort("aborted_by_console");
        out.println("[OTA] aborted");
        return;
    }

    if (command == "url") {
        String url;
        if (!parse_console_arg(rest, pos, url)) {
            out.println("[OTA] URL is required");
            return;
        }

        String extra;
        if (parse_console_arg(rest, pos, extra)) {
            out.println("[OTA] too many arguments");
            return;
        }
        if (resmed_ota.transport_active()) {
            out.println("[OTA] ResMed OTA transport is active");
            return;
        }
        if (!ota.request_url_update(url)) {
            const OtaManagerStatus status = ota.status();
            out.print("[OTA] URL update rejected: ");
            out.println(status.last_error.length() ? status.last_error
                                                   : "error");
            return;
        }
        out.println("[OTA] URL update queued");
        return;
    }

    print_unknown_command(out, "OTA",
                          "ota status, check, install, abort, url URL");
}

void handle_resmed_ota(Print &out,
                       String rest,
                       ResmedOtaManager &resmed_ota) {
    trim_inplace(rest);

    if (!rest.length() || rest == "status") {
        const ResmedOtaStatus status = resmed_ota.status();
        out.print("[RESMED OTA] phase=");
        out.print(resmed_ota.phase_name());
        out.print(" waiting=");
        out.print(status.waiting ? "yes" : "no");
        out.print(" file=\"");
        out.print(status.filename);
        out.print("\" total=");
        out.print(static_cast<unsigned long>(status.total_size));
        out.print(" uploaded=");
        out.print(static_cast<unsigned long>(status.uploaded_bytes));
        out.print(" progress=");
        out.print(status.progress_percent);
        out.print("% block=");
        out.print(static_cast<unsigned long>(status.xfer_block_size));
        if (status.computed_sha256.length()) {
            out.print(" sha256=");
            out.print(status.computed_sha256);
        }
        if (status.apply_mode.length()) {
            out.print(" apply=");
            out.print(status.apply_mode);
        }
        if (status.last_error.length()) {
            out.print(" error=");
            out.print(status.last_error);
        }
        out.println();
        return;
    }

    if (rest == "check") {
        if (resmed_ota.request_check()) {
            out.println("[RESMED OTA] CheckUpgradeFile queued");
        } else {
            const ResmedOtaStatus status = resmed_ota.status();
            out.print("[RESMED OTA] check failed: ");
            out.println(status.last_error);
        }
        return;
    }

    if (rest == "abort") {
        resmed_ota.abort("aborted_by_console");
        out.println("[RESMED OTA] aborted");
        return;
    }

    if (rest.startsWith("apply ")) {
        String args = rest.substring(6);
        int pos = 0;
        String mode;
        if (!parse_console_arg(args, pos, mode)) {
            out.println("[RESMED OTA] usage: resmed-ota apply plain CONFIRM");
            return;
        }
        to_lower_inplace(mode);

        if (mode == "plain") {
            String confirm;
            if (!parse_console_arg(args, pos, confirm)) {
                out.print("[RESMED OTA] confirmation required: ");
                out.println(AC_RESMED_OTA_CONFIRM);
                return;
            }
            if (resmed_ota.request_apply_plain(false, confirm)) {
                out.println("[RESMED OTA] ApplyUpgrade queued");
            } else {
                const ResmedOtaStatus status = resmed_ota.status();
                out.print("[RESMED OTA] apply failed: ");
                out.println(status.last_error);
            }
            return;
        }

        if (mode == "authenticated") {
            String tag;
            String confirm;
            if (!parse_console_arg(args, pos, tag) ||
                !parse_console_arg(args, pos, confirm)) {
                out.println(
                    "[RESMED OTA] usage: resmed-ota apply authenticated "
                    "TAG CONFIRM");
                return;
            }
            if (resmed_ota.request_apply_authenticated(tag, confirm)) {
                out.println("[RESMED OTA] ApplyAuthenticatedUpgrade queued");
            } else {
                const ResmedOtaStatus status = resmed_ota.status();
                out.print("[RESMED OTA] apply failed: ");
                out.println(status.last_error);
            }
            return;
        }
    }

    print_unknown_command(out, "RESMED OTA", "status, check, abort, apply");
}

}  // namespace

OtaConsoleCommands::OtaConsoleCommands(OtaManager &ota,
                                       ResmedOtaManager &resmed_ota)
    : ota_(ota), resmed_ota_(resmed_ota) {}

bool OtaConsoleCommands::execute(const String &command,
                                 const String &rest_arg,
                                 Print &out,
                                 ConsoleCommandSession &session) {
    (void)session;
    if (command != "ota" && command != "resmed-ota" &&
        command != "restart") {
        return false;
    }

    String rest = rest_arg;

    if (command == "ota") {
        handle_ota(out, rest, ota_, resmed_ota_);
        return true;
    }
    if (command == "resmed-ota") {
        handle_resmed_ota(out, rest, resmed_ota_);
        return true;
    }
    if (command == "restart") {
        trim_inplace(rest);
        if (rest.length()) {
            print_unknown_command(out, "SYSTEM", "restart");
        } else {
            ota_.schedule_reboot(500);
            out.println("[SYSTEM] restart scheduled");
        }
        return true;
    }
    return false;
}

}  // namespace aircannect
