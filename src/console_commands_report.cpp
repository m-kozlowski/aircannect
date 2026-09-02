#include "console_commands.h"

#include <string.h>

#include "board_report.h"
#include "management_console_utils.h"
#include "report_task.h"

namespace aircannect {
namespace {

const char *report_condition_name(ReportTaskCondition condition) {
    switch (condition) {
        case ReportTaskCondition::Working: return "working";
        case ReportTaskCondition::Waiting: return "waiting";
        case ReportTaskCondition::Complete: return "complete";
        case ReportTaskCondition::Failed: return "failed";
        case ReportTaskCondition::Stopped:
        default: return "stopped";
    }
}

const char *report_operation_name(ReportTaskOperation operation) {
    switch (operation) {
        case ReportTaskOperation::LoadingCatalog: return "loading_catalog";
        case ReportTaskOperation::RefreshingCatalog:
            return "refreshing_catalog";
        case ReportTaskOperation::CheckingSpools: return "checking_spools";
        case ReportTaskOperation::Reconciling: return "reconciling";
        case ReportTaskOperation::LookingUp: return "looking_up";
        case ReportTaskOperation::Building: return "building";
        case ReportTaskOperation::Publishing: return "publishing";
        case ReportTaskOperation::LoadingPayload: return "loading_payload";
        case ReportTaskOperation::CompressingPayload:
            return "compressing_payload";
        case ReportTaskOperation::SavingCatalog: return "saving_catalog";
        case ReportTaskOperation::None:
        default: return "--";
    }
}

const char *report_wait_reason_name(ReportTaskWaitReason reason) {
    switch (reason) {
        case ReportTaskWaitReason::Startup: return "startup";
        case ReportTaskWaitReason::Queue: return "queue";
        case ReportTaskWaitReason::Catalog: return "catalog";
        case ReportTaskWaitReason::Retry: return "retry";
        case ReportTaskWaitReason::Therapy: return "therapy";
        case ReportTaskWaitReason::RealtimeStream: return "stream";
        case ReportTaskWaitReason::ForegroundRequest:
            return "foreground_request";
        case ReportTaskWaitReason::Ota: return "ota";
        case ReportTaskWaitReason::Export: return "export";
        case ReportTaskWaitReason::As11Unavailable:
            return "as11_unavailable";
        case ReportTaskWaitReason::None:
        default: return "--";
    }
}

void print_report_sleep_day(Print &out, SleepDayId sleep_day) {
    char day[9] = {};
    if (!sleep_day.format_yyyymmdd(day, sizeof(day))) return;

    out.print(" night=");
    out.print(day);
}

void print_report_status(Print &out, const ReportTask &task) {
    const ReportTaskOperationalSnapshot status =
        task.operational_snapshot();

    out.print("[REPORT] state=");
    out.print(report_condition_name(status.condition));
    if (status.condition == ReportTaskCondition::Working) {
        out.print(" operation=");
        out.print(report_operation_name(status.operation));
    } else if (status.condition == ReportTaskCondition::Waiting) {
        out.print(" reason=");
        out.print(report_wait_reason_name(status.wait_reason));
    }

    print_report_sleep_day(out, status.sleep_day);
    if (status.retry_in_ms) {
        out.print(" retry_in_ms=");
        out.print(static_cast<unsigned long>(status.retry_in_ms));
    }
    if (status.condition == ReportTaskCondition::Complete) {
        out.print(" nights=");
        out.print(static_cast<unsigned long>(status.catalog_nights));
    }
    if (status.error[0]) {
        out.print(" error=");
        out.print(status.error);
    }
    out.println();
}

void print_report_stats(Print &out, const ReportTask &task) {
    const ReportTaskDiagnosticSnapshot status = task.diagnostic_snapshot();

    out.print("[REPORT queue] task=");
    out.print(static_cast<unsigned long>(status.commands_queued));
    out.print(" engine=");
    out.print(static_cast<unsigned long>(status.engine_queued));
    out.print(" dropped=");
    out.print(static_cast<unsigned long>(status.command_drops));
    out.print(" failed=");
    out.println(static_cast<unsigned long>(status.command_failures));

    out.print("[REPORT cache] entries=");
    out.print(static_cast<unsigned long>(status.payload_cache_entries));
    out.print('/');
    out.print(static_cast<unsigned long>(
        AC_REPORT_PAYLOAD_CACHE_ENTRY_CAPACITY));
    out.print(" bytes=");
    out.print(static_cast<unsigned long>(status.payload_cache_bytes));
    out.print(" hits=");
    out.print(static_cast<unsigned long>(status.payload_cache_hits));
    out.print(" misses=");
    out.print(static_cast<unsigned long>(status.payload_cache_misses));
    out.print(" evictions=");
    out.println(static_cast<unsigned long>(status.payload_cache_evictions));

    out.print("[REPORT catalog] nights=");
    out.print(static_cast<unsigned long>(status.catalog_nights));
    out.print(" files=");
    out.print(static_cast<unsigned long>(status.catalog_files_indexed));
    out.print('/');
    out.print(static_cast<unsigned long>(status.catalog_files_seen));
    out.print(" sessions=");
    out.print(static_cast<unsigned long>(status.catalog_sessions));
    out.print(" generation=");
    out.print(static_cast<unsigned long>(status.catalog_generation));
    out.print('/');
    out.println(static_cast<unsigned long>(
        status.durable_catalog_generation));
}

bool parse_report_sleep_day(String value, SleepDayId &sleep_day) {
    value.trim();
    return value.length() == 8 &&
           SleepDayId::from_yyyymmdd(value.c_str(), sleep_day);
}

const NightCatalogRecord *select_report_night(const NightCatalog &catalog,
                                              const String &selector,
                                              bool &valid_selector) {
    valid_selector = true;
    if (selector == "latest") return catalog.record(0);

    SleepDayId sleep_day;
    if (!parse_report_sleep_day(selector, sleep_day)) {
        valid_selector = false;
        return nullptr;
    }
    return catalog.find(sleep_day);
}

void print_report_night(Print &out,
                        const NightCatalog &catalog,
                        const NightCatalogRecord &night) {
    char day[9] = {};
    night.sleep_day.format_yyyymmdd(day, sizeof(day));
    out.print("  ");
    out.print(day);
    out.print(" duration_min=");
    out.print(static_cast<unsigned long>(
        night_catalog_duration_minutes(catalog, night)));
    out.print(" sessions=");
    out.print(static_cast<unsigned long>(night.session_count));
    out.print(" sources=0x");
    out.println(static_cast<unsigned>(night.source_flags), HEX);
}

void print_report_list(Print &out,
                       const NightCatalog &catalog,
                       const NightCatalogRecord *selected = nullptr) {
    out.println("[REPORT list]");
    if (selected) {
        print_report_night(out, catalog, *selected);
        return;
    }
    if (!catalog.size()) {
        out.println("  no therapy nights indexed");
        return;
    }

    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *night = catalog.record(i);
        if (night) print_report_night(out, catalog, *night);
    }
}

std::shared_ptr<const NightCatalog> report_catalog(
    const ReportTask &task,
    Print &out) {
    const std::shared_ptr<const NightCatalog> catalog =
        task.catalog_snapshot();
    if (!catalog) out.println("[REPORT] night catalog unavailable");
    return catalog;
}

}  // namespace

