#include "edf_recording_overview.h"

#include <stdio.h>
#include <string.h>

#include "edf_file_inventory.h"
#include "sleep_day_id.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr size_t ENTRY_BUDGET = 32;
constexpr uint32_t RETRY_MS = 10000;

int session_file_index(EdfInventoryFileKind kind) {
    switch (kind) {
        case EdfInventoryFileKind::Brp: return 0;
        case EdfInventoryFileKind::Pld: return 1;
        case EdfInventoryFileKind::Sa2: return 2;
        case EdfInventoryFileKind::Tcv: return 3;
        case EdfInventoryFileKind::Eve: return 4;
        case EdfInventoryFileKind::Csl: return 5;
        default: return -1;
    }
}

bool datalog_day(const char *path) {
    SleepDayId day;
    return path && strncmp(path, "/DATALOG/", 9) == 0 &&
           SleepDayId::from_yyyymmdd(path + 9, day);
}

}  // namespace

bool EdfRecordingOverview::Session::note_file(const char *path, uint64_t bytes) {
    EdfInventoryEntry entry;
    if (!edf_inventory_describe_path(path, entry)) return false;

    const int slot = session_file_index(entry.kind);
    if (slot < 0) return false;

    const int order = strcmp(entry.session_stamp, stamp);
    if (order < 0) return false;
    if (order > 0) {
        *this = {};
        copy_cstr(stamp, sizeof(stamp), entry.session_stamp);
    }

    const uint8_t bit = static_cast<uint8_t>(1u << slot);
    if ((present & bit) && sizes[slot] == bytes) return false;

    present |= bit;
    sizes[slot] = bytes;
    return true;
}

void EdfRecordingOverview::publish_session(bool refreshing) {
    snapshot_.ready = true;
    snapshot_.refreshing = refreshing;
    copy_cstr(snapshot_.session, sizeof(snapshot_.session), latest_.stamp);
    snapshot_.file_count = 0;
    snapshot_.bytes = 0;
    snapshot_.error[0] = '\0';

    for (size_t i = 0; i < 6; ++i) {
        if (!(latest_.present & (1u << i))) continue;

        ++snapshot_.file_count;
        snapshot_.bytes += latest_.sizes[i];
    }
    ++snapshot_.revision;
}

void EdfRecordingOverview::request_refresh() {
    restart_ = true;
    retry_at_ms_ = 0;
    if (!snapshot_.refreshing || snapshot_.error[0]) {
        snapshot_.refreshing = true;
        snapshot_.error[0] = '\0';
        ++snapshot_.revision;
    }
}

void EdfRecordingOverview::note_file(const char *path, uint64_t bytes) {
    if (!latest_.note_file(path, bytes)) return;

    mounted_ = true;
    // A boot scan must not overwrite newer writes with an older listing.
    if (phase_ != Phase::Idle || restart_) request_refresh();
    publish_session(snapshot_.refreshing);
}

void EdfRecordingOverview::path_changed(const char *path) {
    if (!path) return;
    if (strcmp(path, "/DATALOG") == 0 || datalog_day(path)) {
        request_refresh();
        return;
    }

    EdfInventoryEntry entry;
    if (edf_inventory_describe_path(path, entry) &&
        session_file_index(entry.kind) >= 0) {
        request_refresh();
    }
}

bool EdfRecordingOverview::discard_scan() {
    if (ticket_.valid() && !scan_.abandon(ticket_)) return false;

    ticket_ = {};
    days_.reset();
    files_.reset();
    candidate_ = {};
    cursor_ = 0;
    day_[0] = '\0';
    next_day_[0] = '\0';
    phase_ = Phase::Root;
    restart_ = false;
    return true;
}

void EdfRecordingOverview::fail(const char *error, uint32_t now_ms) {
    copy_cstr(snapshot_.error, sizeof(snapshot_.error), error);
    snapshot_.refreshing = false;
    ++snapshot_.revision;
    retry_at_ms_ = now_ms + RETRY_MS;
    restart_ = true;
}

