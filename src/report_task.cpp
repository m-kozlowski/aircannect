#include "report_task.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>
#include <utility>

#include "board_report.h"
#include "night_catalog_builder.h"
#include "night_catalog_capture.h"
#include "report_fallback_artifact.h"
#include "report_spool_availability.h"
#include "storage_service.h"
#include "string_util.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "debug_log.h"
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr uint32_t CATALOG_STORE_GENERATION = 1;
constexpr uint32_t CATALOG_SESSION_SETTLE_MS = 5000;
constexpr uint32_t CATALOG_RECONCILE_IDLE_MS = 5 * 60 * 1000;
constexpr uint32_t CATALOG_RETRY_MIN_MS = 1000;
constexpr uint32_t CATALOG_RETRY_MAX_MS = 30000;
constexpr uint32_t MATERIALIZE_RETRY_MS = 10 * 60 * 1000;
constexpr uint32_t SPOOL_AVAILABILITY_RETRY_MS = 10 * 60 * 1000;

enum class ReportTaskCommandKind : uint8_t {
    MaterializeNight,
    Rebuild,
};

struct ReportTaskCommand {
    ReportTaskCommandKind kind = ReportTaskCommandKind::MaterializeNight;
    SleepDayId sleep_day;
    ReportRequestPriority priority = ReportRequestPriority::Foreground;
    bool force_rebuild = false;
    uint32_t generation = 0;
    SleepDayId first_day;
    SleepDayId last_day;
};

struct PendingCatalogRefresh {
    uint32_t generation = 0;
    uint32_t due_ms = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
    bool summary_attempted = false;
    bool post_therapy = false;
    NightCatalogRefreshTarget target;

    bool valid() const { return generation != 0; }
    void clear() { *this = {}; }
};

struct PendingSessionEnded {
    bool pending = false;
    bool full_reconcile = false;
    NightCatalogRefreshTarget target;
};

struct ReportNightFailureEntry {
    SleepDayId sleep_day;
    SourceRevision source_revision;
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t retry_at_ms = 0;
    bool retryable = true;

    bool valid() const {
        return sleep_day.valid() && source_revision.valid() && error[0];
    }
};

struct ReportPublishedState {
    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const ReportSignalStoreCatalog> store_catalog;
    DisplayReportSummary display_summary;
};

enum class CatalogStorePurpose : uint8_t {
    None,
    Load,
    Save,
};

uint32_t increment_generation(uint32_t generation) {
    ++generation;
    return generation == 0 ? 1 : generation;
}

uint32_t monotonic_generation(uint32_t requested, uint32_t current) {
    if (current == 0 || static_cast<int32_t>(requested - current) > 0) {
        return requested;
    }
    return increment_generation(current);
}

bool deadline_due(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_ms == 0 ||
           static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

uint32_t earlier_deadline(uint32_t left, uint32_t right) {
    if (left == 0 || right == 0) return 0;
    return static_cast<int32_t>(left - right) < 0 ? left : right;
}

bool same_refresh_target(const NightCatalogRefreshTarget &left,
                         const NightCatalogRefreshTarget &right) {
    return left.valid() && right.valid() &&
           left.sleep_day == right.sleep_day &&
           strcmp(left.datalog_sleep_day, right.datalog_sleep_day) == 0;
}

uint32_t deadline_remaining(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_due(now_ms, deadline_ms) ? 0 : deadline_ms - now_ms;
}

uint32_t retry_delay(uint8_t attempt) {
    uint32_t delay = CATALOG_RETRY_MIN_MS;
    for (uint8_t i = 0; i < attempt && delay < CATALOG_RETRY_MAX_MS; ++i) {
        delay = std::min(delay * 2, CATALOG_RETRY_MAX_MS);
    }
    return delay;
}

void advance_retry(uint8_t &attempt) {
    if (attempt < 5) ++attempt;
}

bool local_source_available(const NightCatalogRecord &night) {
    return (night.source_flags &
            (NIGHT_CATALOG_SOURCE_EDF |
             NIGHT_CATALOG_SOURCE_SPOOL_FALLBACK)) != 0;
}

int64_t align_block_start(int64_t timestamp_ms) {
    int64_t remainder = timestamp_ms % REPORT_SIGNAL_STORE_BLOCK_MS;
    if (remainder < 0) remainder += REPORT_SIGNAL_STORE_BLOCK_MS;
    return timestamp_ms - remainder;
}

}  // namespace

struct ReportTask::Runtime {
    Runtime() : engine(build_slots, AC_REPORT_TASK_BUILD_CAPACITY) {}

