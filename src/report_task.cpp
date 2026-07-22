#include "report_task.h"

#include <algorithm>
#include <new>
#include <utility>

#include "board_report.h"
#include "night_catalog_builder.h"
#include "report_fallback_artifact.h"
#include "report_night_artifact_builder.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr uint32_t CATALOG_STORE_GENERATION = 1;
constexpr uint32_t CATALOG_STORE_RETRY_MIN_MS = 1000;
constexpr uint32_t CATALOG_STORE_RETRY_MAX_MS = 30000;
constexpr size_t IDLE_QUEUE_LOOKAHEAD = 2;

enum class ReportTaskCommandKind : uint8_t {
    Artifact,
    RefreshCatalog,
    CancelGeneration,
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

    bool valid() const { return generation != 0; }
    void clear() { *this = {}; }
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

uint32_t next_store_retry_delay(uint8_t attempt) {
    uint32_t delay_ms = CATALOG_STORE_RETRY_MIN_MS;
    for (uint8_t i = 0; i < attempt && delay_ms < CATALOG_STORE_RETRY_MAX_MS;
         ++i) {
        delay_ms = std::min(delay_ms * 2, CATALOG_STORE_RETRY_MAX_MS);
    }
    return delay_ms;
}

void advance_store_retry(uint8_t &attempt) {
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

        for (size_t i = 0; i < command_count; ++i) {
            ReportTaskCommand &queued = commands[i];
            if (command.kind == ReportTaskCommandKind::Artifact &&
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
            if (command.kind == ReportTaskCommandKind::CancelGeneration &&
                queued.kind == command.kind &&
                queued.generation == command.generation) {
                unlock();
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

    bool pop(ReportTaskCommand &command) {
        if (!lock()) return false;
        if (command_count == 0) {
            unlock();
            return false;
        }

        command = commands[0];
        for (size_t i = 1; i < command_count; ++i) {
            commands[i - 1] = commands[i];
        }
        commands[--command_count] = {};
        unlock();
        return true;
    }

    void remove_artifact_commands(uint32_t generation) {
        if (!lock()) return;

        size_t write = 0;
        for (size_t read = 0; read < command_count; ++read) {
            if (commands[read].kind == ReportTaskCommandKind::Artifact &&
                commands[read].generation == generation) {
                continue;
            }
            if (write != read) commands[write] = commands[read];
            ++write;
        }
        while (command_count > write) commands[--command_count] = {};
        unlock();
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

    bool schedule_catalog_work() {
        if (!catalog || idle_cursor >= catalog->size()) return false;
        if (engine.status().queued >= IDLE_QUEUE_LOOKAHEAD) return false;

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

    void publish_status() {
        size_t queued = 0;
        uint32_t drops = 0;
        if (lock()) {
            queued = command_count;
            drops = command_drops;
            unlock();
        }

        ReportTaskStatus next;
        next.initialized = initialized;
        next.task_started = task_started;
        next.commands_queued = queued;
        next.catalog_nights = catalog ? catalog->size() : 0;
        next.command_drops = drops;
        next.command_failures = command_failures;
        next.catalog_generation = catalog_generation;
        next.catalog_refresh = catalog_refresh.status();
        next.catalog_store = catalog_store.status();
        next.artifact_index_refresh = artifact_index_refresh.status();
        next.engine = engine.status();

        if (!initialized) {
            next.state = ReportTaskState::Stopped;
        } else if (store_purpose == CatalogStorePurpose::Load ||
                   catalog_load_pending) {
            next.state = ReportTaskState::LoadingCatalog;
        } else if (!artifact_index_loaded &&
                   (artifact_index_refresh.active() ||
                    artifact_index_refresh_pending)) {
            next.state = ReportTaskState::IndexingArtifacts;
        } else if (catalog_refresh.active() || pending_refresh.valid() ||
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

        if (!lock()) return;
        status = next;
        unlock();
    }

    ReportArtifactRequest build_slots[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    ReportEngine engine;
    ReportNightArtifactBuilder builder;
    NightCatalogRefreshService catalog_refresh;
    NightCatalogStoreService catalog_store;
    ReportArtifactIndexRefreshService artifact_index_refresh;

    ReportTaskCommand commands[AC_REPORT_TASK_COMMAND_CAPACITY] = {};
    size_t command_count = 0;
    uint32_t command_drops = 0;
    uint32_t command_failures = 0;
    PendingCatalogRefresh pending_refresh;
    uint32_t refresh_generation = 0;

    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const NightCatalog> published_catalog;
    std::shared_ptr<const NightCatalog> pending_catalog_save;
    std::shared_ptr<const ReportArtifactBundle> published;
    std::shared_ptr<const ReportArtifactBundle> pending_published;
    std::shared_ptr<const ReportArtifactIndex> artifact_index;
    std::shared_ptr<const ReportArtifactIndex> published_artifact_index;
    uint32_t catalog_generation = 0;
    size_t idle_cursor = 0;
    uint32_t idle_generation = 0x80000000u;

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
    ReportTaskStatus status;

#ifdef ARDUINO
    mutable SemaphoreHandle_t mutex = nullptr;
    TaskHandle_t task = nullptr;
#endif
};

ReportTask::~ReportTask() {
    if (!runtime_) return;

#ifdef ARDUINO
    if (runtime_->task) {
        vTaskDelete(runtime_->task);
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
                       ReportSpoolPort &spool_port) {
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
    runtime_->engine.begin(read_port,
                           write_port,
                           spool_port,
                           runtime_->builder);
    runtime_->initialized = true;
    runtime_->publish_status();

#ifdef ARDUINO
    const BaseType_t created = xTaskCreatePinnedToCore(
        task_entry,
        "ac_report",
        AC_REPORT_TASK_STACK,
        this,
        AC_REPORT_TASK_PRIO,
        &runtime_->task,
        AC_REPORT_TASK_CORE);
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

OperationAdmission ReportTask::cancel_generation(uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || generation == 0) {
        return OperationAdmission::Rejected;
    }

    runtime_->remove_artifact_commands(generation);

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::CancelGeneration;
    command.generation = generation;
    return runtime_->enqueue(command);
}

ReportTaskStatus ReportTask::status() const {
    if (!runtime_ || !runtime_->lock(20)) return {};
    const ReportTaskStatus out = runtime_->status;
    runtime_->unlock();
    return out;
}

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

std::shared_ptr<const ReportArtifactBundle> ReportTask::take_published() {
    if (!runtime_ || !runtime_->lock(20)) return {};
    std::shared_ptr<const ReportArtifactBundle> out =
        std::move(runtime_->published);
    runtime_->unlock();
    return out;
}

bool ReportTask::step(uint32_t now_ms, size_t record_budget) {
    if (!runtime_ || !runtime_->initialized) return false;
    Runtime &runtime = *runtime_;
    bool worked = false;

    ReportTaskCommand command;
    if (runtime.pop(command)) {
        worked = true;
        switch (command.kind) {
            case ReportTaskCommandKind::Artifact: {
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
                }
                break;
            }

            case ReportTaskCommandKind::RefreshCatalog:
                runtime.pending_refresh.generation = command.generation;
                runtime.pending_refresh.current_offset_valid =
                    command.current_offset_valid;
                runtime.pending_refresh.current_offset_minutes =
                    command.current_offset_minutes;
                break;

            case ReportTaskCommandKind::CancelGeneration:
                (void)runtime.engine.cancel_generation(command.generation);
                if (runtime.pending_refresh.generation == command.generation) {
                    runtime.pending_refresh.clear();
                }
                if (runtime.refresh_generation == command.generation) {
                    runtime.catalog_refresh.cancel();
                    runtime.refresh_generation = 0;
                }
                break;
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
                    publish_catalog(runtime.catalog_store.snapshot(),
                                    store_status.generation);
                }
            } else if (store_status.state == NightCatalogStoreState::Ready) {
                runtime.pending_catalog_save.reset();
                runtime.catalog_store_retry_at_ms = 0;
                runtime.catalog_store_retry_attempt = 0;
            } else {
                runtime.catalog_store_retry_at_ms =
                    now_ms + next_store_retry_delay(
                                 runtime.catalog_store_retry_attempt);
                advance_store_retry(runtime.catalog_store_retry_attempt);
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
            } else {
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
                    now_ms + next_store_retry_delay(
                                 runtime.artifact_index_retry_attempt);
                advance_store_retry(runtime.artifact_index_retry_attempt);
                runtime.command_failures++;
            }
            runtime.artifact_index_refresh_generation = 0;
            worked = true;
        }
    }

    if (runtime.artifact_index_refresh_pending && runtime.catalog &&
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

    if (!runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load &&
        !runtime.engine.catalog_update_required() &&
        runtime.pending_refresh.valid()) {
        const OperationAdmission admitted =
            runtime.catalog_refresh.request_refresh(
                nullptr,
                0,
                runtime.pending_refresh.current_offset_valid,
                runtime.pending_refresh.current_offset_minutes,
                runtime.pending_refresh.generation);
        if (admitted == OperationAdmission::Accepted) {
            runtime.refresh_generation = runtime.pending_refresh.generation;
            runtime.pending_refresh.clear();
            worked = true;
        } else if (admitted == OperationAdmission::Rejected) {
            runtime.pending_refresh.clear();
            runtime.command_failures++;
            worked = true;
        }
    }

    if (!runtime.catalog_load_pending &&
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
            runtime.catalog_store_retry_at_ms =
                now_ms + next_store_retry_delay(
                             runtime.catalog_store_retry_attempt);
            advance_store_retry(runtime.catalog_store_retry_attempt);
            worked = true;
        }
    }

    const bool catalog_stable =
        !runtime.catalog_load_pending &&
        !runtime.catalog_refresh.active() &&
        runtime.refresh_generation == 0 &&
        !runtime.pending_refresh.valid() &&
        !runtime.engine.catalog_update_required() &&
        runtime.artifact_index_loaded &&
        !runtime.artifact_index_refresh.active() &&
        runtime.store_purpose == CatalogStorePurpose::None &&
        !runtime.pending_catalog_save;
    if (catalog_stable) {
        worked = runtime.schedule_catalog_work() || worked;
    }

    worked = runtime.engine.poll(now_ms, record_budget) || worked;

    ReportArtifactAvailability availability =
        runtime.engine.take_available();
    if (availability.requested_ready()) {
        if (!runtime.merge_availability(availability)) {
            runtime.command_failures++;
        }
        worked = true;
    }

    std::shared_ptr<const ReportArtifactBundle> published =
        runtime.engine.take_published();
    if (published) runtime.pending_published = std::move(published);
    if (runtime.pending_published && runtime.lock()) {
        runtime.published = std::move(runtime.pending_published);
        runtime.unlock();
        worked = true;
    }

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
        const ReportTaskStatus current = status();
        if (worked) {
            vTaskDelay(pdMS_TO_TICKS(AC_REPORT_TASK_WORK_TICK_MS));
        } else if (current.state == ReportTaskState::LoadingCatalog ||
                   current.state == ReportTaskState::RefreshingCatalog ||
                   current.state == ReportTaskState::Queued ||
                   current.state == ReportTaskState::LookingUp ||
                   current.state == ReportTaskState::Building ||
                   current.state == ReportTaskState::Publishing) {
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