bool EdfRecordingOverview::request_scan(uint32_t now_ms) {
    char path[18] = "/DATALOG";
    if (phase_ == Phase::Day) {
        snprintf(path, sizeof(path), "/DATALOG/%s", day_);
    }

    const StorageScanRoot root{path, false};
    StorageScanCommand command;
    command.roots = &root;
    command.root_count = 1;
    command.include_directories = phase_ == Phase::Root;
    command.generation = ++generation_;
    if (!command.generation) command.generation = ++generation_;

    const OperationSubmission submission = scan_.request_scan(command);
    if (submission.admission == OperationAdmission::Busy) return false;
    if (!submission.accepted()) {
        fail("edf_overview_scan_rejected", now_ms);
        return true;
    }

    ticket_ = submission.ticket;
    if (!snapshot_.refreshing || snapshot_.error[0]) {
        snapshot_.refreshing = true;
        snapshot_.error[0] = '\0';
        ++snapshot_.revision;
    }
    return true;
}

void EdfRecordingOverview::select_day() {
    for (size_t budget = ENTRY_BUDGET; cursor_ < days_->size() && budget;
         --budget, ++cursor_) {
        StorageScanEntryView entry;
        if (!days_->entry(cursor_, entry) || !entry.directory ||
            !datalog_day(entry.path)) {
            continue;
        }

        const char *day = entry.path + 9;
        if ((!day_[0] || strcmp(day, day_) < 0) &&
            strcmp(day, next_day_) > 0) {
            copy_cstr(next_day_, sizeof(next_day_), day);
        }
    }
    if (cursor_ < days_->size()) return;

    if (next_day_[0]) {
        copy_cstr(day_, sizeof(day_), next_day_);
        next_day_[0] = '\0';
        phase_ = Phase::Day;
    } else {
        latest_ = {};
        publish_session(false);
        days_.reset();
        phase_ = Phase::Idle;
    }
    cursor_ = 0;
}

void EdfRecordingOverview::read_day() {
    for (size_t budget = ENTRY_BUDGET; cursor_ < files_->size() && budget;
         --budget, ++cursor_) {
        StorageScanEntryView entry;
        if (files_->entry(cursor_, entry) && !entry.directory) {
            candidate_.note_file(entry.path, entry.size);
        }
    }
    if (cursor_ < files_->size()) return;

    files_.reset();
    cursor_ = 0;
    if (candidate_.present) {
        latest_ = candidate_;
        publish_session(false);
        days_.reset();
        phase_ = Phase::Idle;
    } else {
        phase_ = Phase::SelectDay;
    }
}

bool EdfRecordingOverview::poll(bool mounted, bool allow_scan, uint32_t now_ms) {
    if (!mounted) {
        if (mounted_ || snapshot_.refreshing || snapshot_.ready) {
            latest_ = {};
            snapshot_.ready = false;
            snapshot_.refreshing = false;
            snapshot_.session[0] = '\0';
            snapshot_.file_count = 0;
            snapshot_.bytes = 0;
            copy_cstr(snapshot_.error, sizeof(snapshot_.error),
                      "storage_unavailable");
            ++snapshot_.revision;
            restart_ = true;
        }
        mounted_ = false;
        if (restart_) return discard_scan();
        return false;
    }
    if (!mounted_) {
        mounted_ = true;
        request_refresh();
    }

    if (restart_ && !discard_scan()) return false;
    if (!allow_scan || phase_ == Phase::Idle ||
        (retry_at_ms_ && static_cast<int32_t>(now_ms - retry_at_ms_) < 0)) {
        return false;
    }

    if (ticket_.valid()) {
        StorageScanCompletion completion;
        if (!scan_.take_completion(ticket_, completion)) return false;

        ticket_ = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            !completion.snapshot) {
            fail(completion.error[0] ? completion.error : "edf_overview_scan_failed",
                 now_ms);
        } else if (phase_ == Phase::Root) {
            days_ = std::move(completion.snapshot);
            phase_ = Phase::SelectDay;
        } else {
            files_ = std::move(completion.snapshot);
            candidate_ = {};
        }
        return true;
    }

    if (phase_ == Phase::SelectDay) select_day();
    else if (files_) read_day();
    else return request_scan(now_ms);
    return true;
}

}  // namespace aircannect
