#include "console_commands.h"

#include <string.h>

#include "board_report.h"
#include "management_console_utils.h"
#include "report_sources.h"
#include "report_task.h"

namespace aircannect {
namespace {

const char *report_task_state_name(ReportTaskState state) {
    switch (state) {
        case ReportTaskState::LoadingCatalog: return "loading_catalog";
        case ReportTaskState::IndexingArtifacts: return "indexing_artifacts";
        case ReportTaskState::Idle: return "idle";
        case ReportTaskState::RefreshingCatalog: return "refreshing_catalog";
        case ReportTaskState::Queued: return "queued";
        case ReportTaskState::LookingUp: return "looking_up";
        case ReportTaskState::Building: return "building";
        case ReportTaskState::Publishing: return "publishing";
        case ReportTaskState::Stopped:
        default: return "stopped";
    }
}

const char *report_engine_state_name(ReportEngineState state) {
    switch (state) {
        case ReportEngineState::Queued: return "queued";
        case ReportEngineState::WaitingForCatalog: return "waiting_catalog";
        case ReportEngineState::LookingUp: return "looking_up";
        case ReportEngineState::AcquiringFallback: return "fallback";
        case ReportEngineState::Executing: return "executing";
        case ReportEngineState::Publishing: return "publishing";
        case ReportEngineState::Idle:
        default: return "idle";
    }
}

const char *report_payload_load_state_name(
    ReportArtifactPayloadLoadState state) {
    switch (state) {
        case ReportArtifactPayloadLoadState::Submitting: return "submitting";
        case ReportArtifactPayloadLoadState::Waiting: return "waiting";
        case ReportArtifactPayloadLoadState::Copying: return "copying";
        case ReportArtifactPayloadLoadState::Ready: return "ready";
        case ReportArtifactPayloadLoadState::Error: return "error";
        case ReportArtifactPayloadLoadState::Cancelled: return "cancelled";
        case ReportArtifactPayloadLoadState::Idle:
        default: return "idle";
    }
}

const char *report_catalog_state_name(NightCatalogRefreshState state) {
    switch (state) {
        case NightCatalogRefreshState::Scanning: return "scanning";
        case NightCatalogRefreshState::ReadingEdf: return "reading_edf";
        case NightCatalogRefreshState::ReadingMetadata:
            return "reading_metadata";
        case NightCatalogRefreshState::ReadingFallback:
            return "reading_fallback";
        case NightCatalogRefreshState::ReadingStr: return "reading_str";
        case NightCatalogRefreshState::Building: return "building";
        case NightCatalogRefreshState::Cancelling: return "cancelling";
        case NightCatalogRefreshState::Ready: return "ready";
        case NightCatalogRefreshState::Error: return "error";
        case NightCatalogRefreshState::Idle:
        default: return "idle";
    }
}

const char *report_fallback_state_name(
    ReportFallbackAcquisitionState state) {
    switch (state) {
        case ReportFallbackAcquisitionState::Preserving:
            return "preserving";
        case ReportFallbackAcquisitionState::Fetching: return "fetching";
        case ReportFallbackAcquisitionState::Publishing:
            return "publishing";
        case ReportFallbackAcquisitionState::Cancelling:
            return "cancelling";
        case ReportFallbackAcquisitionState::Ready: return "ready";
        case ReportFallbackAcquisitionState::Failed: return "failed";
        case ReportFallbackAcquisitionState::Cancelled:
            return "cancelled";
        case ReportFallbackAcquisitionState::Idle:
        default: return "idle";
    }
}

void print_report_status(Print &out, const ReportTask &task) {
    const ReportTaskDiagnosticSnapshot status = task.diagnostic_snapshot();

    out.print("[REPORT] state=");
    out.print(report_task_state_name(status.state));
    out.print(" started=");
    out.print(status.task_started ? "yes" : "no");
    out.print(" nights=");
    out.print(static_cast<unsigned long>(status.catalog_nights));
    out.print(" generation=");
    out.print(static_cast<unsigned long>(status.catalog_generation));
    out.print(" commands=");
    out.print(static_cast<unsigned long>(status.commands_queued));
    out.print(" dropped=");
    out.print(static_cast<unsigned long>(status.command_drops));
    out.print(" failed=");
    out.println(static_cast<unsigned long>(status.command_failures));

    out.print("[REPORT] engine=");
    out.print(report_engine_state_name(status.engine_state));
    out.print(" queued=");
    out.print(static_cast<unsigned long>(status.engine_queued));
    out.print(" foreground=");
    out.print(status.foreground_active ? "yes" : "no");
    out.print(" background=");
    out.print(status.background_active ? "yes" : "no");
    out.print(" suspended=");
    out.print(status.background_suspended ? "yes" : "no");
    out.print(" last_error=");
    out.println(status.engine_error[0] ? status.engine_error : "--");

    out.print("[REPORT] payload_cache=");
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
    out.print(static_cast<unsigned long>(status.payload_cache_evictions));
    out.print(" load=");
    out.print(report_payload_load_state_name(status.payload_load_state));
    out.print(" loaded=");
    out.print(static_cast<unsigned long>(status.payload_load_bytes));
    out.print(" error=");
    out.println(status.payload_load_error[0]
                    ? status.payload_load_error
                    : "--");

    const ReportSourceDef *fallback_source =
        report_source_def(status.fallback_source);
    out.print("[REPORT] fallback=");
    out.print(report_fallback_state_name(status.fallback_state));
    out.print(" source=");
    out.print(fallback_source && fallback_source->spool_type
                  ? fallback_source->spool_type
                  : "--");
    out.print(" sources=");
    out.print(static_cast<unsigned long>(
        status.fallback_sources_completed));
    out.print('/');
    out.print(static_cast<unsigned long>(status.fallback_sources_total));
    out.print(" sections=");
    out.print(static_cast<unsigned long>(status.fallback_sections_added));
    out.print(" unavailable=");
    out.print(static_cast<unsigned long>(
        status.fallback_unavailable_added));
    out.print(" error=");
    out.println(status.fallback_error[0] ? status.fallback_error : "--");

    out.print("[REPORT] catalog=");
    out.print(report_catalog_state_name(status.catalog_state));
    out.print(" files=");
    out.print(static_cast<unsigned long>(status.catalog_files_indexed));
    out.print('/');
    out.print(static_cast<unsigned long>(status.catalog_files_seen));
    out.print(" sessions=");
    out.print(static_cast<unsigned long>(status.catalog_sessions));
    out.print(" error=");
    out.println(status.catalog_error[0] ? status.catalog_error : "--");
}

uint32_t report_night_duration(const NightCatalog &catalog,
                               const NightCatalogRecord &night) {
    if (night.metrics.has(NightCatalogMetric::DurationMinutes)) {
        return night.metrics.duration_min;
    }

    uint64_t duration_ms = 0;
    size_t session_count = 0;
    const NightCatalogTimeRange *sessions =
        catalog.sessions(night, session_count);
    for (size_t i = 0; sessions && i < session_count; ++i) {
        if (!sessions[i].valid()) continue;
        duration_ms += static_cast<uint64_t>(
            sessions[i].end_ms - sessions[i].start_ms);
    }
    return static_cast<uint32_t>(duration_ms / 60000);
}

void print_report_nights(Print &out, const ReportTask &task) {
    const std::shared_ptr<const NightCatalog> catalog =
        task.catalog_snapshot();
    out.println("[REPORT nights]");
    if (!catalog || !catalog->size()) {
        out.println("  no therapy nights indexed");
        return;
    }

    for (size_t i = 0; i < catalog->size(); ++i) {
        const NightCatalogRecord *night = catalog->record(i);
        if (!night) continue;

        char day[9] = {};
        night->sleep_day.format_yyyymmdd(day, sizeof(day));
        out.print("  ");
        out.print(day);
        out.print(" duration_min=");
        out.print(static_cast<unsigned long>(
            report_night_duration(*catalog, *night)));
        out.print(" sessions=");
        out.print(static_cast<unsigned long>(night->session_count));
        out.print(" sources=0x");
        out.println(static_cast<unsigned>(night->source_flags), HEX);
    }
}

bool parse_report_sleep_day(String value, SleepDayId &sleep_day) {
    value.trim();
    return value.length() == 8 &&
           SleepDayId::from_yyyymmdd(value.c_str(), sleep_day);
}

}  // namespace

ReportConsoleCommands::ReportConsoleCommands(ReportTask &report)
    : report_(report) {}

bool ReportConsoleCommands::execute(const String &command,
                                    const String &rest_arg,
                                    Print &out,
                                    ConsoleCommandSession &session) {
    (void)session;
    if (command != "report") return false;

    String rest = rest_arg;
    rest.trim();
    rest.toLowerCase();
    if (!rest.length() || rest == "status") {
        print_report_status(out, report_);
        return true;
    }
    if (rest == "nights" || rest == "list") {
        print_report_nights(out, report_);
        return true;
    }
    if (rest == "result") {
        out.println("[REPORT] usage: report result latest|YYYYMMDD");
        return true;
    }
    if (!rest.startsWith("result ")) {
        print_unknown_command(
            out, "REPORT",
            "report, report status, report nights, "
            "report result latest|YYYYMMDD");
        return true;
    }

    String value = rest.substring(strlen("result "));
    value.trim();

    const std::shared_ptr<const NightCatalog> catalog =
        report_.catalog_snapshot();
    if (!catalog) {
        out.println("[REPORT] night catalog unavailable");
        return true;
    }

    SleepDayId sleep_day;
    if (value == "latest") {
        const NightCatalogRecord *latest = catalog->record(0);
        if (!latest) {
            out.println("[REPORT] no indexed nights");
            return true;
        }
        sleep_day = latest->sleep_day;
    } else if (!parse_report_sleep_day(value, sleep_day)) {
        out.println("[REPORT] usage: report result latest|YYYYMMDD");
        return true;
    }

    const NightCatalogRecord *night = catalog->find(sleep_day);
    if (!night) {
        out.println("[REPORT] night not found");
        return true;
    }

    ++request_generation_;
    if (!request_generation_) ++request_generation_;

    const OperationAdmission admitted = report_.request_artifact(
        ReportArtifactKey::result(sleep_day, night->source_revision),
        ReportRequestPriority::Foreground, request_generation_);
    if (admitted == OperationAdmission::Accepted) {
        out.println("[REPORT] result requested");
    } else if (admitted == OperationAdmission::Busy) {
        out.println("[REPORT] request queue busy");
    } else {
        out.println("[REPORT] request rejected");
    }
    return true;
}

}  // namespace aircannect