    bool lock(uint32_t timeout_ms = 10) const {
#ifdef ARDUINO
        return mutex &&
               xSemaphoreTake(mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
        (void)timeout_ms;
        return true;
#endif
    }

    void unlock() const {
#ifdef ARDUINO
        if (mutex) xSemaphoreGive(mutex);
#endif
    }

    void wake() const {
#ifdef ARDUINO
        if (task) xTaskNotifyGive(task);
#endif
    }

    OperationAdmission enqueue(ReportTaskCommand command) {
        if (!lock()) return OperationAdmission::Busy;

        if (command.kind == ReportTaskCommandKind::Rebuild && rebuild.active) {
            unlock();
            return OperationAdmission::Busy;
        }
        if (command.kind == ReportTaskCommandKind::Rebuild &&
            (!published || !published->catalog)) {
            unlock();
            return OperationAdmission::Rejected;
        }

        for (size_t i = 0; i < command_count; ++i) {
            ReportTaskCommand &queued = commands[i];
            if (queued.kind != command.kind) continue;
            if (queued.sleep_day == command.sleep_day) {
                queued.force_rebuild =
                    queued.force_rebuild || command.force_rebuild;
                if (report_request_priority_higher(
                        command.priority, queued.priority)) {
                    queued.priority = command.priority;
                }
                queued.generation = command.generation;
                unlock();
                wake();
                return OperationAdmission::Accepted;
            }
        }

        if (command_count >= AC_REPORT_TASK_COMMAND_CAPACITY) {
            ++command_drops;
            unlock();
            return OperationAdmission::Busy;
        }

        if (command.kind == ReportTaskCommandKind::Rebuild) {
            rebuild = {};
            rebuild.generation = command.generation;
            rebuild.active = true;
            rebuild.first_day = command.first_day;
            rebuild.last_day = command.last_day;
        }

        commands[command_count++] = command;
        unlock();
        wake();
        return OperationAdmission::Accepted;
    }

    bool pop(ReportTaskCommand &command) {
        if (!lock()) return false;
        if (command_count == 0) {
            unlock();
            return false;
        }

        size_t selected = SIZE_MAX;
        for (size_t i = 0; i < command_count; ++i) {
            if (commands[i].priority == ReportRequestPriority::Foreground) {
                selected = i;
                break;
            }
        }
        if (selected == SIZE_MAX) selected = 0;

        command = commands[selected];
        for (size_t i = selected + 1; i < command_count; ++i) {
            commands[i - 1] = commands[i];
        }
        commands[--command_count] = {};
        unlock();
        return true;
    }

    void publish_activity(const ActivitySnapshot &next) {
        if (!lock()) return;

        pending_activity = next;
        activity_pending = true;
        unlock();
        wake();
    }

    void publish_capture_session(const EdfSessionMetadata &metadata) {
        if (!lock()) return;

        const bool changed =
            pending_capture.raw_segment_start_ms != metadata.raw_segment_start_ms ||
            pending_capture.capture_session_id != metadata.capture_session_id;
        if (changed) {
            pending_capture = metadata;
            capture_pending = true;
        }
        unlock();
        if (changed) wake();
    }

    bool apply_capture_session() {
        if (!lock()) return false;
        if (!capture_pending) {
            unlock();
            return false;
        }

        capture_session = pending_capture;
        capture_pending = false;
        unlock();

        (void)engine.cancel_background();
        capture_build = {};
        capture_published_end_ms = 0;
        capture_attempt_end_ms = 0;
        capture_retry_at_ms = 0;
        capture_checked_revision = 0;
        capture_available_end_ms = 0;
        capture_progress.reset();
        capture_attempt_progress.reset();
        return true;
    }

    bool capture_storage_ready() const {
        if (!storage_status || activity.ota_install_active ||
            activity.export_work_claimed) return false;

        const StorageWorkloadSnapshot workload =
            storage_status->workload_snapshot();
        return workload.valid && workload.available && !workload.busy &&
               workload.edf_queued == 0 && !workload.maintenance_active;
    }

    bool schedule_capture(uint32_t now_ms) {
        if (!activity.therapy_active || !capture_session.raw_segment_start_ms ||
            capture_build.valid() || catalog_load_pending ||
            store_purpose != CatalogStorePurpose::None ||
            engine.status().state != ReportEngineState::Idle ||
            engine.status().queued != 0 || !capture_storage_ready() ||
            !deadline_due(now_ms, capture_retry_at_ms)) return false;

        const auto progress = storage_status->edf_progress_snapshot();
        if (!progress) return false;

        const bool progress_changed = capture_checked_revision != progress->revision;
        if (progress_changed) {
            capture_available_end_ms = NightCatalogCapture::closed_end(
                capture_session, *progress);
            capture_checked_revision = progress->revision;
        }
        const int64_t end_ms = capture_available_end_ms;
        if (end_ms <= capture_session.canonical_segment_start_ms ||
            (end_ms <= capture_published_end_ms && !progress_changed)) return false;

        const auto next = NightCatalogCapture::build(
            catalog, capture_session, *progress, end_ms);
        if (!next) {
            capture_checked_revision = 0;
            capture_retry_at_ms = now_ms + CATALOG_RETRY_MAX_MS;
            return false;
        }

        const auto *night = next->find(capture_session.canonical_sleep_day);
        if (!night) return false;

        bool rewritten = false;
        if (capture_progress) {
            for (size_t i = 0; i < AC_EDF_STORAGE_PROGRESS_FILE_COUNT; ++i) {
                const auto &before = capture_progress->files[i];
                const auto &after = progress->files[i];
                rewritten = rewritten ||
                    (before.request_id == after.request_id &&
                     strcmp(before.path, after.path) == 0 &&
                     before.rewrite_revision != after.rewrite_revision);
            }
        }

        const auto *previous = catalog ? catalog->find(night->sleep_day) : nullptr;
        if (!rewritten && end_ms <= capture_published_end_ms && previous &&
            previous->source_revision == night->source_revision) return false;

        if (!rewritten && end_ms <= capture_published_end_ms && previous) {
            // Later annotation records may lie beyond this quarter. Only a
            // numeric source catching up warrants republishing the same window.
            size_t next_count = 0;
            size_t previous_count = 0;
            const auto *next_files = next->files(*night, next_count);
            const auto *previous_files = catalog->files(*previous, previous_count);
            bool numeric_changed = false;
            for (size_t i = 0; i < next_count && !numeric_changed; ++i) {
                if (next_files[i].signal_layout_count == 0) continue;

                const auto &file = next_files[i];
                size_t j = 0;
                while (j < previous_count && strcmp(next->path(file),
                       catalog->path(previous_files[j])) != 0) ++j;
                numeric_changed = j == previous_count ||
                    file.complete_records != previous_files[j].complete_records;
            }
            if (!numeric_changed) return false;
        }

        const ReportArtifactKey key = ReportArtifactKey::result(
            night->sleep_day, night->source_revision);
        const uint32_t generation = next_catalog_generation();
        accept_catalog(next, generation, false);
        pending_catalog_save = next;
        pending_catalog_save_generation = catalog_generation;
        const auto request = engine.request(
            key, ReportRequestPriority::Reconcile, generation, rewritten);
        if (request.status == ReportRequestEnqueueStatus::Full ||
            request.status == ReportRequestEnqueueStatus::Invalid) {
            capture_checked_revision = 0;
            capture_retry_at_ms = now_ms + CATALOG_RETRY_MAX_MS;
            return false;
        }

        capture_build = key;
        capture_attempt_end_ms = end_ms;
        capture_attempt_progress = progress;
        return true;
    }

    OperationAdmission publish_session_ended(
        uint32_t sessions_ended,
        const NightCatalogRefreshTarget &target) {
        if (sessions_ended == 0) return OperationAdmission::Accepted;
        if (!lock()) return OperationAdmission::Busy;

        if (last_sessions_ended != 0 &&
            static_cast<int32_t>(sessions_ended - last_sessions_ended) <= 0) {
            unlock();
            return OperationAdmission::Accepted;
        }

        const bool count_gap =
            (last_sessions_ended == 0 && sessions_ended != 1) ||
            (last_sessions_ended != 0 &&
             sessions_ended != last_sessions_ended + 1);
        if (pending_session_ended.pending) {
            pending_session_ended.full_reconcile =
                pending_session_ended.full_reconcile || count_gap ||
                !target.valid() ||
                !same_refresh_target(pending_session_ended.target, target);
            if (!pending_session_ended.full_reconcile) {
                pending_session_ended.target = target;
            }
        } else {
            pending_session_ended.pending = true;
            pending_session_ended.full_reconcile = count_gap ||
                !target.valid();
            pending_session_ended.target =
                pending_session_ended.full_reconcile
                    ? NightCatalogRefreshTarget{} : target;
        }
        last_sessions_ended = sessions_ended;
        control.background_active = true;
        control.post_therapy_settle_pending = true;
        unlock();
        wake();
        return OperationAdmission::Accepted;
    }

    OperationAdmission publish_timezone_change(uint32_t revision,
                                                bool offset_valid,
                                                int32_t offset_minutes) {
        if (!lock()) return OperationAdmission::Busy;

        const bool changed = timezone_seen &&
            revision != timezone_revision;
        timezone_seen = true;
        timezone_revision = revision;
        timezone_offset_valid = offset_valid;
        timezone_offset_minutes = offset_minutes;
        if (!changed) {
            unlock();
            return OperationAdmission::Accepted;
        }

        pending_timezone_change = true;
        control.background_active = true;
        unlock();
        wake();
        return OperationAdmission::Accepted;
    }

    std::shared_ptr<const ReportPublishedState> published_state() const {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        return std::atomic_load_explicit(
            &published, std::memory_order_acquire);
#pragma GCC diagnostic pop
    }

    bool publish_state() {
        std::shared_ptr<const ReportPublishedState> next =
            std::make_shared<ReportPublishedState>(ReportPublishedState{
                catalog, store_catalog, display_summary});
        if (!next) return false;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::atomic_store_explicit(
            &published, std::move(next), std::memory_order_release);
#pragma GCC diagnostic pop
        return true;
    }

    void accept_store_catalog(
        std::shared_ptr<const ReportSignalStoreCatalog> next) {
        if (!next) return;

        store_catalog = std::move(next);
        engine.publish_store_catalog(store_catalog);
        if (!publish_state()) ++command_failures;
    }

    void accept_catalog(std::shared_ptr<const NightCatalog> next,
                        uint32_t generation,
                        bool discover_stored_nights = true) {
        if (!next || generation == 0) return;

        observe_catalog_generation(generation);
        catalog_generation = monotonic_generation(
            generation, catalog_generation);
        catalog = std::move(next);
        display_summary = build_display_report_summary(*catalog);
        engine.publish_catalog(catalog);

        if (!summary_acquisition.snapshot()) {
            summary_acquisition.seed(
                NightCatalogSummarySnapshot::from_catalog(*catalog));
        }

        if (store_catalog) {
            std::shared_ptr<const ReportSignalStoreCatalog> reconciled =
                ReportSignalStoreCatalogBuilder::reconcile(
                    *store_catalog, *catalog);
            if (reconciled) {
                store_catalog = std::move(reconciled);
            } else {
                ++command_failures;
            }
        } else {
            store_catalog = ReportSignalStoreCatalogBuilder::build(
                nullptr, 0);
        }
        engine.publish_store_catalog(store_catalog);

        store_catalog_loader.cancel();
        store_catalog_loader.reset();
        store_catalog_load_pending = discover_stored_nights;
        store_catalog_load_generation = catalog_generation;
        store_catalog_load_retry_at_ms = 0;
        store_catalog_load_retry_attempt = 0;

        clear_failures();
        reset_background_pass();
        if (!publish_state()) ++command_failures;
    }

    void clear_failures() {
        if (!lock(20)) return;
        for (ReportNightFailureEntry &failure : failures) failure = {};
        failure_cursor = 0;
        unlock();
    }

    void remember_failure(const ReportEngineCompletion &completion,
                          uint32_t now_ms) {
        if (!completion.valid() || completion.error[0] == '\0' ||
            !lock(20)) {
            return;
        }

        ReportNightFailureEntry *entry = nullptr;
        for (ReportNightFailureEntry &candidate : failures) {
            if (candidate.valid() &&
                candidate.sleep_day ==
                    completion.request.artifact.sleep_day) {
                entry = &candidate;
                break;
            }
        }
        if (!entry) {
            entry = &failures[failure_cursor++ %
                              AC_REPORT_TASK_BUILD_CAPACITY];
        }

        entry->sleep_day = completion.request.artifact.sleep_day;
        entry->source_revision =
            completion.request.artifact.source_revision;
        copy_cstr(entry->error, sizeof(entry->error), completion.error);
        entry->retryable =
            strcmp(completion.error, "report_source_expired") != 0;
        entry->retry_at_ms = entry->retryable
            ? now_ms + MATERIALIZE_RETRY_MS : 0;
        unlock();
    }

    void clear_failure(SleepDayId sleep_day) {
        if (!lock(20)) return;
        for (ReportNightFailureEntry &entry : failures) {
            if (entry.sleep_day == sleep_day) entry = {};
        }
        unlock();
    }

    bool find_failure(SleepDayId sleep_day,
                      ReportNightFailureStatus &out,
                      uint32_t timeout_ms) const {
        out = {};
        if (!sleep_day.valid() || !lock(timeout_ms)) return false;

        const NightCatalogRecord *night =
            catalog ? catalog->find(sleep_day) : nullptr;
        for (const ReportNightFailureEntry &entry : failures) {
            if (!entry.valid() || entry.sleep_day != sleep_day ||
                !night || entry.source_revision != night->source_revision) {
                continue;
            }

            copy_cstr(out.error, sizeof(out.error), entry.error);
            out.retryable = entry.retryable;
            out.retry_after_ms = entry.retryable
                ? deadline_remaining(last_step_ms, entry.retry_at_ms) : 0;
            unlock();
            return true;
        }
        unlock();
        return false;
    }

    bool local_background_work_blocked() const {
        return activity.therapy_active || activity.realtime_stream_active ||
               activity.foreground_report_demand ||
               activity.ota_install_active || activity.export_work_claimed ||
               engine.status().foreground_active;
    }

    void reset_background_pass() {
        idle_cursor = 0;
        idle_retry_at_ms = 0;
        idle_pass_failed = false;
        idle_generation = increment_generation(idle_generation);
    }

    void invalidate_spool_availability() {
        spool_availability_needed = true;
        spool_availability_retry_at_ms = 0;
        spool_availability_terminal_handled = false;
        spool_availability_probe.cancel();
        engine.publish_spool_availability({}, false);
        reset_background_pass();
    }

    bool apply_pending_activity() {
        ActivitySnapshot next;
        if (!lock()) return false;
        if (!activity_pending) {
            unlock();
            return false;
        }

        next = pending_activity;
        activity_pending = false;
        unlock();

        const bool therapy_ended =
            activity.therapy_active && !next.therapy_active;
        const bool became_blocked =
            !background_suspended &&
            (next.therapy_active || next.realtime_stream_active ||
             next.foreground_report_demand || next.ota_install_active ||
             next.export_work_claimed);
        const bool rpc_became_available =
            !activity.as11_rpc_available && next.as11_rpc_available;

        if (capture_build.valid() &&
            (next.ota_install_active || next.export_work_claimed)) {
            (void)engine.cancel_background();
            capture_build = {};
            capture_attempt_progress.reset();
            capture_checked_revision = 0;
        }

        activity = next;
        background_suspended =
            activity.therapy_active || activity.realtime_stream_active ||
            activity.foreground_report_demand ||
            activity.ota_install_active || activity.export_work_claimed;

        if (therapy_ended) invalidate_spool_availability();
        if (rpc_became_available) reset_background_pass();
        if (!became_blocked) return true;

        (void)engine.cancel_background();
        capture_build = {};
        capture_attempt_progress.reset();
        capture_checked_revision = 0;
        summary_acquisition.cancel();
        spool_availability_probe.cancel();
        if (catalog_refresh.active()) {
            if (!pending_refresh.valid()) {
                pending_refresh.generation = refresh_generation;
                pending_refresh.due_ms = 0;
                pending_refresh.current_offset_valid = refresh_offset_valid;
                pending_refresh.current_offset_minutes =
                    refresh_offset_minutes;
                pending_refresh.target = refresh_target;
                pending_refresh.summary_attempted = true;
                pending_refresh.post_therapy = refresh_post_therapy;
            } else {
                pending_refresh.post_therapy =
                    pending_refresh.post_therapy || refresh_post_therapy;
            }
            catalog_refresh.cancel();
            refresh_generation = 0;
            refresh_post_therapy = false;
        }
        if (store_catalog_loader.status().active()) {
            store_catalog_loader.cancel();
            store_catalog_loader.reset();
            store_catalog_load_pending = catalog != nullptr;
            store_catalog_load_retry_at_ms = 0;
        }
        reset_background_pass();
        return true;
    }

    bool catalog_storage_ready() const {
        if (!storage_status) return false;
        const StorageWorkloadSnapshot workload =
            storage_status->workload_snapshot();
        return workload.valid && !workload.busy &&
               workload.edf_queued == 0 && workload.open_file_count == 0;
    }

    void observe_catalog_generation(uint32_t generation) {
        if (generation == 0) return;

        if (catalog_request_generation == 0 ||
            static_cast<int32_t>(generation - catalog_request_generation) > 0) {
            catalog_request_generation = generation;
        }
    }

    uint32_t next_catalog_generation() {
        observe_catalog_generation(catalog_generation);
        observe_catalog_generation(durable_catalog_generation);
        observe_catalog_generation(refresh_generation);
        observe_catalog_generation(pending_refresh.generation);
        catalog_request_generation = increment_generation(
            catalog_request_generation);
        return catalog_request_generation;
    }

    void record_durable_catalog_generation(uint32_t generation) {
        if (generation == 0) return;

        if (durable_catalog_generation == 0 ||
            static_cast<int32_t>(generation - durable_catalog_generation) > 0) {
            durable_catalog_generation = generation;
        }
        observe_catalog_generation(generation);
    }

    void schedule_reconcile(uint32_t due_ms,
                            bool post_therapy,
                            bool reset_post_therapy_deadline = false) {
        if (reset_post_therapy_deadline && post_therapy) {
            reconcile_due_ms = due_ms;
        } else if (!reconcile_pending ||
            earlier_deadline(due_ms, reconcile_due_ms) == due_ms) {
            reconcile_due_ms = due_ms;
        }
        reconcile_pending = true;
        reconcile_post_therapy =
            reconcile_post_therapy || post_therapy;
        reconcile_deadline_initialized = true;
    }

    void schedule_refresh(uint32_t due_ms,
                          bool offset_valid,
                          int32_t offset_minutes,
                          const NightCatalogRefreshTarget &target,
                          bool post_therapy,
                          bool reset_post_therapy_deadline = false) {
        if (pending_refresh.valid()) {
            const bool same_target = same_refresh_target(
                pending_refresh.target, target);
            const bool keep_target = target.valid() && same_target;
            if (!keep_target) {
                pending_refresh.target = {};
                pending_refresh.summary_attempted = false;
            }
            pending_refresh.due_ms =
                reset_post_therapy_deadline && post_therapy
                    ? due_ms
                    : earlier_deadline(pending_refresh.due_ms, due_ms);
            pending_refresh.current_offset_valid = offset_valid;
            pending_refresh.current_offset_minutes = offset_minutes;
            pending_refresh.post_therapy =
                pending_refresh.post_therapy || post_therapy;
            return;
        }

        pending_refresh.generation = next_catalog_generation();
        pending_refresh.due_ms = due_ms;
        pending_refresh.current_offset_valid = offset_valid;
        pending_refresh.current_offset_minutes = offset_minutes;
        pending_refresh.summary_attempted = target.valid() && catalog;
        pending_refresh.post_therapy = post_therapy;
        pending_refresh.target = target;
    }

    bool apply_pending_refresh_inputs(uint32_t now_ms) {
        PendingSessionEnded session;
        bool timezone_changed = false;
        bool offset_valid = false;
        int32_t offset_minutes = 0;
        if (!lock()) return false;

        session = pending_session_ended;
        pending_session_ended = {};
        timezone_changed = pending_timezone_change;
        pending_timezone_change = false;
        offset_valid = timezone_offset_valid;
        offset_minutes = timezone_offset_minutes;
        unlock();

        if (!session.pending && !timezone_changed) return false;

        (void)engine.cancel_background();
        if (session.pending) {
            if (session.full_reconcile) {
                schedule_reconcile(
                    now_ms + CATALOG_SESSION_SETTLE_MS,
                    true,
                    true);
            } else {
                // Historical discovery must not widen the ended-night refresh.
                if (catalog) {
                    if (pending_refresh.valid() &&
                        !pending_refresh.target.valid() &&
                        !pending_refresh.post_therapy) {
                        summary_acquisition.cancel();
                        pending_refresh.clear();
                    }

                    if (refresh_generation != 0 &&
                        !refresh_target.valid() && !refresh_post_therapy) {
                        catalog_refresh.cancel();
                        refresh_generation = 0;
                    }

                    if (!reconcile_post_therapy) {
                        reconcile_due_ms = now_ms + CATALOG_RECONCILE_IDLE_MS;
                    }
                }

                schedule_refresh(
                    now_ms + CATALOG_SESSION_SETTLE_MS,
                    offset_valid,
                    offset_minutes,
                    session.target,
                    true,
                    true);
                schedule_reconcile(
                    now_ms + CATALOG_RECONCILE_IDLE_MS,
                    false);
            }
        }
        if (timezone_changed) {
            schedule_reconcile(now_ms,
                               false);
        }
        return true;
    }

    bool materialize_due_reconcile(uint32_t now_ms) {
        if (!reconcile_pending ||
            !deadline_due(now_ms, reconcile_due_ms) ||
            pending_refresh.valid() || refresh_generation != 0 ||
            (!reconcile_post_therapy &&
             (pending_catalog_save_post_therapy || post_therapy_build.valid())) ||
            catalog_refresh.active() || catalog_load_pending) {
            return false;
        }

        const bool post_therapy = reconcile_post_therapy;
        if (!lock()) return false;
        const bool offset_valid = timezone_offset_valid;
        const int32_t offset_minutes = timezone_offset_minutes;
        unlock();

        reconcile_pending = false;
        reconcile_post_therapy = false;
        schedule_refresh(
            0,
            offset_valid,
            offset_minutes,
            {},
            post_therapy);
        return true;
    }

    bool post_therapy_settle_pending() const {
        return pending_session_ended.pending ||
               (pending_refresh.valid() && pending_refresh.post_therapy) ||
               (reconcile_pending && reconcile_post_therapy) ||
               refresh_post_therapy || pending_catalog_save_post_therapy ||
               post_therapy_build.valid();
    }

    bool startup_idle_allowed(uint32_t now_ms) {
        if (startup_idle_grace_complete ||
            activity.foreground_report_demand) {
            return true;
        }
        if (!deadline_due(now_ms, AC_RUNTIME_STARTUP_IDLE_GRACE_MS)) {
            return false;
        }

        startup_idle_grace_complete = true;
        return true;
    }

    bool start_store_catalog_load(uint32_t now_ms) {
        if (!store_catalog_load_pending || !catalog ||
            post_therapy_build.valid() ||
            store_catalog_loader.status().active() ||
            !deadline_due(now_ms, store_catalog_load_retry_at_ms)) {
            return false;
        }

        const ReportEngineStatus work = engine.status();
        if (work.state != ReportEngineState::Idle || work.queued != 0) {
            return false;
        }

        const OperationAdmission admitted = store_catalog_loader.start(
            catalog, store_catalog_load_generation);
        if (admitted == OperationAdmission::Busy) return false;

        if (admitted == OperationAdmission::Accepted) {
            store_catalog_load_pending = false;
            return true;
        }

        store_catalog_load_retry_at_ms =
            now_ms + retry_delay(store_catalog_load_retry_attempt);
        advance_retry(store_catalog_load_retry_attempt);
        ++command_failures;
        return true;
    }

    bool observe_store_catalog_load(uint32_t now_ms) {
        const ReportSignalStoreCatalogLoadStatus status =
            store_catalog_loader.status();
        if (!status.terminal()) return false;

        if (status.state == ReportSignalStoreCatalogLoadState::Ready) {
            std::shared_ptr<const ReportSignalStoreCatalog> loaded =
                store_catalog_loader.take_completed();
            if (loaded) {
                accept_store_catalog(std::move(loaded));
                store_catalog_load_retry_at_ms = 0;
                store_catalog_load_retry_attempt = 0;
            } else {
                ++command_failures;
            }
        } else if (status.state ==
                   ReportSignalStoreCatalogLoadState::Failed) {
            ++command_failures;
            store_catalog_loader.reset();
            store_catalog_load_pending = catalog != nullptr;
            store_catalog_load_retry_at_ms =
                now_ms + retry_delay(store_catalog_load_retry_attempt);
            advance_retry(store_catalog_load_retry_attempt);
        } else {
            store_catalog_loader.reset();
        }
        return true;
    }

    bool start_spool_probe(uint32_t now_ms) {
        if (!spool_availability_needed || !activity.as11_rpc_available ||
            spool_availability_probe.status().active() ||
            !deadline_due(now_ms, spool_availability_retry_at_ms)) {
            return false;
        }

        const ReportEngineStatus engine_status = engine.status();
        if (engine_status.state != ReportEngineState::Idle ||
            engine_status.queued != 0) {
            return false;
        }

        spool_availability_generation = increment_generation(
            spool_availability_generation);
        const OperationAdmission admitted = spool_availability_probe.request(
            spool_availability_generation);
        if (admitted == OperationAdmission::Busy) return false;
        if (admitted == OperationAdmission::Rejected) {
            spool_availability_retry_at_ms =
                now_ms + SPOOL_AVAILABILITY_RETRY_MS;
            ++command_failures;
            return true;
        }

        spool_availability_terminal_handled = false;
        engine.publish_spool_availability({}, false);
        return true;
    }

    bool observe_spool_probe(uint32_t now_ms) {
        const ReportSpoolAvailabilityProbeStatus status =
            spool_availability_probe.status();
        if (!status.terminal() || spool_availability_terminal_handled) {
            return false;
        }

        spool_availability_terminal_handled = true;
        const bool complete =
            status.state == ReportSpoolAvailabilityProbeState::Ready;
        engine.publish_spool_availability(
            spool_availability_probe.availability(), complete);
        if (complete) {
            spool_availability_needed = false;
            spool_availability_retry_at_ms = 0;
            reset_background_pass();
        } else {
            spool_availability_retry_at_ms =
                now_ms + SPOOL_AVAILABILITY_RETRY_MS;
        }
        return true;
    }

    bool schedule_post_therapy_build() {
        if (!post_therapy_build.valid()) return false;

        const NightCatalogRecord *night =
            catalog->find(post_therapy_build.sleep_day);
        if (!night || !local_source_available(*night)) {
            post_therapy_build = {};
            return true;
        }

        post_therapy_build.source_revision = night->source_revision;
        if (store_catalog &&
            store_catalog->ready(night->sleep_day, night->source_revision)) {
            post_therapy_build = {};
            return true;
        }

        const ReportEngineStatus status = engine.status();
        if (status.state != ReportEngineState::Idle || status.queued != 0) {
            return false;
        }

        const ReportRequestEnqueueResult queued = engine.request(
            post_therapy_build, ReportRequestPriority::Reconcile,
            catalog_generation);
        if (queued.status == ReportRequestEnqueueStatus::Full) return false;
        if (queued.status == ReportRequestEnqueueStatus::Invalid) {
            post_therapy_build = {};
            ++command_failures;
        }
        return true;
    }

    bool schedule_background(uint32_t now_ms) {
        if (!catalog || store_catalog_load_pending ||
            post_therapy_build.valid() ||
            store_catalog_loader.status().active() ||
            local_background_work_blocked()) {
            return false;
        }

        if (idle_cursor >= catalog->size()) {
            if (!idle_pass_failed ||
                !deadline_due(now_ms, idle_retry_at_ms)) {
                return false;
            }
            reset_background_pass();
            return true;
        }

        const ReportEngineStatus status = engine.status();
        if (status.state != ReportEngineState::Idle || status.queued != 0) {
            return false;
        }

        const NightCatalogRecord *night = catalog->record(idle_cursor);
        if (!night || !night->sleep_day.valid() ||
            !night->source_revision.valid()) {
            ++idle_cursor;
            ++command_failures;
            return true;
        }
        if (store_catalog &&
            store_catalog->ready(night->sleep_day, night->source_revision)) {
            ++idle_cursor;
            return true;
        }
        if (spool_availability_needed &&
            !local_source_available(*night)) {
            return false;
        }
        if (!activity.as11_rpc_available && !local_source_available(*night)) {
            ++idle_cursor;
            return true;
        }

        const ReportRequestPriority priority = idle_cursor == 0
            ? ReportRequestPriority::Reconcile
            : ReportRequestPriority::Idle;
        const ReportRequestEnqueueResult queued = engine.request(
            ReportArtifactKey::result(
                night->sleep_day, night->source_revision),
            priority,
            idle_generation);
        if (queued.status == ReportRequestEnqueueStatus::Full) return false;
        if (queued.status == ReportRequestEnqueueStatus::Invalid) {
            ++idle_cursor;
            ++command_failures;
        }
        return true;
    }

    bool handle_fallback_replacement() {
        if (!engine.catalog_update_required()) return false;

        const std::shared_ptr<const LargeByteBuffer> replacement =
            engine.fallback_replacement();
        const ReportEngineStatus status = engine.status();
        char path[AC_STORAGE_PATH_MAX] = {};
        std::shared_ptr<const NightCatalog> updated;
        const char *error = nullptr;
        if (!catalog) {
            error = "fallback_catalog_missing";
        } else if (!replacement) {
            error = "fallback_replacement_missing";
        } else if (!report_fallback_artifact_path(
                       status.active_request.artifact.sleep_day,
                       path,
                       sizeof(path))) {
            error = "fallback_replacement_path_invalid";
        } else {
            updated = NightCatalogBuilder::replace_fallback(
                *catalog, path, replacement);
            if (!updated) error = "fallback_catalog_replace_failed";
        }

        if (!updated) {
            engine.catalog_update_failed(error);
            ++command_failures;
            return true;
        }

        const uint32_t generation = increment_generation(catalog_generation);
        accept_catalog(std::move(updated), generation);
        pending_catalog_save = catalog;
        pending_catalog_save_generation = catalog_generation;
        catalog_store_retry_at_ms = 0;
        catalog_store_retry_attempt = 0;
        return true;
    }

    bool advance_rebuild() {
        if (!rebuild_catalog || !lock()) return false;
        if (!rebuild.active || rebuild_ticket.valid()) {
            unlock();
            return false;
        }

        while (rebuild_cursor < rebuild_catalog->size()) {
            const NightCatalogRecord *night =
                rebuild_catalog->record(rebuild_cursor);
            if (night->sleep_day < rebuild.first_day ||
                rebuild.last_day < night->sleep_day) {
                ++rebuild_cursor;
                continue;
            }

            // Keep the requested days fixed, but use the latest source revision.
            const NightCatalogRecord *current = catalog
                ? catalog->find(night->sleep_day) : nullptr;
            const ReportArtifactKey key = ReportArtifactKey::result(
                night->sleep_day,
                current ? current->source_revision : night->source_revision);
            const ReportRequestEnqueueResult queued = engine.request(
                key, ReportRequestPriority::Foreground,
                rebuild.generation, true);
            if (queued.status == ReportRequestEnqueueStatus::Full) {
                unlock();
                return false;
            }

            ++rebuild_cursor;
            if (queued.status == ReportRequestEnqueueStatus::Invalid) {
                ++rebuild.completed;
                ++rebuild.failed;
                rebuild.last_completion = {};
                rebuild.last_completion.request.artifact = key;
                copy_cstr(rebuild.last_completion.error,
                          sizeof(rebuild.last_completion.error),
                          "report_request_rejected");
                unlock();
                return true;
            }

            rebuild_ticket = queued.ticket;
            unlock();
#ifdef ARDUINO
            char day[9] = {};
            key.sleep_day.format_yyyymmdd(day, sizeof(day));
            Log::logf(CAT_REPORT, LOG_INFO, "rebuild started night=%s", day);
#endif
            clear_failure(key.sleep_day);
            return true;
        }

        rebuild.active = false;
        rebuild_catalog.reset();
        unlock();
        return true;
    }

    bool observe_engine(uint32_t now_ms) {
        bool worked = false;
        ReportSignalStoreCatalogInput published_input =
            engine.take_published();
        if (published_input.metadata) {
            std::shared_ptr<const ReportSignalStoreCatalog> next =
                store_catalog
                    ? ReportSignalStoreCatalogBuilder::upsert(
                          *store_catalog, published_input)
                    : ReportSignalStoreCatalogBuilder::build(
                          &published_input, 1);
            if (next) {
                accept_store_catalog(std::move(next));
            } else {
                ++command_failures;
            }
            worked = true;
        }

        const ReportEngineCompletion completion =
            engine.status().last_completion;
        if (!completion.valid() ||
            completion.request.ticket == observed_engine_completion) {
            return worked;
        }
        observed_engine_completion = completion.request.ticket;
        if (rebuild_catalog) {
            if (!lock()) {
                observed_engine_completion = {};
                return worked;
            }
            if (rebuild.active && completion.request.ticket == rebuild_ticket) {
                rebuild.last_completion = completion;
                ++rebuild.completed;
                if (completion.outcome.disposition !=
                    OperationDisposition::Succeeded) {
                    ++rebuild.failed;
                }
                rebuild_ticket = {};
#ifdef ARDUINO
                char day[9] = {};
                completion.request.artifact.sleep_day.format_yyyymmdd(
                    day, sizeof(day));
                if (completion.outcome.disposition ==
                    OperationDisposition::Succeeded) {
                    Log::logf(CAT_REPORT, LOG_INFO,
                              "rebuild complete night=%s", day);
                } else {
                    Log::logf(CAT_REPORT, LOG_WARN,
                              "rebuild failed night=%s error=%s", day,
                              completion.error[0] ? completion.error :
                                  "report_build_failed");
                }
#endif
            }
            unlock();
        }

        const SleepDayId completed_day =
            completion.request.artifact.sleep_day;
        const bool succeeded =
            completion.outcome.disposition ==
            OperationDisposition::Succeeded;

        if (completion.request.artifact == capture_build) {
            if (succeeded) {
                capture_published_end_ms = capture_attempt_end_ms;
                capture_progress = std::move(capture_attempt_progress);
            }
            capture_retry_at_ms = succeeded ? 0 : now_ms + CATALOG_RETRY_MAX_MS;
            if (!succeeded) capture_checked_revision = 0;
            capture_build = {};
        }

        if (completion.request.artifact == post_therapy_build &&
            completion.outcome.disposition != OperationDisposition::Cancelled) {
            post_therapy_build = {};
#ifdef ARDUINO
            char day[9] = {};
            completed_day.format_yyyymmdd(day, sizeof(day));
            Log::logf(CAT_REPORT, succeeded ? LOG_INFO : LOG_WARN,
                      "post-therapy report %s night=%s error=%s",
                      succeeded ? "ready" : "failed", day,
                      completion.error[0] ? completion.error : "--");
#endif
        }

        if (succeeded) {
            clear_failure(completed_day);
        } else if (completion.outcome.disposition !=
                   OperationDisposition::Cancelled) {
            remember_failure(completion, now_ms);
            ++command_failures;
        }

        if (completion.request.priority !=
                ReportRequestPriority::Foreground &&
            completion.outcome.disposition !=
                OperationDisposition::Cancelled) {
            const NightCatalogRecord *current =
                catalog && idle_cursor < catalog->size()
                    ? catalog->record(idle_cursor) : nullptr;
            if (current && current->sleep_day == completed_day) {
                ++idle_cursor;
            }
            if (!succeeded) {
                idle_pass_failed = true;
                idle_retry_at_ms = now_ms + MATERIALIZE_RETRY_MS;
            }
        }
        return true;
    }

    ReportTaskWaitReason activity_wait_reason() const {
        if (activity.therapy_active) return ReportTaskWaitReason::Therapy;
        if (activity.realtime_stream_active) {
            return ReportTaskWaitReason::RealtimeStream;
        }
        if (activity.foreground_report_demand) {
            return ReportTaskWaitReason::ForegroundRequest;
        }
        if (activity.ota_install_active) return ReportTaskWaitReason::Ota;
        if (activity.export_work_claimed) return ReportTaskWaitReason::Export;
        if (!activity.as11_rpc_available) {
            return ReportTaskWaitReason::As11Unavailable;
        }
        return ReportTaskWaitReason::None;
    }

    ReportTaskOperationalSnapshot operational() const {
        ReportTaskOperationalSnapshot out;
        out.catalog_nights = catalog ? catalog->size() : 0;
        out.materialized_nights = store_catalog ? store_catalog->size() : 0;
        if (!initialized) return out;

        out.condition = ReportTaskCondition::Working;
        if (catalog_store.active()) {
            out.operation = store_purpose == CatalogStorePurpose::Save
                ? ReportTaskOperation::SavingCatalog
                : ReportTaskOperation::LoadingCatalog;
            return out;
        }
        if (store_catalog_loader.status().active()) {
            out.operation = ReportTaskOperation::LoadingStoreCatalog;
            return out;
        }
        if (summary_acquisition.active() || catalog_refresh.active()) {
            out.operation = ReportTaskOperation::RefreshingCatalog;
            out.sleep_day = refresh_target.sleep_day;
            return out;
        }
        if (spool_availability_probe.status().active()) {
            out.operation = ReportTaskOperation::CheckingSpools;
            return out;
        }

        const ReportEngineStatus engine_status = engine.status();
        out.sleep_day = engine_status.active_request.artifact.sleep_day;
        switch (engine_status.state) {
            case ReportEngineState::AcquiringFallback:
            case ReportEngineState::Executing:
                out.operation = ReportTaskOperation::Building;
                return out;
            case ReportEngineState::Publishing:
                out.operation = ReportTaskOperation::Publishing;
                return out;
            case ReportEngineState::Queued:
                out.condition = ReportTaskCondition::Waiting;
                out.wait_reason = ReportTaskWaitReason::Queue;
                return out;
            case ReportEngineState::WaitingForCatalog:
                out.condition = ReportTaskCondition::Waiting;
                out.wait_reason = ReportTaskWaitReason::Catalog;
                return out;
            case ReportEngineState::Idle:
                break;
        }

        out.condition = ReportTaskCondition::Waiting;
        if (command_count > 0) {
            out.wait_reason = ReportTaskWaitReason::Queue;
            return out;
        }

        out.wait_reason = activity_wait_reason();
        if (out.wait_reason != ReportTaskWaitReason::None) return out;
        if (!startup_idle_grace_complete) {
            out.wait_reason = ReportTaskWaitReason::Startup;
            return out;
        }
        if (!catalog) {
            out.condition = ReportTaskCondition::Failed;
            copy_cstr(out.error,
                      sizeof(out.error),
                      catalog_store.status().error[0]
                          ? catalog_store.status().error
                          : "report_catalog_unavailable");
            return out;
        }
        if (idle_pass_failed && idle_retry_at_ms != 0) {
            out.wait_reason = ReportTaskWaitReason::Retry;
            out.retry_in_ms = deadline_remaining(
                last_step_ms, idle_retry_at_ms);
            copy_cstr(out.error,
                      sizeof(out.error),
                      engine_status.last_completion.error);
            return out;
        }

        out.condition = ReportTaskCondition::Complete;
        out.wait_reason = ReportTaskWaitReason::None;
        return out;
    }

    void publish_status() {
        ReportTaskControlSnapshot next;
        next.initialized = initialized;
        next.task_started = task_started;
        next.catalog_generation = catalog_generation;
        next.durable_catalog_generation = durable_catalog_generation;
        next.catalog_refresh_state = catalog_refresh.status().state;
        next.catalog_refresh_generation =
            catalog_refresh.status().generation;
        next.catalog_refresh_retryable =
            catalog_refresh.status().retryable;

        const ReportEngineStatus engine_status = engine.status();
        next.foreground_active = engine_status.foreground_active;
        next.background_active =
            rebuild_catalog != nullptr || engine_status.queued > 0 ||
            engine_status.state != ReportEngineState::Idle ||
            store_catalog_loader.status().active() ||
            catalog_refresh.active() || summary_acquisition.active() ||
            spool_availability_probe.status().active() ||
            pending_catalog_save != nullptr || pending_refresh.valid() ||
            post_therapy_build.valid();

        if (!initialized) {
            next.state = ReportTaskState::Stopped;
        } else if (catalog_load_pending ||
                   store_purpose == CatalogStorePurpose::Load ||
                   store_catalog_loader.status().active()) {
            next.state = ReportTaskState::LoadingCatalog;
        } else if (catalog_refresh.active() || pending_refresh.valid() ||
                   summary_acquisition.active()) {
            next.state = ReportTaskState::RefreshingCatalog;
        } else {
            switch (engine_status.state) {
                case ReportEngineState::AcquiringFallback:
                case ReportEngineState::Executing:
                    next.state = ReportTaskState::Building;
                    break;
                case ReportEngineState::Publishing:
                    next.state = ReportTaskState::Publishing;
                    break;
                case ReportEngineState::Queued:
                case ReportEngineState::WaitingForCatalog:
                    next.state = ReportTaskState::Queued;
                    break;
                case ReportEngineState::Idle:
                    next.state = ReportTaskState::Idle;
                    break;
            }
        }

        if (!lock()) return;
        if (next.state == ReportTaskState::Idle && command_count != 0) {
            next.state = ReportTaskState::Queued;
        }
        const bool pending_inputs = pending_session_ended.pending &&
            !pending_session_ended.full_reconcile;
        next.background_active = next.background_active || pending_inputs;
        if (pending_inputs && next.state == ReportTaskState::Idle) {
            next.state = ReportTaskState::RefreshingCatalog;
        }
        next.post_therapy_settle_pending =
            post_therapy_settle_pending();
        control = next;
        unlock();
    }

    ReportArtifactRequest build_slots[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    ReportEngine engine;
    ReportSummaryAcquisition summary_acquisition;
    ReportSpoolAvailabilityProbe spool_availability_probe;
    NightCatalogRefreshService catalog_refresh;
    NightCatalogStoreService catalog_store;
    ReportSignalStoreCatalogLoadService store_catalog_loader;

    ReportTaskCommand commands[AC_REPORT_TASK_COMMAND_CAPACITY] = {};
    ReportRebuildStatus rebuild;
    std::shared_ptr<const NightCatalog> rebuild_catalog;
    size_t rebuild_cursor = 0;
    OperationTicket rebuild_ticket;
    size_t command_count = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;

    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const ReportSignalStoreCatalog> store_catalog;
    DisplayReportSummary display_summary;
    std::shared_ptr<const ReportPublishedState> published;
    std::shared_ptr<const NightCatalog> pending_catalog_save;
    uint32_t pending_catalog_save_generation = 0;
    bool pending_catalog_save_post_therapy = false;
    uint32_t catalog_store_save_generation = 0;
    ReportArtifactKey post_therapy_build;

    EdfSessionMetadata capture_session;
    EdfSessionMetadata pending_capture;
    bool capture_pending = false;
    ReportArtifactKey capture_build;
    int64_t capture_published_end_ms = 0;
    int64_t capture_attempt_end_ms = 0;
    uint32_t capture_retry_at_ms = 0;
    uint64_t capture_checked_revision = 0;
    int64_t capture_available_end_ms = 0;
    std::shared_ptr<const EdfStorageProgress> capture_progress;
    std::shared_ptr<const EdfStorageProgress> capture_attempt_progress;

    PendingCatalogRefresh pending_refresh;
    PendingSessionEnded pending_session_ended;
    bool pending_timezone_change = false;
    uint32_t last_sessions_ended = 0;
    bool timezone_seen = false;
    uint32_t timezone_revision = 0;
    bool timezone_offset_valid = false;
    int32_t timezone_offset_minutes = 0;
    bool reconcile_pending = false;
    bool reconcile_post_therapy = false;
    uint32_t reconcile_due_ms = 0;
    bool reconcile_deadline_initialized = false;
    uint32_t refresh_generation = 0;
    bool refresh_offset_valid = false;
    int32_t refresh_offset_minutes = 0;
    NightCatalogRefreshTarget refresh_target;
    bool refresh_post_therapy = false;
    uint32_t catalog_refresh_retry_at_ms = 0;
    uint8_t catalog_refresh_retry_attempt = 0;

    CatalogStorePurpose store_purpose = CatalogStorePurpose::None;
    bool catalog_load_pending = true;
    uint32_t catalog_store_retry_at_ms = 0;
    uint8_t catalog_store_retry_attempt = 0;
    uint32_t catalog_generation = 0;
    uint32_t durable_catalog_generation = 0;
    uint32_t catalog_request_generation = 0;

    StorageStatusPort *storage_status = nullptr;

    bool store_catalog_load_pending = false;
    uint32_t store_catalog_load_generation = 0;
    uint32_t store_catalog_load_retry_at_ms = 0;
    uint8_t store_catalog_load_retry_attempt = 0;

    size_t idle_cursor = 0;
    uint32_t idle_generation = 0x80000000u;
    uint32_t idle_retry_at_ms = 0;
    bool idle_pass_failed = false;
    bool startup_idle_grace_complete = false;

    uint32_t spool_availability_generation = 0;
    uint32_t spool_availability_retry_at_ms = 0;
    bool spool_availability_needed = true;
    bool spool_availability_terminal_handled = false;

    ReportNightFailureEntry failures[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    size_t failure_cursor = 0;
    OperationTicket observed_engine_completion;
    uint32_t last_step_ms = 0;

    ActivitySnapshot activity;
    ActivitySnapshot pending_activity;
    bool activity_pending = false;
    bool background_suspended = false;

    bool initialized = false;
    bool task_started = false;
    ReportTaskControlSnapshot control;

#ifdef ARDUINO
    mutable SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
    bool task_stack_external = false;
#endif
};

ReportTask::~ReportTask() {
    if (!runtime_) return;

#ifdef ARDUINO
    if (runtime_->task) {
        if (runtime_->task_stack_external) {
            vTaskDeleteWithCaps(runtime_->task);
        } else {
            vTaskDelete(runtime_->task);
        }
        runtime_->task = nullptr;
    }
    if (runtime_->mutex) {
        vSemaphoreDelete(runtime_->mutex);
        runtime_->mutex = nullptr;
    }
    runtime_->~Runtime();
    Memory::free(runtime_);
#else
    delete runtime_;
#endif
    runtime_ = nullptr;
}

bool ReportTask::begin(StorageReadPort &read_port,
                       StorageAtomicWritePort &write_port,
                       StorageScanPort &scan_port,
                       ReportSpoolPort &spool_port,
                       StorageRangeWritePort &range_write_port,
                       StorageStatusPort &status_port) {
    if (runtime_) return runtime_->initialized;

#ifdef ARDUINO
    void *memory = Memory::alloc_large(sizeof(Runtime), false);
    runtime_ = memory ? new (memory) Runtime() : nullptr;
#else
    runtime_ = new (std::nothrow) Runtime();
#endif
    if (!runtime_) return false;

    runtime_->storage_status = &status_port;

#ifdef ARDUINO
    runtime_->mutex = xSemaphoreCreateMutex();
    if (!runtime_->mutex) {
        runtime_->~Runtime();
        Memory::free(runtime_);
        runtime_ = nullptr;
        return false;
    }
#endif

    runtime_->catalog_refresh.begin(scan_port, read_port);
    runtime_->catalog_store.begin(read_port, write_port);
    runtime_->summary_acquisition.begin(spool_port);
    runtime_->spool_availability_probe.begin(spool_port);
    runtime_->store_catalog_loader.begin(read_port);
    runtime_->engine.begin(read_port, write_port, spool_port, range_write_port);
    runtime_->store_catalog = ReportSignalStoreCatalogBuilder::build(
        nullptr, 0);
    runtime_->engine.publish_store_catalog(runtime_->store_catalog);
    runtime_->initialized = runtime_->store_catalog != nullptr;
    runtime_->publish_state();
    runtime_->publish_status();

#ifdef ARDUINO
    BaseType_t created = pdFAIL;
    if (runtime_->initialized && Memory::psram_available()) {
        created = xTaskCreatePinnedToCoreWithCaps(
            task_entry,
            "ac_report",
            AC_REPORT_TASK_STACK,
            this,
            AC_REPORT_TASK_PRIO,
            &runtime_->task,
            AC_REPORT_TASK_CORE,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        runtime_->task_stack_external =
            created == pdPASS && runtime_->task != nullptr;
    }

    if (runtime_->initialized && !runtime_->task_stack_external) {
        runtime_->task = nullptr;
        created = xTaskCreatePinnedToCore(
            task_entry,
            "ac_report",
            AC_REPORT_TASK_STACK,
            this,
            AC_REPORT_TASK_PRIO,
            &runtime_->task,
            AC_REPORT_TASK_CORE);
    }

    if (!runtime_->initialized || created != pdPASS || !runtime_->task) {
        runtime_->initialized = false;
        runtime_->publish_status();
        if (runtime_->mutex) vSemaphoreDelete(runtime_->mutex);
        runtime_->mutex = nullptr;
        runtime_->~Runtime();
        Memory::free(runtime_);
        runtime_ = nullptr;
        return false;
    }
#endif
    return runtime_->initialized;
}

OperationAdmission ReportTask::request_rebuild(
    SleepDayId first_day, SleepDayId last_day, uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || generation == 0 ||
        !first_day.valid() || !last_day.valid() || last_day < first_day) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::Rebuild;
    command.first_day = first_day;
    command.last_day = last_day;
    command.generation = generation;
    return runtime_->enqueue(command);
}

ReportRebuildStatus ReportTask::rebuild_status() const {
    if (!runtime_ || !runtime_->lock()) return {};
    const ReportRebuildStatus result = runtime_->rebuild;
    runtime_->unlock();
    return result;
}

OperationAdmission ReportTask::request_night(
    SleepDayId sleep_day,
    ReportRequestPriority priority,
    uint32_t generation,
    bool force_rebuild) {
    if (!runtime_ || !runtime_->initialized || !sleep_day.valid() ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.sleep_day = sleep_day;
    command.priority = priority;
    command.force_rebuild = force_rebuild;
    command.generation = generation;
    return runtime_->enqueue(command);
}

OperationAdmission ReportTask::publish_session_ended(
    uint32_t sessions_ended,
    const NightCatalogRefreshTarget &target) {
    if (!runtime_ || !runtime_->initialized) {
        return OperationAdmission::Rejected;
    }
    return runtime_->publish_session_ended(sessions_ended, target);
}

OperationAdmission ReportTask::publish_timezone_change(
    uint32_t revision,
    bool offset_valid,
    int32_t offset_minutes) {
    if (!runtime_ || !runtime_->initialized) {
        return OperationAdmission::Rejected;
    }
    return runtime_->publish_timezone_change(
        revision, offset_valid, offset_minutes);
}

void ReportTask::publish_activity(const ActivitySnapshot &activity) {
    if (runtime_ && runtime_->initialized) {
        runtime_->publish_activity(activity);
    }
}

void ReportTask::publish_capture_session(const EdfSessionMetadata &metadata) {
    if (runtime_) runtime_->publish_capture_session(metadata);
}

ReportTaskControlSnapshot ReportTask::control_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    const ReportTaskControlSnapshot out = runtime_->control;
    runtime_->unlock();
    return out;
}

ReportTaskOperationalSnapshot ReportTask::operational_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    const ReportTaskOperationalSnapshot out = runtime_->operational();
    runtime_->unlock();
    return out;
}

ReportTaskDiagnosticSnapshot ReportTask::diagnostic_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};

