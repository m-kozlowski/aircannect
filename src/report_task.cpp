#include "report_task.h"

#include <algorithm>
#include <new>
#include <utility>

#include "board_report.h"
#include "night_catalog_builder.h"
#include "report_fallback_artifact.h"
#include "report_night_artifact_builder.h"
#include "string_util.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr uint32_t CATALOG_STORE_GENERATION = 1;
constexpr uint32_t CATALOG_STORE_RETRY_MIN_MS = 1000;
constexpr uint32_t CATALOG_STORE_RETRY_MAX_MS = 30000;
constexpr uint32_t LEGACY_CACHE_DELETE_RETRY_MS = 30000;
constexpr uint32_t ARTIFACT_FAILURE_RETRY_MS = 30000;
constexpr size_t IDLE_QUEUE_LOOKAHEAD = 2;

constexpr char LEGACY_CACHE_PARENT[] = "/aircannect/report";
constexpr const char *LEGACY_CACHE_NAMES[] = {"v3", "v4", "v5"};

enum class ReportTaskCommandKind : uint8_t {
    Artifact,
    CacheArtifact,
    RefreshCatalog,
};

struct ReportTaskCommand {
    ReportTaskCommandKind kind = ReportTaskCommandKind::Artifact;
    ReportArtifactKey artifact;
    ReportRequestPriority priority = ReportRequestPriority::Foreground;
    uint32_t generation = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
};

struct PendingCatalogRefresh {
    uint32_t generation = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
    bool summary_attempted = false;

    bool valid() const { return generation != 0; }
    void clear() { *this = {}; }
};

struct ReportArtifactFailureEntry {
    ReportArtifactKey artifact;
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t retry_at_ms = 0;

    bool valid() const { return artifact.valid() && error[0] != '\0'; }
};

enum class CatalogStorePurpose : uint8_t {
    None,
    Load,
    Save,
};

bool same_artifact_identity(const ReportArtifactKey &lhs,
                            const ReportArtifactKey &rhs) {
    return lhs.sleep_day == rhs.sleep_day && lhs.kind == rhs.kind &&
           lhs.range_start_ms == rhs.range_start_ms &&
           lhs.range_end_ms == rhs.range_end_ms;
}

uint32_t next_background_retry_delay(uint8_t attempt) {
    uint32_t delay_ms = CATALOG_STORE_RETRY_MIN_MS;
    for (uint8_t i = 0; i < attempt && delay_ms < CATALOG_STORE_RETRY_MAX_MS;
         ++i) {
        delay_ms = std::min(delay_ms * 2, CATALOG_STORE_RETRY_MAX_MS);
    }
    return delay_ms;
}

void advance_background_retry(uint8_t &attempt) {
    if (attempt < 5) ++attempt;
}

uint32_t next_catalog_generation(uint32_t generation) {
    generation++;
    return generation == 0 ? 1 : generation;
}

bool deadline_due(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_ms == 0 ||
           static_cast<int32_t>(now_ms - deadline_ms) >= 0;
}

}  // namespace

struct ReportTask::Runtime {
    Runtime() :
        engine(build_slots, AC_REPORT_TASK_BUILD_CAPACITY),
        payload_cache(AC_REPORT_PAYLOAD_CACHE_MAX_BYTES) {}

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

        for (size_t i = 0; i < command_count; ++i) {
            ReportTaskCommand &queued = commands[i];
            const bool artifact_command =
                command.kind == ReportTaskCommandKind::Artifact ||
                command.kind == ReportTaskCommandKind::CacheArtifact;
            if (artifact_command &&
                queued.kind == command.kind &&
                same_artifact_identity(queued.artifact, command.artifact)) {
                queued = command;
                unlock();
                wake();
                return OperationAdmission::Accepted;
            }
            if (command.kind == ReportTaskCommandKind::RefreshCatalog &&
                queued.kind == command.kind) {
                queued = command;
                unlock();
                wake();
                return OperationAdmission::Accepted;
            }
        }

        if (command_count >= AC_REPORT_TASK_COMMAND_CAPACITY) {
            command_drops++;
            unlock();
            return OperationAdmission::Busy;
        }