ReportConsoleCommands::ReportConsoleCommands(ReportTask &report)
    : report_(report) {}

bool ReportConsoleCommands::execute(const String &command,
                                    const String &rest_arg,
                                    Print &out,
                                    ConsoleCommandSession &session) {
    if (command != "report") return false;

    String rest = rest_arg;
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        print_report_status(out, report_);
        return true;
    }
    if (rest == "list") {
        const std::shared_ptr<const NightCatalog> catalog =
            report_catalog(report_, out);
        if (!catalog) return true;

        print_report_list(out, *catalog);
        return true;
    }
    if (rest.startsWith("list ")) {
        const int separator = rest.indexOf(' ');
        String selector = rest.substring(separator + 1);
        selector.trim();

        const std::shared_ptr<const NightCatalog> catalog =
            report_catalog(report_, out);
        if (!catalog) return true;

        bool valid_selector = false;
        const NightCatalogRecord *night = select_report_night(
            *catalog, selector, valid_selector);
        if (!valid_selector) {
            out.println("[REPORT] usage: report list [latest|YYYYMMDD]");
        } else if (!night) {
            out.println("[REPORT] night not found");
        } else {
            print_report_list(out, *catalog, night);
        }
        return true;
    }
    if (rest == "rebuild") {
        out.println("[REPORT] usage: report rebuild latest|YYYYMMDD");
        return true;
    }
    if (!rest.startsWith("rebuild ")) {
        print_unknown_command(
            out, "REPORT",
            "report, report status, report list [latest|YYYYMMDD], "
            "report rebuild latest|YYYYMMDD");
        return true;
    }

    const String args = rest.substring(strlen("rebuild "));
    int position = 0;
    String selector;
    String extra;
    if (!parse_console_arg(args, position, selector) ||
        parse_console_arg(args, position, extra)) {
        out.println("[REPORT] usage: report rebuild latest|YYYYMMDD");
        return true;
    }

    const std::shared_ptr<const NightCatalog> catalog =
        report_catalog(report_, out);
    if (!catalog) return true;

    bool valid_selector = false;
    const NightCatalogRecord *night = select_report_night(
        *catalog, selector, valid_selector);
    if (!valid_selector) {
        out.println("[REPORT] usage: report rebuild latest|YYYYMMDD");
        return true;
    }
    if (!night) {
        out.println("[REPORT] night not found");
        return true;
    }

    ++request_generation_;
    if (!request_generation_) ++request_generation_;

    const OperationAdmission admitted = report_.request_artifact(
        ReportArtifactKey::result(night->sleep_day, night->source_revision),
        ReportRequestPriority::Foreground,
        request_generation_,
        true);
    if (admitted == OperationAdmission::Accepted) {
        request_session_id_ = session.id;
        request_wait_generation_ = request_generation_;
        request_wait_artifact_ = ReportArtifactKey::result(
            night->sleep_day, night->source_revision);

        out.print("[REPORT] rebuild requested");
        print_report_sleep_day(out, night->sleep_day);
        out.println();
    } else if (admitted == OperationAdmission::Busy) {
        out.println("[REPORT] request queue busy");
    } else {
        out.println("[REPORT] request rejected");
    }
    return true;
}