    const ReportEngineStatus engine = runtime_->engine.status();
    const ReportFallbackAcquisitionStatus fallback = engine.fallback;
    const NightCatalogRefreshStatus catalog =
        runtime_->catalog_refresh.status();
    const ReportSignalStoreCatalogLoadStatus store =
        runtime_->store_catalog_loader.status();

    ReportTaskDiagnosticSnapshot out;
    out.task_started = runtime_->task_started;
    out.state = runtime_->control.state;
    out.commands_queued = runtime_->command_count;
    out.catalog_nights = runtime_->catalog ? runtime_->catalog->size() : 0;
    out.materialized_nights = runtime_->store_catalog
        ? runtime_->store_catalog->size() : 0;
    out.command_drops = runtime_->command_drops;
    out.command_failures = runtime_->command_failures;
    out.catalog_generation = runtime_->catalog_generation;
    out.durable_catalog_generation =
        runtime_->durable_catalog_generation;
    out.foreground_active = engine.foreground_active;
    out.background_active = runtime_->control.background_active;
    out.background_suspended = runtime_->background_suspended;

    out.engine_state = engine.state;
    out.engine_queued = engine.queued;
    out.engine_sleep_day = engine.active_request.artifact.sleep_day;
    out.executor_state = engine.executor.state;
    out.executor_operation_index = engine.executor.operation_index;
    out.executor_operation_count = engine.executor.operation_count;
    out.executor_record_index = engine.executor.record_index;
    out.executor_record_count = engine.executor.record_count;
    copy_cstr(out.engine_error,
              sizeof(out.engine_error),
              engine.last_completion.error);