        commands[command_count++] = command;
        unlock();
        wake();
        return OperationAdmission::Accepted;
    }

    bool pop(ReportTaskCommand &command, bool cache_load_available) {
        if (!lock()) return false;
        if (command_count == 0) {
            unlock();
            return false;
        }

        size_t selected = SIZE_MAX;
        for (size_t i = 0; i < command_count; ++i) {
            if (commands[i].kind == ReportTaskCommandKind::Artifact &&
                commands[i].priority ==
                    ReportRequestPriority::Foreground) {
                selected = i;
                break;
            }
        }
        if (selected == SIZE_MAX) {
            for (size_t i = 0; i < command_count; ++i) {
                if (commands[i].kind ==
                        ReportTaskCommandKind::CacheArtifact &&
                    !cache_load_available) {
                    continue;
                }

                selected = i;
                break;
            }
        }
        if (selected == SIZE_MAX) {
            unlock();
            return false;
        }

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

    bool defer_active_catalog_refresh() {
        if (!catalog_refresh.active() || refresh_generation == 0) {
            return false;
        }

        if (!pending_refresh.valid()) {
            pending_refresh.generation = refresh_generation;
            pending_refresh.current_offset_valid = refresh_offset_valid;
            pending_refresh.current_offset_minutes = refresh_offset_minutes;
            pending_refresh.summary_attempted = true;
        }

        catalog_refresh.cancel();
        refresh_generation = 0;
        catalog_refresh_retry_at_ms = 0;
        return true;
    }

    bool preempt_background_for_foreground() {
        if (!engine.status().foreground_active) return false;

        bool worked = engine.cancel_background() > 0;

        if (payload_loader.status().active()) {
            payload_loader.cancel();
            worked = true;
        }

        if (summary_acquisition.active()) {
            summary_acquisition.cancel();
            worked = true;
        }

        worked = defer_active_catalog_refresh() || worked;

        if (artifact_index_refresh.active()) {
            artifact_index_refresh.cancel();
            artifact_index_refresh_generation = 0;
            artifact_index_refresh_pending = true;
            worked = true;
        }

        return worked;
    }

    bool background_work_blocked() const {
        return background_suspended || engine.status().foreground_active;
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

        const bool was_suspended = background_suspended;
        activity = next;
        background_suspended =
            activity.therapy_active || activity.realtime_stream_active ||
            activity.foreground_report_demand ||
            activity.ota_install_active || activity.export_work_claimed;
        if (!background_suspended || was_suspended) return true;

        (void)engine.cancel_background();
        idle_cursor = 0;

        if (summary_acquisition.active()) {
            summary_acquisition.cancel();
        }

        if (payload_loader.status().active()) {
            payload_loader.cancel();
        }

        (void)defer_active_catalog_refresh();

        if (artifact_index_refresh.active()) {
            artifact_index_refresh.cancel();
            artifact_index_refresh_generation = 0;
            artifact_index_refresh_pending = true;
        }

        return true;
    }

    bool find_availability(const ReportArtifactKey &artifact,
                           ReportArtifactAvailability &out) {
        out = {};
        if (!artifact.valid() || !lock(20)) return false;

        const NightCatalogRecord *night = published_catalog
            ? published_catalog->find(artifact.sleep_day)
            : nullptr;
        if (!night || night->source_revision != artifact.source_revision) {
            unlock();
            return false;
        }

        const bool found = published_artifact_index &&
            published_artifact_index->availability(artifact, out);
        unlock();
        return found;
    }

    std::shared_ptr<const LargeByteBuffer> find_payload(
        const ReportArtifactDescriptor &artifact) {
        if (!artifact.valid() || !lock(20)) return {};

        std::shared_ptr<const LargeByteBuffer> out =
            payload_cache.find(artifact);
        unlock();
        return out;
    }

    bool payload_cached(const ReportArtifactDescriptor &artifact) const {
        if (!artifact.valid() || !lock(20)) return false;

        const bool cached = payload_cache.contains(artifact);
        unlock();
        return cached;
    }

    bool prepare_payload_allocation(
        const ReportArtifactDescriptor &artifact) {
        if (!artifact.valid() || !lock(20)) return false;
        if (!payload_cache.can_hold(artifact)) {
            unlock();
            return false;
        }

#ifdef ARDUINO
        MemoryStatus memory = Memory::status();
        while (memory.psram_available &&
               memory.psram_free <
                   artifact.size +
                       AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE &&
               payload_cache.evict_lru()) {
            memory = Memory::status();
        }
        const bool available = memory.psram_available &&
            memory.psram_free >=
                artifact.size + AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE;
#else
        const bool available = true;
#endif

        unlock();
        return available;
    }

    bool cache_payload(const ReportArtifactDescriptor &artifact,
                       std::shared_ptr<const LargeByteBuffer> bytes) {
        if (!artifact.valid() || !bytes || !lock(20)) return false;

#ifdef ARDUINO
        MemoryStatus memory = Memory::status();
        while (memory.psram_available &&
               memory.psram_free <
                   AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE &&
               payload_cache.evict_lru()) {
            memory = Memory::status();
        }
        if (!memory.psram_available ||
            memory.psram_free < AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE) {
            unlock();
            return false;
        }
#endif

        const bool inserted = payload_cache.insert(artifact, std::move(bytes));
        unlock();
        return inserted;
    }

    bool cache_bundle(
        const std::shared_ptr<const ReportArtifactBundle> &bundle) {
        if (!bundle || !bundle->valid()) return false;

        bool cached = false;
        if (bundle->key.kind == ReportArtifactKind::Result) {
            ReportArtifactDescriptor result;
            result.key = ReportArtifactKey::result(
                bundle->key.sleep_day, bundle->key.source_revision);
            result.size = bundle->result->size();
            result.crc32 = bundle->result_crc32;
            cached = cache_payload(result, bundle->result) || cached;

            ReportArtifactDescriptor overview;
            overview.key = ReportArtifactKey::overview(
                bundle->key.sleep_day, bundle->key.source_revision);
            overview.size = bundle->overview->size();
            overview.crc32 = bundle->overview_crc32;
            cached = cache_payload(overview, bundle->overview) || cached;
            return cached;
        }

        if (bundle->key.kind == ReportArtifactKind::RangeTile) {
            ReportArtifactDescriptor tile;
            tile.key = bundle->key;
            tile.size = bundle->range_tile->size();
            tile.crc32 = bundle->range_tile_crc32;
            cached = cache_payload(tile, bundle->range_tile);
        }
        return cached;
    }

    OperationAdmission start_payload_load(
        const ReportArtifactKey &artifact,
        uint32_t generation,
        StorageReadLane lane) {
        if (payload_loader.status().active()) return OperationAdmission::Busy;

        ReportArtifactAvailability availability;
        if (!artifact_index ||
            !artifact_index->availability(artifact, availability)) {
            return OperationAdmission::Rejected;
        }

        ReportArtifactDescriptor descriptor;
        if (!availability.descriptor(artifact, descriptor)) {
            return OperationAdmission::Rejected;
        }
        if (payload_cached(descriptor)) return OperationAdmission::Accepted;
        if (!prepare_payload_allocation(descriptor)) {
            return OperationAdmission::Rejected;
        }

        return payload_loader.start(descriptor, generation, lane);
    }

    bool finish_payload_load(uint32_t now_ms) {
        const ReportArtifactPayloadLoadStatus load_status =
            payload_loader.status();
        if (!load_status.terminal()) return false;

        if (load_status.state == ReportArtifactPayloadLoadState::Ready) {
            std::shared_ptr<const LargeByteBuffer> bytes =
                payload_loader.take_completed();
            if (bytes) (void)cache_payload(load_status.artifact,
                                           std::move(bytes));
            payload_load_failed = {};
            payload_load_retry_at_ms = 0;
            return true;
        }

        if (load_status.state == ReportArtifactPayloadLoadState::Error) {
            payload_load_failed = load_status.artifact.key;
            payload_load_retry_at_ms =
                now_ms + ARTIFACT_FAILURE_RETRY_MS;
        }
        payload_loader.reset();
        return true;
    }

    bool payload_load_suppressed(const ReportArtifactKey &artifact,
                                 uint32_t now_ms) const {
        return payload_load_failed == artifact &&
               !deadline_due(now_ms, payload_load_retry_at_ms);
    }

    bool find_failure(const ReportArtifactKey &artifact,
                      ReportArtifactFailureStatus &out) const {
        out = {};
        if (!artifact.valid() || !lock(20)) return false;

        for (const ReportArtifactFailureEntry &entry : artifact_failures) {
            if (!entry.valid() || entry.artifact != artifact ||
                deadline_due(last_step_ms, entry.retry_at_ms)) {
                continue;
            }

            copy_cstr(out.error, sizeof(out.error), entry.error);
            out.retry_after_ms = entry.retry_at_ms - last_step_ms;
            unlock();
            return true;
        }

        unlock();
        return false;
    }

    bool publish_artifact_index(
        std::shared_ptr<const ReportArtifactIndex> next) {
        if (!next || !lock(20)) return false;

        artifact_index = std::move(next);
        published_artifact_index = artifact_index;
        unlock();
        return true;
    }

    bool merge_availability(
        const ReportArtifactAvailability &availability) {
        if (!availability.requested_ready()) return false;

        std::shared_ptr<const ReportArtifactIndex> source = artifact_index;
        if (!source) source = ReportArtifactIndexBuilder::build(nullptr, 0);
        if (!source) return false;

        std::shared_ptr<const ReportArtifactIndex> updated =
            ReportArtifactIndexBuilder::merge_availability(
                *source, availability);
        return publish_artifact_index(std::move(updated));
    }

    void observe_engine_completion(uint32_t now_ms) {
        const ReportEngineCompletion completion =
            engine.status().last_completion;
        if (!completion.valid() ||
            completion.request.ticket == observed_engine_completion) {
            return;
        }
        if (!lock(20)) return;

        observed_engine_completion = completion.request.ticket;
        if (completion.outcome.disposition ==
            OperationDisposition::Succeeded) {
            clear_artifact_failure_locked(completion.request.artifact);
            unlock();
            return;
        }
        if (completion.outcome.disposition != OperationDisposition::Failed ||
            completion.request.priority !=
                ReportRequestPriority::Foreground) {
            unlock();
            return;
        }

        remember_artifact_failure_locked(
            completion.request.artifact,
            completion.error[0] ? completion.error : "report_build_failed",
            now_ms);
        unlock();
    }

    void remember_artifact_failure_locked(const ReportArtifactKey &artifact,
                                          const char *error,
                                          uint32_t now_ms) {
        if (!artifact.valid() || !error || !error[0]) return;

        size_t selected = AC_REPORT_TASK_BUILD_CAPACITY;
        for (size_t i = 0; i < AC_REPORT_TASK_BUILD_CAPACITY; ++i) {
            if (artifact_failures[i].valid() &&
                artifact_failures[i].artifact == artifact) {
                selected = i;
                break;
            }
            if (selected == AC_REPORT_TASK_BUILD_CAPACITY &&
                !artifact_failures[i].valid()) {
                selected = i;
            }
        }
        if (selected == AC_REPORT_TASK_BUILD_CAPACITY) {
            selected = artifact_failure_cursor;
            artifact_failure_cursor =
                (artifact_failure_cursor + 1) %
                AC_REPORT_TASK_BUILD_CAPACITY;
        }

        ReportArtifactFailureEntry &entry = artifact_failures[selected];
        entry.artifact = artifact;
        copy_cstr(entry.error, sizeof(entry.error), error);
        entry.retry_at_ms = now_ms + ARTIFACT_FAILURE_RETRY_MS;
    }

    void clear_artifact_failure_locked(const ReportArtifactKey &artifact) {
        for (ReportArtifactFailureEntry &entry : artifact_failures) {
            if (entry.valid() && entry.artifact == artifact) entry = {};
        }
    }

    void clear_artifact_failures() {
        if (!lock(20)) return;

        for (ReportArtifactFailureEntry &entry : artifact_failures) {
            entry = {};
        }
        artifact_failure_cursor = 0;
        unlock();
    }

    void accept_catalog(std::shared_ptr<const NightCatalog> next,
                        uint32_t generation) {
        if (artifact_index_refresh.active()) {
            artifact_index_refresh.cancel();
            artifact_index_refresh_generation = 0;
            artifact_index_refresh_pending = true;
        }

        catalog = std::move(next);
        catalog_generation = generation;
        engine.publish_catalog(catalog);
        clear_artifact_failures();

        if (payload_loader.status().active()) {
            const ReportArtifactDescriptor loading =
                payload_loader.status().artifact;
            const NightCatalogRecord *loading_night =
                catalog->find(loading.key.sleep_day);
            if (!loading_night ||
                loading_night->source_revision !=
                    loading.key.source_revision) {
                payload_loader.cancel();
            }
        }

        if (lock(20)) {
            payload_cache.reconcile(*catalog);
            unlock();
        }

        if (!summary_acquisition.snapshot()) {
            summary_acquisition.seed(
                NightCatalogSummarySnapshot::from_catalog(*catalog));
        }

        if (artifact_index) {
            std::shared_ptr<const ReportArtifactIndex> reconciled =
                ReportArtifactIndexBuilder::reconcile(
                    *artifact_index, *catalog);
            if (!publish_artifact_index(std::move(reconciled))) {
                artifact_index_refresh_pending = true;
                command_failures++;
            }
        } else {
            artifact_index_refresh_pending = true;
        }

        idle_cursor = 0;
        idle_generation = (idle_generation + 1) | 0x80000000u;

        if (!lock()) return;
        published_catalog = catalog;
        unlock();
    }

    bool schedule_catalog_work(uint32_t now_ms) {
        if (!catalog || idle_cursor >= catalog->size()) return false;

        const ReportEngineStatus engine_status = engine.status();
        if (engine_status.queued >= IDLE_QUEUE_LOOKAHEAD) return false;
        if (payload_loader.status().active()) return false;

        const NightCatalogRecord *night = catalog->record(idle_cursor);
        if (!night || !night->sleep_day.valid() ||
            !night->source_revision.valid()) {
            idle_cursor++;
            command_failures++;
            return true;
        }

        const ReportRequestPriority priority = idle_cursor == 0
            ? ReportRequestPriority::Reconcile
            : ReportRequestPriority::Idle;
        const ReportArtifactKey result = ReportArtifactKey::result(
            night->sleep_day, night->source_revision);
        ReportArtifactAvailability available;
        if (artifact_index && artifact_index->availability(result, available)) {
            const bool payload_warm_available =
                engine_status.state == ReportEngineState::Idle &&
                engine_status.queued == 0;
            if (idle_cursor < AC_REPORT_PAYLOAD_CACHE_WARM_NIGHTS &&
                payload_warm_available) {
                const ReportArtifactDescriptor candidates[] = {
                    available.result,
                    available.overview,
                };
                for (const ReportArtifactDescriptor &candidate : candidates) {
                    if (!candidate.valid() || payload_cached(candidate) ||
                        payload_load_suppressed(candidate.key, now_ms)) {
                        continue;
                    }

                    const OperationAdmission admitted = start_payload_load(
                        candidate.key,
                        idle_generation,
                        StorageReadLane::Maintenance);
                    if (admitted == OperationAdmission::Accepted) return true;
                    if (admitted == OperationAdmission::Busy) return false;
                }
            }

            idle_cursor++;
            return true;
        }

        const ReportRequestEnqueueResult queued = engine.request(
            result,
            priority,
            idle_generation);
        if (queued.status == ReportRequestEnqueueStatus::Full) return false;

        idle_cursor++;
        if (queued.status == ReportRequestEnqueueStatus::Invalid) {
            command_failures++;
        }
        return true;
    }

    bool startup_idle_work_allowed(uint32_t now_ms) {
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

    bool schedule_legacy_cache_cleanup(uint32_t now_ms) {
        if (!legacy_cleanup_pending || !delete_port || !catalog ||
            idle_cursor < catalog->size() ||
            !deadline_due(now_ms, legacy_cleanup_retry_at_ms)) {
            return false;
        }

        const ReportEngineStatus engine_status = engine.status();
        if (engine_status.state != ReportEngineState::Idle ||
            engine_status.queued != 0) {
            return false;
        }

        const bool accepted = delete_port->start_selected(
            LEGACY_CACHE_PARENT,
            LEGACY_CACHE_NAMES,
            sizeof(LEGACY_CACHE_NAMES) / sizeof(LEGACY_CACHE_NAMES[0]));
        if (accepted) {
            legacy_cleanup_pending = false;
            legacy_cleanup_retry_at_ms = 0;
        } else {
            legacy_cleanup_retry_at_ms =
                now_ms + LEGACY_CACHE_DELETE_RETRY_MS;
        }
        return true;
    }

    void publish_status() {
        size_t queued = 0;
        uint32_t drops = 0;
        bool foreground_command = false;
        ReportArtifactPayloadCacheStatus payload_cache_status;
        if (lock()) {
            queued = command_count;
            drops = command_drops;
            payload_cache_status = payload_cache.status();
            for (size_t i = 0; i < command_count; ++i) {
                if (commands[i].kind == ReportTaskCommandKind::Artifact &&
                    commands[i].priority ==
                        ReportRequestPriority::Foreground) {
                    foreground_command = true;
                    break;
                }
            }
            unlock();
        }

        ReportTaskStatus &next = status_scratch;
        next = {};
        next.initialized = initialized;
        next.task_started = task_started;
        next.commands_queued = queued;
        next.catalog_nights = catalog ? catalog->size() : 0;
        next.command_drops = drops;
        next.command_failures = command_failures;
        next.catalog_generation = catalog_generation;
        next.durable_catalog_generation = durable_catalog_generation;
        next.background_suspended = background_suspended;
        next.summary_acquisition = summary_acquisition.status();
        next.catalog_refresh = catalog_refresh.status();
        next.catalog_store = catalog_store.status();
        next.artifact_index_refresh = artifact_index_refresh.status();
        next.payload_cache = payload_cache_status;
        next.payload_load = payload_loader.status();
        next.engine = engine.status();
        next.foreground_active =
            foreground_command || next.engine.foreground_active;

        if (!initialized) {
            next.state = ReportTaskState::Stopped;
        } else if (store_purpose == CatalogStorePurpose::Load ||
                   catalog_load_pending) {
            next.state = ReportTaskState::LoadingCatalog;
        } else if (!artifact_index_loaded &&
                   (artifact_index_refresh.active() ||
                    artifact_index_refresh_pending)) {
            next.state = ReportTaskState::IndexingArtifacts;
        } else if (summary_acquisition.active() ||
                   catalog_refresh.active() || pending_refresh.valid() ||
                   refresh_generation != 0 ||
                   engine.catalog_update_required()) {
            next.state = ReportTaskState::RefreshingCatalog;
        } else {
            switch (next.engine.state) {
                case ReportEngineState::Publishing:
                    next.state = ReportTaskState::Publishing;
                    break;
                case ReportEngineState::LookingUp:
                    next.state = ReportTaskState::LookingUp;
                    break;
                case ReportEngineState::Executing:
                case ReportEngineState::AcquiringFallback:
                    next.state = ReportTaskState::Building;
                    break;
                case ReportEngineState::Queued:
                case ReportEngineState::WaitingForCatalog:
                    next.state = ReportTaskState::Queued;
                    break;
                case ReportEngineState::Idle:
                default:
                    next.state = ReportTaskState::Idle;
                    break;
            }
        }
        const bool catalog_commit_pending =
            store_purpose == CatalogStorePurpose::Save ||
            static_cast<bool>(pending_catalog_save);
        next.background_active =
            queued > 0 || catalog_commit_pending ||
            next.payload_load.active() ||
            (next.state != ReportTaskState::Stopped &&
             next.state != ReportTaskState::Idle);

        if (!lock()) return;
        status = next;
        unlock();
    }

    ReportArtifactRequest build_slots[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    ReportEngine engine;
    ReportArtifactPayloadCache payload_cache;
    ReportArtifactPayloadLoader payload_loader;
    ReportNightArtifactBuilder builder;
    ReportSummaryAcquisition summary_acquisition;
    NightCatalogRefreshService catalog_refresh;
    NightCatalogStoreService catalog_store;
    ReportArtifactIndexRefreshService artifact_index_refresh;
    StorageDeletePort *delete_port = nullptr;

    ReportTaskCommand commands[AC_REPORT_TASK_COMMAND_CAPACITY] = {};
    size_t command_count = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    ReportArtifactFailureEntry
        artifact_failures[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    size_t artifact_failure_cursor = 0;
    OperationTicket observed_engine_completion;
    uint32_t last_step_ms = 0;
    ReportArtifactKey payload_load_failed;
    uint32_t payload_load_retry_at_ms = 0;
    PendingCatalogRefresh pending_refresh;
    uint32_t refresh_generation = 0;
    uint32_t catalog_refresh_retry_at_ms = 0;
    uint8_t catalog_refresh_retry_attempt = 0;

    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const NightCatalog> published_catalog;
    std::shared_ptr<const NightCatalog> pending_catalog_save;
    std::shared_ptr<const ReportArtifactIndex> artifact_index;
    std::shared_ptr<const ReportArtifactIndex> published_artifact_index;
    uint32_t catalog_generation = 0;
    uint32_t durable_catalog_generation = 0;
    size_t idle_cursor = 0;
    uint32_t idle_generation = 0x80000000u;
    uint32_t legacy_cleanup_retry_at_ms = 0;
    bool legacy_cleanup_pending = true;

    ActivitySnapshot activity;
    ActivitySnapshot pending_activity;
    bool activity_pending = false;
    bool background_suspended = false;
    bool startup_idle_grace_complete = false;

    bool refresh_offset_valid = false;
    int32_t refresh_offset_minutes = 0;

    bool artifact_index_loaded = false;
    bool artifact_index_refresh_pending = false;
    uint32_t artifact_index_refresh_generation = 0;
    uint32_t artifact_index_retry_at_ms = 0;
    uint8_t artifact_index_retry_attempt = 0;

    CatalogStorePurpose store_purpose = CatalogStorePurpose::None;
    bool catalog_load_pending = true;
    uint32_t catalog_store_retry_at_ms = 0;
    uint8_t catalog_store_retry_attempt = 0;

    bool initialized = false;
    bool task_started = false;
    ReportTaskStatus status_scratch;
    ReportTaskStatus status;

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
                       StorageDeletePort &delete_port) {
    if (runtime_) return runtime_->initialized;

#ifdef ARDUINO
    void *memory = Memory::alloc_large(sizeof(Runtime), false);
    runtime_ = memory ? new (memory) Runtime() : nullptr;
#else
    runtime_ = new (std::nothrow) Runtime();
#endif
    if (!runtime_) return false;

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
    runtime_->artifact_index_refresh.begin(scan_port, read_port);
    runtime_->summary_acquisition.begin(spool_port);
    runtime_->payload_loader.begin(read_port);
    runtime_->delete_port = &delete_port;
    runtime_->engine.begin(read_port,
                           write_port,
                           spool_port,
                           runtime_->builder);
    runtime_->initialized = true;
    runtime_->publish_status();

#ifdef ARDUINO
    BaseType_t created = pdFAIL;
    if (Memory::psram_available()) {
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

    if (!runtime_->task_stack_external) {
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

    if (created != pdPASS || !runtime_->task) {
        runtime_->initialized = false;
        runtime_->publish_status();
        vSemaphoreDelete(runtime_->mutex);
        runtime_->mutex = nullptr;
        runtime_->~Runtime();
        Memory::free(runtime_);
        runtime_ = nullptr;
        return false;
    }
#endif
    return true;
}

OperationAdmission ReportTask::request_artifact(
    const ReportArtifactKey &artifact,
    ReportRequestPriority priority,
    uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || !artifact.valid() ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::Artifact;
    command.artifact = artifact;
    command.priority = priority;
    command.generation = generation;
    return runtime_->enqueue(command);
}

OperationAdmission ReportTask::request_payload_cache(
    const ReportArtifactKey &artifact,
    uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || !artifact.valid() ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::CacheArtifact;
    command.artifact = artifact;
    command.priority = ReportRequestPriority::Foreground;
    command.generation = generation;
    return runtime_->enqueue(command);
}

OperationAdmission ReportTask::request_catalog_refresh(
    bool current_offset_valid,
    int32_t current_offset_minutes,
    uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::RefreshCatalog;
    command.generation = generation;
    command.current_offset_valid = current_offset_valid;
    command.current_offset_minutes = current_offset_minutes;
    return runtime_->enqueue(command);
}

void ReportTask::publish_activity(const ActivitySnapshot &activity) {
    if (!runtime_ || !runtime_->initialized) return;
    runtime_->publish_activity(activity);
}

ReportTaskControlSnapshot ReportTask::control_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};

    ReportTaskControlSnapshot out;
    out.initialized = runtime_->status.initialized;
    out.task_started = runtime_->status.task_started;
    out.state = runtime_->status.state;
    out.catalog_generation = runtime_->status.catalog_generation;
    out.durable_catalog_generation =
        runtime_->status.durable_catalog_generation;
    out.foreground_active = runtime_->status.foreground_active;
    out.background_active = runtime_->status.background_active;

    runtime_->unlock();
    return out;
}

ReportTaskDiagnosticSnapshot ReportTask::diagnostic_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};

    const ReportTaskStatus &status = runtime_->status;
    const ReportEngineStatus &engine = status.engine;
    const ReportFallbackAcquisitionStatus &fallback = engine.fallback;
    const NightCatalogRefreshStatus &catalog = status.catalog_refresh;

    ReportTaskDiagnosticSnapshot out;
    out.task_started = status.task_started;
    out.state = status.state;
    out.commands_queued = status.commands_queued;
    out.catalog_nights = status.catalog_nights;
    out.command_drops = status.command_drops;
    out.command_failures = status.command_failures;
    out.catalog_generation = status.catalog_generation;
    out.durable_catalog_generation = status.durable_catalog_generation;
    out.foreground_active = status.foreground_active;
    out.background_active = status.background_active;
    out.background_suspended = status.background_suspended;

    out.payload_cache_entries = status.payload_cache.entries;
    out.payload_cache_bytes = status.payload_cache.bytes;
    out.payload_cache_hits = status.payload_cache.hits;
    out.payload_cache_misses = status.payload_cache.misses;
    out.payload_cache_evictions = status.payload_cache.evictions;
    out.payload_load_state = status.payload_load.state;
    out.payload_load_bytes = status.payload_load.bytes_loaded;
    copy_cstr(out.payload_load_error, sizeof(out.payload_load_error),
              status.payload_load.error);

    out.engine_state = engine.state;
    out.engine_queued = engine.queued;
    copy_cstr(out.engine_error, sizeof(out.engine_error),
              engine.last_completion.error);

    out.fallback_state = fallback.state;
    out.fallback_source = fallback.source;
    out.fallback_sources_total = fallback.sources_total;
    out.fallback_sources_completed = fallback.sources_completed;
    out.fallback_sections_added = fallback.sections_added;
    out.fallback_unavailable_added = fallback.unavailable_added;
    copy_cstr(out.fallback_error, sizeof(out.fallback_error),
              fallback.error);

    out.catalog_state = catalog.state;
    out.catalog_files_seen = catalog.files_seen;
    out.catalog_files_indexed = catalog.files_indexed;
    out.catalog_sessions = catalog.sessions;
    copy_cstr(out.catalog_error, sizeof(out.catalog_error), catalog.error);

    runtime_->unlock();
    return out;
}

#ifndef ARDUINO
ReportTaskStatus ReportTask::status() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    const ReportTaskStatus out = runtime_->status;
    runtime_->unlock();
    return out;
}
#endif