void ReportConsoleCommands::poll_pending(
    Print &out,
    ConsoleCommandSession &session) {
    if (!request_session_id_ || session.id != request_session_id_) return;

    const ReportEngineCompletion completion =
        report_.last_artifact_completion();
    if (!completion.valid() ||
        completion.request.ticket.generation != request_wait_generation_ ||
        completion.request.artifact != request_wait_artifact_) {
        return;
    }

    if (completion.outcome.disposition ==
        OperationDisposition::Succeeded) {
        out.print("[REPORT] rebuild complete");
    } else {
        out.print("[REPORT] rebuild failed");
    }
    print_report_sleep_day(out, request_wait_artifact_.sleep_day);
    if (completion.outcome.disposition !=
        OperationDisposition::Succeeded) {
        out.print(" error=");
        out.print(completion.error[0]
                      ? completion.error
                      : "report_build_failed");
    }
    out.println();

    request_session_id_ = 0;
    request_wait_generation_ = 0;
    request_wait_artifact_ = {};
}

bool ReportConsoleCommands::pending_output(
    const ConsoleCommandSession &session) const {
    return request_session_id_ && request_wait_generation_ &&
        session.id == request_session_id_;
}

void ReportConsoleCommands::cancel_pending(
    ConsoleCommandSession &session) {
    if (session.id != request_session_id_) return;

    request_session_id_ = 0;
    request_wait_generation_ = 0;
    request_wait_artifact_ = {};
}

void ReportConsoleCommands::stop(ConsoleCommandSession &session) {
    cancel_pending(session);
}

bool ReportConsoleCommands::print_scoped_stats(const String &scope,
                                               Print &out) {
    if (scope != "report") return false;

    print_report_stats(out, report_);
    return true;
}

}  // namespace aircannect