    out.fallback_state = fallback.state;
    out.fallback_source = fallback.source;
    out.fallback_sources_total = fallback.sources_total;
    out.fallback_sources_completed = fallback.sources_completed;
    out.fallback_sections_added = fallback.sections_added;
    out.fallback_unavailable_added = fallback.unavailable_added;
    copy_cstr(out.fallback_error,
              sizeof(out.fallback_error),
              fallback.error);

    out.catalog_state = catalog.state;
    out.catalog_files_seen = catalog.files_seen;
    out.catalog_files_indexed = catalog.files_indexed;
    out.catalog_sessions = catalog.sessions;
    copy_cstr(out.catalog_error,
              sizeof(out.catalog_error),
              catalog.error);

    out.store_catalog_state = store.state;
    out.store_catalog_checked = store.nights_checked;
    out.store_catalog_loaded = store.nights_loaded;
    out.store_catalog_skipped = store.nights_skipped;
    copy_cstr(out.store_catalog_error,
              sizeof(out.store_catalog_error),
              store.error);

    runtime_->unlock();
    return out;
}

ReportEngineCompletion ReportTask::last_completion() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    const ReportEngineCompletion out = runtime_->engine.status().last_completion;
    runtime_->unlock();
    return out;
}

std::shared_ptr<const NightCatalog> ReportTask::catalog_snapshot() const {
    if (!runtime_) return {};
    const std::shared_ptr<const ReportPublishedState> state =
        runtime_->published_state();
    return state ? state->catalog : nullptr;
}