std::shared_ptr<const NightCatalog> ReportTask::catalog_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    std::shared_ptr<const NightCatalog> out = runtime_->published_catalog;
    runtime_->unlock();
    return out;
}

bool ReportTask::artifact_availability(
    const ReportArtifactKey &artifact,
    ReportArtifactAvailability &availability) const {
    if (!runtime_) {
        availability = {};
        return false;
    }
    return runtime_->find_availability(artifact, availability);
}

std::shared_ptr<const LargeByteBuffer> ReportTask::artifact_payload(
    const ReportArtifactDescriptor &artifact) const {
    if (!runtime_) return {};
    return runtime_->find_payload(artifact);
}

bool ReportTask::artifact_failure(
    const ReportArtifactKey &artifact,
    ReportArtifactFailureStatus &failure) const {
    if (!runtime_) {
        failure = {};
        return false;
    }
    return runtime_->find_failure(artifact, failure);
}

bool ReportTask::step(uint32_t now_ms, size_t record_budget) {
    if (!runtime_ || !runtime_->initialized) return false;
    Runtime &runtime = *runtime_;
    if (runtime.lock(20)) {
        runtime.last_step_ms = now_ms;
        runtime.unlock();
    }

    bool worked = runtime.apply_pending_activity();
    const bool startup_idle_work_allowed =
        runtime.startup_idle_work_allowed(now_ms);

    if (runtime.payload_loader.status().active()) {
        worked = runtime.payload_loader.poll() || worked;
    }
    worked = runtime.finish_payload_load(now_ms) || worked;

    ReportTaskCommand command;
    const ReportEngineStatus command_engine_status = runtime.engine.status();
    const bool cache_load_available =
        !runtime.background_suspended &&
        !runtime.payload_loader.status().active() &&
        !command_engine_status.foreground_active;
    if (runtime.pop(command, cache_load_available)) {
        worked = true;
        switch (command.kind) {
            case ReportTaskCommandKind::Artifact: {
                if (runtime.background_suspended &&
                    command.priority != ReportRequestPriority::Foreground) {
                    break;
                }

                ReportArtifactAvailability available;
                if (runtime.artifact_index &&
                    runtime.artifact_index->availability(
                        command.artifact, available)) {
                    break;
                }

                const ReportRequestEnqueueResult queued =
                    runtime.engine.request(command.artifact,
                                           command.priority,
                                           command.generation);
                if (queued.status == ReportRequestEnqueueStatus::Full ||
                    queued.status == ReportRequestEnqueueStatus::Invalid) {
                    runtime.command_failures++;
                } else if (command.priority ==
                           ReportRequestPriority::Foreground) {
                    worked =
                        runtime.preempt_background_for_foreground() || worked;
                }
                break;
            }

            case ReportTaskCommandKind::CacheArtifact:
                (void)runtime.start_payload_load(
                    command.artifact,
                    command.generation,
                    StorageReadLane::Report);
                break;

            case ReportTaskCommandKind::RefreshCatalog:
                runtime.pending_refresh.generation = command.generation;
                runtime.pending_refresh.current_offset_valid =
                    command.current_offset_valid;
                runtime.pending_refresh.current_offset_minutes =
                    command.current_offset_minutes;
                runtime.pending_refresh.summary_attempted = false;
                runtime.catalog_refresh_retry_at_ms = 0;
                runtime.catalog_refresh_retry_attempt = 0;
                break;

        }
    }

    const bool background_work_blocked =
        runtime.background_work_blocked();

    if (runtime.summary_acquisition.active()) {
        worked = runtime.summary_acquisition.poll() || worked;
    }

    if (runtime.pending_refresh.valid() &&
        !runtime.pending_refresh.summary_attempted) {
        const ReportSummaryAcquisitionStatus summary_status =
            runtime.summary_acquisition.status();
        if (!runtime.summary_acquisition.active() &&
            summary_status.generation ==
                runtime.pending_refresh.generation &&
            (summary_status.state ==
                 ReportSummaryAcquisitionState::Ready ||
             summary_status.state ==
                 ReportSummaryAcquisitionState::Error)) {
            runtime.pending_refresh.summary_attempted = true;
            worked = true;
        }
    }

    if (runtime.catalog_store.active()) {
        worked = runtime.catalog_store.poll() || worked;
    }

    if (runtime.store_purpose != CatalogStorePurpose::None &&
        !runtime.catalog_store.active()) {
        const NightCatalogStoreStatus store_status =
            runtime.catalog_store.status();
        if (store_status.state == NightCatalogStoreState::Ready ||
            store_status.state == NightCatalogStoreState::Error) {
            const CatalogStorePurpose completed = runtime.store_purpose;
            runtime.store_purpose = CatalogStorePurpose::None;

            if (completed == CatalogStorePurpose::Load) {
                runtime.catalog_load_pending = false;
                if (store_status.state == NightCatalogStoreState::Ready) {
                    runtime.durable_catalog_generation =
                        store_status.generation;
                    publish_catalog(runtime.catalog_store.snapshot(),
                                    store_status.generation);
                }
            } else if (store_status.state == NightCatalogStoreState::Ready) {
                runtime.durable_catalog_generation =
                    store_status.generation;
                runtime.pending_catalog_save.reset();
                runtime.catalog_store_retry_at_ms = 0;
                runtime.catalog_store_retry_attempt = 0;
            } else {
                runtime.catalog_store_retry_at_ms =
                    now_ms + next_background_retry_delay(
                                 runtime.catalog_store_retry_attempt);
                advance_background_retry(
                    runtime.catalog_store_retry_attempt);
            }
            worked = true;
        }
    }

    if (!background_work_blocked &&
        runtime.catalog_load_pending &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        deadline_due(now_ms, runtime.catalog_store_retry_at_ms)) {
        const OperationAdmission admitted =
            runtime.catalog_store.request_load(CATALOG_STORE_GENERATION);
        if (admitted == OperationAdmission::Accepted) {
            runtime.store_purpose = CatalogStorePurpose::Load;
            runtime.catalog_store_retry_at_ms = 0;
            runtime.catalog_store_retry_attempt = 0;
            worked = true;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.catalog_load_pending = false;
            runtime.command_failures++;
            worked = true;
        }
    }

    if (runtime.catalog_refresh.active()) {
        worked = runtime.catalog_refresh.poll() || worked;
    }

    if (runtime.refresh_generation != 0 &&
        !runtime.catalog_refresh.active()) {
        const NightCatalogRefreshStatus refresh_status =
            runtime.catalog_refresh.status();
        if (refresh_status.state == NightCatalogRefreshState::Ready ||
            refresh_status.state == NightCatalogRefreshState::Error) {
            if (refresh_status.state == NightCatalogRefreshState::Ready) {
                publish_catalog(runtime.catalog_refresh.snapshot(),
                                runtime.refresh_generation);
                runtime.pending_catalog_save = runtime.catalog;
                runtime.catalog_store_retry_at_ms = 0;
                runtime.catalog_store_retry_attempt = 0;
                runtime.catalog_refresh_retry_at_ms = 0;
                runtime.catalog_refresh_retry_attempt = 0;
            } else if (refresh_status.retryable) {
                if (!runtime.pending_refresh.valid()) {
                    runtime.pending_refresh.generation =
                        runtime.refresh_generation;
                    runtime.pending_refresh.current_offset_valid =
                        runtime.refresh_offset_valid;
                    runtime.pending_refresh.current_offset_minutes =
                        runtime.refresh_offset_minutes;
                    runtime.pending_refresh.summary_attempted = true;
                }
                runtime.catalog_refresh_retry_at_ms =
                    now_ms + next_background_retry_delay(
                                 runtime.catalog_refresh_retry_attempt);
                advance_background_retry(
                    runtime.catalog_refresh_retry_attempt);
                runtime.command_failures++;
            } else {
                runtime.catalog_refresh_retry_at_ms = 0;
                runtime.catalog_refresh_retry_attempt = 0;
                runtime.command_failures++;
            }
            runtime.refresh_generation = 0;
            worked = true;
        }
    }

    if (runtime.artifact_index_refresh.active()) {
        worked = runtime.artifact_index_refresh.poll() || worked;
    }

    if (runtime.artifact_index_refresh_generation != 0 &&
        !runtime.artifact_index_refresh.active()) {
        const ReportArtifactIndexRefreshStatus refresh_status =
            runtime.artifact_index_refresh.status();
        if (refresh_status.state ==
                ReportArtifactIndexRefreshState::Ready ||
            refresh_status.state ==
                ReportArtifactIndexRefreshState::Error) {
            if (refresh_status.state ==
                ReportArtifactIndexRefreshState::Ready) {
                if (!runtime.publish_artifact_index(
                        runtime.artifact_index_refresh.snapshot())) {
                    runtime.command_failures++;
                    runtime.artifact_index_refresh_pending = true;
                } else {
                    runtime.artifact_index_loaded = true;
                    runtime.artifact_index_retry_at_ms = 0;
                    runtime.artifact_index_retry_attempt = 0;
                }
            } else {
                if (!runtime.artifact_index) {
                    runtime.publish_artifact_index(
                        ReportArtifactIndexBuilder::build(nullptr, 0));
                }
                runtime.artifact_index_loaded = true;
                runtime.artifact_index_refresh_pending = true;
                runtime.artifact_index_retry_at_ms =
                    now_ms + next_background_retry_delay(
                                 runtime.artifact_index_retry_attempt);
                advance_background_retry(
                    runtime.artifact_index_retry_attempt);
                runtime.command_failures++;
            }
            runtime.artifact_index_refresh_generation = 0;
            worked = true;
        }
    }

    if (!background_work_blocked &&
        runtime.artifact_index_refresh_pending && runtime.catalog &&
        !runtime.artifact_index_refresh.active() &&
        runtime.artifact_index_refresh_generation == 0 &&
        deadline_due(now_ms, runtime.artifact_index_retry_at_ms)) {
        const OperationAdmission admitted =
            runtime.artifact_index_refresh.request_refresh(
                runtime.catalog, runtime.catalog_generation);
        if (admitted == OperationAdmission::Accepted) {
            runtime.artifact_index_refresh_generation =
                runtime.catalog_generation;
            runtime.artifact_index_refresh_pending = false;
            worked = true;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.artifact_index_refresh_pending = false;
            runtime.artifact_index_loaded = true;
            runtime.command_failures++;
            worked = true;
        }
    }

    if (!runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load &&
        runtime.engine.catalog_update_required()) {
        const std::shared_ptr<const LargeByteBuffer> replacement =
            runtime.engine.fallback_replacement();
        const ReportEngineStatus engine_status = runtime.engine.status();
        char path[AC_STORAGE_PATH_MAX] = {};
        std::shared_ptr<const NightCatalog> updated;

        const char *update_error = nullptr;
        if (!runtime.catalog) {
            update_error = "fallback_catalog_missing";
        } else if (!replacement) {
            update_error = "fallback_replacement_missing";
        } else if (!report_fallback_artifact_path(
                       engine_status.active_request.artifact.sleep_day,
                       path,
                       sizeof(path))) {
            update_error = "fallback_replacement_path_invalid";
        } else {
            updated = NightCatalogBuilder::replace_fallback(
                *runtime.catalog, path, replacement);
            if (!updated) update_error = "fallback_catalog_replace_failed";
        }

        if (!updated) {
            runtime.engine.catalog_update_failed(update_error);
            runtime.command_failures++;
        } else {
            const uint32_t generation =
                next_catalog_generation(runtime.catalog_generation);
            runtime.accept_catalog(std::move(updated), generation);
            runtime.pending_catalog_save = runtime.catalog;
            runtime.catalog_store_retry_at_ms = 0;
            runtime.catalog_store_retry_attempt = 0;
        }
        worked = true;
    }

    if (!background_work_blocked && startup_idle_work_allowed &&
        !runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load &&
        !runtime.engine.catalog_update_required() &&
        runtime.pending_refresh.valid() &&
        !runtime.pending_refresh.summary_attempted &&
        !runtime.summary_acquisition.active()) {
        const OperationAdmission admitted =
            runtime.summary_acquisition.request(
                runtime.pending_refresh.generation);
        if (admitted == OperationAdmission::Accepted) {
            worked = true;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.pending_refresh.summary_attempted = true;
            runtime.command_failures++;
            worked = true;
        }
    }

    if (!background_work_blocked && startup_idle_work_allowed &&
        !runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load &&
        !runtime.engine.catalog_update_required() &&
        runtime.pending_refresh.valid() &&
        runtime.pending_refresh.summary_attempted &&
        !runtime.summary_acquisition.active() &&
        deadline_due(now_ms, runtime.catalog_refresh_retry_at_ms)) {
        const std::shared_ptr<const NightCatalogSummarySnapshot> summary =
            runtime.summary_acquisition.snapshot();
        const OperationAdmission admitted =
            runtime.catalog_refresh.request_refresh(
                summary,
                runtime.pending_refresh.current_offset_valid,
                runtime.pending_refresh.current_offset_minutes,
                runtime.pending_refresh.generation);
        if (admitted == OperationAdmission::Accepted) {
            runtime.refresh_generation = runtime.pending_refresh.generation;
            runtime.refresh_offset_valid =
                runtime.pending_refresh.current_offset_valid;
            runtime.refresh_offset_minutes =
                runtime.pending_refresh.current_offset_minutes;
            runtime.pending_refresh.clear();
            runtime.catalog_refresh_retry_at_ms = 0;
            worked = true;
        } else {
            runtime.catalog_refresh_retry_at_ms =
                now_ms + next_background_retry_delay(
                             runtime.catalog_refresh_retry_attempt);
            advance_background_retry(
                runtime.catalog_refresh_retry_attempt);
            if (admitted == OperationAdmission::Rejected) {
                runtime.command_failures++;
            }
            worked = true;
        }
    }

    if (!background_work_blocked &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        runtime.pending_catalog_save &&
        deadline_due(now_ms, runtime.catalog_store_retry_at_ms)) {
        const OperationAdmission admitted = runtime.catalog_store.request_save(
            runtime.pending_catalog_save,
            runtime.catalog_generation);
        if (admitted == OperationAdmission::Accepted) {
            runtime.store_purpose = CatalogStorePurpose::Save;
            worked = true;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.pending_catalog_save.reset();
            runtime.catalog_store_retry_at_ms = 0;
            runtime.catalog_store_retry_attempt = 0;
            runtime.command_failures++;
            worked = true;
        }
    }

    const bool catalog_stable =
        !runtime.catalog_load_pending &&
        !runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.pending_refresh.valid() &&
        !runtime.summary_acquisition.active() &&
        !runtime.engine.catalog_update_required() &&
        runtime.artifact_index_loaded &&
        !runtime.artifact_index_refresh.active() &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        !runtime.pending_catalog_save;
    if (catalog_stable && !background_work_blocked &&
        startup_idle_work_allowed) {
        worked = runtime.schedule_catalog_work(now_ms) || worked;
        worked = runtime.schedule_legacy_cache_cleanup(now_ms) || worked;
    }

    worked = runtime.engine.poll(now_ms, record_budget) || worked;

    std::shared_ptr<const ReportArtifactBundle> published_bundle =
        runtime.engine.take_published_bundle();
    if (published_bundle) {
        (void)runtime.cache_bundle(published_bundle);
        worked = true;
    }

    ReportArtifactAvailability availability =
        runtime.engine.take_available();
    if (availability.requested_ready()) {
        if (!runtime.merge_availability(availability)) {
            runtime.command_failures++;
        }
        worked = true;
    }

    runtime.observe_engine_completion(now_ms);

    runtime.publish_status();
    return worked;
}

void ReportTask::publish_catalog(
    std::shared_ptr<const NightCatalog> catalog,
    uint32_t generation) {
    if (!runtime_ || !catalog || generation == 0) return;
    runtime_->accept_catalog(std::move(catalog), generation);
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
        const bool worked = step(millis(), 1);
        const ReportTaskState state = control_snapshot().state;
        if (worked) {
            vTaskDelay(pdMS_TO_TICKS(AC_REPORT_TASK_WORK_TICK_MS));
        } else if (state == ReportTaskState::LoadingCatalog ||
                   state == ReportTaskState::IndexingArtifacts ||
                   state == ReportTaskState::RefreshingCatalog ||
                   state == ReportTaskState::Queued ||
                   state == ReportTaskState::LookingUp ||
                   state == ReportTaskState::Building ||
                   state == ReportTaskState::Publishing) {
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