std::shared_ptr<const ReportSignalStoreCatalog>
ReportTask::store_catalog_snapshot() const {
    if (!runtime_) return {};
    const std::shared_ptr<const ReportPublishedState> state =
        runtime_->published_state();
    return state ? state->store_catalog : nullptr;
}

DisplayReportSummary ReportTask::display_summary_snapshot() const {
    if (!runtime_) return {};
    const std::shared_ptr<const ReportPublishedState> state =
        runtime_->published_state();
    return state ? state->display_summary : DisplayReportSummary{};
}

ReportNightQuery ReportTask::query_night(SleepDayId sleep_day) const {
    ReportNightQuery out;
    out.sleep_day = sleep_day;
    if (!runtime_ || !runtime_->initialized || !sleep_day.valid()) return out;

    const std::shared_ptr<const ReportPublishedState> state =
        runtime_->published_state();
    if (!state || !state->catalog) {
        out.state = ReportStoreQueryState::CatalogPending;
        return out;
    }

    const NightCatalogRecord *source = state->catalog->find(sleep_day);
    if (!source) {
        out.state = ReportStoreQueryState::NightMissing;
        return out;
    }
    out.source_revision = source->source_revision;

    const ReportSignalStoreCatalogRecord *stored = state->store_catalog
        ? state->store_catalog->find(sleep_day) : nullptr;
    if (!stored || stored->source_revision != source->source_revision ||
        !stored->metadata) {
        out.state = ReportStoreQueryState::StorePending;
        return out;
    }

    out.state = ReportStoreQueryState::Ready;
    out.generation = stored->generation;
    out.metadata = stored->metadata;
    return out;
}

ReportSignalRangeQuery ReportTask::query_signal(
    SleepDayId sleep_day,
    size_t metadata_track_index,
    int64_t first_block_start_ms,
    size_t block_count,
    ReportSignalStoreLevel level) const {
    ReportSignalRangeQuery out;
    out.level = level;
    out.first_block_start_ms = first_block_start_ms;
    out.block_count = block_count;

    const ReportNightQuery night = query_night(sleep_day);
    out.state = night.state;
    if (night.state != ReportStoreQueryState::Ready || !night.metadata) {
        return out;
    }

    ReportSignalStoreNightView view;
    if (!ReportSignalStoreNightCodec::decode(
            night.metadata->data(), night.metadata->size(), view) ||
        !view.track(metadata_track_index, out.track)) {
        out.state = ReportStoreQueryState::TrackMissing;
        return out;
    }

    size_t file_size = 0;
    if (block_count == 0 ||
        !ReportSignalStoreFileCodec::plane_range(
            out.track,
            first_block_start_ms,
            block_count,
            level,
            out.range) ||
        !ReportSignalStoreFileCodec::file_size(out.track, level, file_size) ||
        !report_signal_store_signal_path(
            out.track, level, out.path, sizeof(out.path))) {
        out.state = ReportStoreQueryState::InvalidRange;
        return out;
    }

    out.file_size = file_size;
    out.state = ReportStoreQueryState::Ready;
    return out;
}

ReportEventFileQuery ReportTask::query_events(SleepDayId sleep_day) const {
    ReportEventFileQuery out;
    out.sleep_day = sleep_day;
    const ReportNightQuery night = query_night(sleep_day);
    out.state = night.state;
    if (night.state != ReportStoreQueryState::Ready || !night.metadata) {
        return out;
    }

    ReportSignalStoreNightView view;
    if (!ReportSignalStoreNightCodec::decode(
            night.metadata->data(), night.metadata->size(), view)) {
        out.state = ReportStoreQueryState::InvalidRange;
        return out;
    }

    const int64_t first_block = align_block_start(view.night.day_start_ms);
    const int64_t last_block = align_block_start(view.night.day_end_ms - 1);
    const int64_t block_count =
        (last_block - first_block) / REPORT_SIGNAL_STORE_BLOCK_MS + 1;
    size_t file_size = 0;
    if (block_count <= 0 || block_count > UINT16_MAX ||
        !ReportSignalStoreEventCodec::file_size(
            static_cast<uint16_t>(block_count),
            view.night.event_count,
            file_size) ||
        !report_signal_store_events_path(
            sleep_day, view.night.generation,
            out.path, sizeof(out.path))) {
        out.state = ReportStoreQueryState::InvalidRange;
        return out;
    }

    out.state = ReportStoreQueryState::Ready;
    out.source_revision = view.night.source_revision;
    out.generation = view.night.generation;
    out.file_size = file_size;
    out.event_count = view.night.event_count;
    return out;
}

bool ReportTask::night_failure(SleepDayId sleep_day,
                               ReportNightFailureStatus &failure,
                               uint32_t lock_timeout_ms) const {
    if (!runtime_) {
        failure = {};
        return false;
    }
    return runtime_->find_failure(sleep_day, failure, lock_timeout_ms);
}

bool ReportTask::step(uint32_t now_ms, size_t record_budget) {
    if (!runtime_ || !runtime_->initialized) return false;
    Runtime &runtime = *runtime_;
    runtime.last_step_ms = now_ms;

    bool worked = runtime.apply_pending_refresh_inputs(now_ms);
    if (!runtime.reconcile_deadline_initialized) {
        runtime.reconcile_pending = true;
        runtime.reconcile_due_ms = now_ms + CATALOG_RECONCILE_IDLE_MS;
        runtime.reconcile_deadline_initialized = true;
        worked = true;
    }
    worked = runtime.apply_pending_activity() || worked;
    worked = runtime.apply_capture_session() || worked;
    const bool startup_allowed = runtime.startup_idle_allowed(now_ms);
    const bool local_blocked = runtime.local_background_work_blocked();

    ReportTaskCommand command;
    if (runtime.pop(command)) {
        if (command.kind == ReportTaskCommandKind::Rebuild) {
            runtime.rebuild_catalog = runtime.catalog;
            runtime.rebuild_cursor = 0;
        } else if (!runtime.catalog) {
            (void)runtime.enqueue(command);
        } else {
            const NightCatalogRecord *night =
                runtime.catalog->find(command.sleep_day);
            if (night) {
                const ReportRequestEnqueueResult queued =
                    runtime.engine.request(
                        ReportArtifactKey::result(
                            night->sleep_day, night->source_revision),
                        command.priority,
                        command.generation,
                        command.force_rebuild);
                if (queued.status == ReportRequestEnqueueStatus::Full) {
                    (void)runtime.enqueue(command);
                } else if (queued.status ==
                           ReportRequestEnqueueStatus::Invalid) {
                    ++runtime.command_failures;
                } else {
                    runtime.clear_failure(command.sleep_day);
                }
            }
        }
        worked = true;
    }

    if (runtime.catalog_store.active()) {
        worked = runtime.catalog_store.poll() || worked;
    }
    if (runtime.store_purpose != CatalogStorePurpose::None &&
        !runtime.catalog_store.active()) {
        const NightCatalogStoreStatus status =
            runtime.catalog_store.status();
        if (status.state == NightCatalogStoreState::Ready ||
            status.state == NightCatalogStoreState::Error) {
            const CatalogStorePurpose completed = runtime.store_purpose;
            runtime.store_purpose = CatalogStorePurpose::None;
            if (completed == CatalogStorePurpose::Load) {
                runtime.catalog_load_pending = false;
                if (status.state == NightCatalogStoreState::Ready) {
                    runtime.record_durable_catalog_generation(status.generation);
                    publish_catalog(runtime.catalog_store.snapshot(),
                                    status.generation);
                } else if (!runtime.catalog) {
                    runtime.schedule_reconcile(now_ms, false);
                }
            } else if (status.state == NightCatalogStoreState::Ready) {
                runtime.record_durable_catalog_generation(status.generation);
                const bool saved_latest =
                    runtime.pending_catalog_save &&
                    runtime.pending_catalog_save_generation ==
                        runtime.catalog_store_save_generation &&
                    status.generation == runtime.catalog_store_save_generation;
                if (saved_latest) {
                    runtime.pending_catalog_save.reset();
                    runtime.pending_catalog_save_generation = 0;
                    runtime.pending_catalog_save_post_therapy = false;
                }
                runtime.catalog_store_retry_at_ms = 0;
                runtime.catalog_store_retry_attempt = 0;
            } else {
                runtime.catalog_store_retry_at_ms =
                    now_ms + retry_delay(runtime.catalog_store_retry_attempt);
                advance_retry(runtime.catalog_store_retry_attempt);
            }
            worked = true;
        }
    }

    if (runtime.catalog_load_pending &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        deadline_due(now_ms, runtime.catalog_store_retry_at_ms)) {
        const OperationAdmission admitted =
            runtime.catalog_store.request_load(CATALOG_STORE_GENERATION);
        if (admitted == OperationAdmission::Accepted) {
            runtime.store_purpose = CatalogStorePurpose::Load;
            runtime.catalog_store_retry_at_ms = 0;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.catalog_load_pending = false;
            if (!runtime.catalog) runtime.schedule_reconcile(now_ms, false);
            ++runtime.command_failures;
        }
        worked = true;
    }

    const ReportEngineStatus engine_work = runtime.engine.status();
    if (runtime.store_catalog_loader.status().active() &&
        (engine_work.state != ReportEngineState::Idle || engine_work.queued != 0 ||
         runtime.post_therapy_build.valid())) {
        // Requested-night discovery reads only its night. Resume inventory later,
        // so a late full-catalog snapshot cannot overwrite its publication.
        runtime.store_catalog_loader.cancel();
        runtime.store_catalog_loader.reset();
        runtime.store_catalog_load_pending = true;
    }

    if (runtime.store_catalog_loader.status().active()) {
        worked = runtime.store_catalog_loader.poll() || worked;
    }
    worked = runtime.observe_store_catalog_load(now_ms) || worked;
    if (!runtime.activity.ota_install_active && !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load) {
        worked = runtime.start_store_catalog_load(now_ms) || worked;
    }

    worked = runtime.materialize_due_reconcile(now_ms) || worked;

    if (runtime.summary_acquisition.active()) {
        worked = runtime.summary_acquisition.poll() || worked;
    }
    if (runtime.pending_refresh.valid() &&
        !runtime.pending_refresh.summary_attempted &&
        !runtime.summary_acquisition.active() &&
        !local_blocked && startup_allowed &&
        runtime.catalog_storage_ready() &&
        deadline_due(now_ms, runtime.pending_refresh.due_ms)) {
        if (!runtime.activity.as11_rpc_available) {
            runtime.pending_refresh.summary_attempted = true;
        } else {
            const OperationAdmission admitted =
                runtime.summary_acquisition.request(
                    runtime.pending_refresh.generation);
            if (admitted == OperationAdmission::Accepted) {
                runtime.pending_refresh.summary_attempted = true;
            } else if (admitted == OperationAdmission::Rejected) {
                runtime.pending_refresh.summary_attempted = true;
                ++runtime.command_failures;
            }
        }
        worked = true;
    }

    if (runtime.catalog_refresh.active()) {
        worked = runtime.catalog_refresh.poll() || worked;
    }
    if (runtime.refresh_generation != 0 &&
        !runtime.catalog_refresh.active()) {
        const NightCatalogRefreshStatus status =
            runtime.catalog_refresh.status();
        if (status.state == NightCatalogRefreshState::Ready ||
            status.state == NightCatalogRefreshState::Error) {
            if (status.state == NightCatalogRefreshState::Ready) {
                publish_catalog(runtime.catalog_refresh.snapshot(),
                                runtime.refresh_generation);
                runtime.pending_catalog_save = runtime.catalog;
                runtime.pending_catalog_save_generation =
                    runtime.catalog_generation;
                runtime.pending_catalog_save_post_therapy =
                    runtime.pending_catalog_save_post_therapy ||
                    runtime.refresh_post_therapy;

                if (runtime.refresh_post_therapy) {
                    const NightCatalogRecord *night =
                        runtime.refresh_target.valid()
                        ? runtime.catalog->find(runtime.refresh_target.sleep_day)
                        : runtime.catalog->record(0);
                    runtime.post_therapy_build =
                        night && local_source_available(*night)
                        ? ReportArtifactKey::result(
                              night->sleep_day, night->source_revision)
                        : ReportArtifactKey{};
                }

                runtime.catalog_refresh_retry_at_ms = 0;
                runtime.catalog_refresh_retry_attempt = 0;
            } else if (status.retryable) {
                if (!runtime.pending_refresh.valid()) {
                    runtime.pending_refresh.generation =
                        runtime.refresh_generation;
                    runtime.pending_refresh.due_ms = 0;
                    runtime.pending_refresh.current_offset_valid =
                        runtime.refresh_offset_valid;
                    runtime.pending_refresh.current_offset_minutes =
                        runtime.refresh_offset_minutes;
                    runtime.pending_refresh.target = runtime.refresh_target;
                    runtime.pending_refresh.summary_attempted = true;
                    runtime.pending_refresh.post_therapy =
                        runtime.refresh_post_therapy;
                } else {
                    runtime.pending_refresh.post_therapy =
                        runtime.pending_refresh.post_therapy ||
                        runtime.refresh_post_therapy;
                }
                runtime.catalog_refresh_retry_at_ms =
                    now_ms + retry_delay(
                                 runtime.catalog_refresh_retry_attempt);
                advance_retry(runtime.catalog_refresh_retry_attempt);
                ++runtime.command_failures;
            } else {
                if (runtime.refresh_post_therapy ||
                    runtime.refresh_target.valid()) {
                    runtime.schedule_reconcile(
                        now_ms,
                        true,
                        false);
                }
                ++runtime.command_failures;
            }
            runtime.refresh_generation = 0;
            runtime.refresh_target = {};
            runtime.refresh_post_therapy = false;
            worked = true;
        }
    }

    if (runtime.pending_refresh.valid() &&
        runtime.pending_refresh.summary_attempted &&
        !runtime.summary_acquisition.active() &&
        !runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_load_pending && !local_blocked &&
        startup_allowed &&
        runtime.catalog_storage_ready() &&
        deadline_due(now_ms, runtime.pending_refresh.due_ms) &&
        deadline_due(now_ms, runtime.catalog_refresh_retry_at_ms)) {
        std::shared_ptr<const NightCatalogSummarySnapshot> summary =
            runtime.summary_acquisition.snapshot();
        if (summary && runtime.catalog &&
            runtime.summary_acquisition.status().state ==
                ReportSummaryAcquisitionState::Ready &&
            runtime.summary_acquisition.status().generation ==
                runtime.pending_refresh.generation) {
            summary = NightCatalogSummarySnapshot::preserve_expired_history(
                *summary, *runtime.catalog);
            if (summary) runtime.summary_acquisition.seed(summary);
        }

        const OperationAdmission admitted =
            runtime.catalog_refresh.request_refresh(
                summary,
                runtime.pending_refresh.current_offset_valid,
                runtime.pending_refresh.current_offset_minutes,
                runtime.pending_refresh.generation,
                runtime.catalog,
                runtime.pending_refresh.target);
        if (admitted == OperationAdmission::Accepted) {
            runtime.refresh_generation =
                runtime.pending_refresh.generation;
            runtime.refresh_offset_valid =
                runtime.pending_refresh.current_offset_valid;
            runtime.refresh_offset_minutes =
                runtime.pending_refresh.current_offset_minutes;
            runtime.refresh_target = runtime.pending_refresh.target;
            runtime.refresh_post_therapy =
                runtime.pending_refresh.post_therapy;
            runtime.pending_refresh.clear();
            runtime.catalog_refresh_retry_at_ms = 0;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.catalog_refresh_retry_at_ms =
                now_ms + retry_delay(runtime.catalog_refresh_retry_attempt);
            advance_retry(runtime.catalog_refresh_retry_attempt);
            ++runtime.command_failures;
        }
        worked = true;
    }

    worked = runtime.handle_fallback_replacement() || worked;

    if ((!local_blocked || (runtime.capture_session.raw_segment_start_ms &&
                           runtime.capture_storage_ready())) &&
        runtime.pending_catalog_save &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        deadline_due(now_ms, runtime.catalog_store_retry_at_ms)) {
        const OperationAdmission admitted =
            runtime.catalog_store.request_save(
                runtime.pending_catalog_save,
                runtime.pending_catalog_save_generation);
        if (admitted == OperationAdmission::Accepted) {
            runtime.store_purpose = CatalogStorePurpose::Save;
            runtime.catalog_store_save_generation =
                runtime.pending_catalog_save_generation;
        } else if (admitted == OperationAdmission::Rejected) {
            ++runtime.command_failures;
            runtime.catalog_store_retry_at_ms =
                now_ms + retry_delay(runtime.catalog_store_retry_attempt);
            advance_retry(runtime.catalog_store_retry_attempt);
        }
        worked = true;
    }

    if (runtime.spool_availability_probe.status().active()) {
        worked = runtime.spool_availability_probe.poll() || worked;
    }
    worked = runtime.observe_spool_probe(now_ms) || worked;

    const bool catalog_stable = runtime.catalog &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        !runtime.pending_catalog_save &&
        !runtime.pending_refresh.valid() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_refresh.active() &&
        !runtime.summary_acquisition.active() &&
        !runtime.engine.catalog_update_required();
    if (catalog_stable && !local_blocked && startup_allowed) {
        worked = runtime.schedule_post_therapy_build() || worked;
        if (!runtime.store_catalog_load_pending &&
            !runtime.store_catalog_loader.status().active()) {
            worked = runtime.start_spool_probe(now_ms) || worked;
            worked = runtime.schedule_background(now_ms) || worked;
        }
    }

    worked = runtime.schedule_capture(now_ms) || worked;
    if (!runtime.activity.therapy_active || runtime.capture_storage_ready()) {
        worked = runtime.engine.poll(
            now_ms, runtime.activity.therapy_active
                ? 1 : std::max<size_t>(record_budget, 1)) || worked;
    }
    worked = runtime.observe_engine(now_ms) || worked;
    worked = runtime.advance_rebuild() || worked;
    runtime.publish_status();
    return worked;
}

void ReportTask::publish_catalog(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (runtime_) runtime_->accept_catalog(std::move(catalog), generation);
}

void ReportTask::task_entry(void *context) {
    ReportTask *self = static_cast<ReportTask *>(context);
    if (self) self->run();
#ifdef ARDUINO
    vTaskDelete(nullptr);
#endif
}

void ReportTask::run() {
#ifdef ARDUINO
    if (!runtime_) return;

    runtime_->task_started = true;
    runtime_->publish_status();
    for (;;) {
        const bool worked = step(
            millis(), AC_REPORT_FOREGROUND_RECORD_BUDGET);
        const ReportTaskControlSnapshot status = control_snapshot();
        if (worked) {
            vTaskDelay(pdMS_TO_TICKS(AC_REPORT_TASK_WORK_TICK_MS));
        } else if (status.background_active) {
            ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(AC_REPORT_TASK_WAIT_TICK_MS));
        } else {
            ulTaskNotifyTake(pdTRUE,
                             pdMS_TO_TICKS(AC_REPORT_TASK_IDLE_TICK_MS));
        }
    }
#endif
}

}  // namespace aircannect
