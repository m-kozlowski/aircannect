#include "report_task.h"

#include <algorithm>
#include <atomic>
#include <new>
#include <string.h>
#include <utility>

#include "board_report.h"
#include "display_report_summary.h"
#include "night_catalog_builder.h"
#include "report_artifact_index.h"
#include "report_fallback_artifact.h"
#include "report_night_artifact_builder.h"
#include "report_payload_deflater.h"
#include "report_plot_format.h"
#include "report_spool_availability.h"
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
constexpr uint32_t CATALOG_STORE_RETRY_MIN_MS = 1000;
constexpr uint32_t CATALOG_STORE_RETRY_MAX_MS = 30000;
constexpr uint32_t LEGACY_CACHE_DELETE_RETRY_MS = 30000;
constexpr uint32_t ARTIFACT_FAILURE_RETRY_MS = 30000;
constexpr uint32_t ARTIFACT_FAILURE_RETRY_MAX_MS = 15 * 60 * 1000;
constexpr uint32_t SPOOL_AVAILABILITY_RETRY_MS = 10 * 60 * 1000;
constexpr char LEGACY_CACHE_PARENT[] = "/aircannect/report";
constexpr const char *LEGACY_CACHE_NAMES[] = {
    "v3", "v4", "v5", "v6", "v7",
};

enum class ReportTaskCommandKind : uint8_t {
    Artifact,
    CacheArtifact,
    RefreshCatalog,
};

enum class PayloadLoadStartResult : uint8_t {
    Started,
    AlreadyCached,
    Busy,
    Superseded,
    TooLarge,
    MemoryUnavailable,
    Rejected,
};

enum class PayloadCacheInsertResult : uint8_t {
    Cached,
    Retry,
    Rejected,
};

enum class PlotPayloadResolveResult : uint8_t {
    Ready,
    IndexPending,
    SectionMissing,
    Invalid,
};

struct ReportTaskCommand {
    ReportTaskCommandKind kind = ReportTaskCommandKind::Artifact;
    ReportArtifactKey artifact;
    ReportArtifactPayloadDescriptor payload;
    ReportRequestPriority priority = ReportRequestPriority::Foreground;
    bool force_rebuild = false;
    uint32_t generation = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
    NightCatalogRefreshTarget catalog_target;
};

struct PendingCatalogRefresh {
    uint32_t generation = 0;
    bool current_offset_valid = false;
    int32_t current_offset_minutes = 0;
    bool summary_attempted = false;
    NightCatalogRefreshTarget target;

    bool valid() const { return generation != 0; }
    void clear() { *this = {}; }
};

struct ReportArtifactFailureEntry {
    ReportArtifactKey artifact;
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t retry_at_ms = 0;
    bool retryable = true;

    bool valid() const { return artifact.valid() && error[0] != '\0'; }
};

bool report_artifact_failure_retryable(const char *error) {
    return !error || strcmp(error, "report_source_expired") != 0;
}

struct ReportPublishedState {
    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const ReportArtifactIndex> artifact_index;
    DisplayReportSummary display_summary;
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

bool same_artifact_descriptor(const ReportArtifactDescriptor &lhs,
                              const ReportArtifactDescriptor &rhs) {
    return lhs.key == rhs.key && lhs.size == rhs.size &&
           lhs.crc32 == rhs.crc32 &&
           lhs.prefix_crc32 == rhs.prefix_crc32;
}

uint32_t next_background_retry_delay(uint8_t attempt,
                                     uint32_t minimum_ms,
                                     uint32_t maximum_ms) {
    uint32_t delay_ms = minimum_ms;
    for (uint8_t i = 0; i < attempt && delay_ms < maximum_ms; ++i) {
        delay_ms = std::min(delay_ms * 2, maximum_ms);
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

uint32_t deadline_remaining(uint32_t now_ms, uint32_t deadline_ms) {
    return deadline_due(now_ms, deadline_ms) ? 0 : deadline_ms - now_ms;
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
            if (command.kind == ReportTaskCommandKind::Artifact &&
                queued.kind == command.kind &&
                same_artifact_identity(queued.artifact, command.artifact)) {
                command.force_rebuild =
                    command.force_rebuild || queued.force_rebuild;
                if (report_request_priority_higher(
                        queued.priority, command.priority)) {
                    command.priority = queued.priority;
                }
                queued = command;
                unlock();
                wake();
                return OperationAdmission::Accepted;
            }
            if (command.kind == ReportTaskCommandKind::CacheArtifact &&
                queued.kind == command.kind &&
                queued.payload == command.payload) {
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
            pending_refresh.target = refresh_target;
            pending_refresh.summary_attempted = true;
        }

        catalog_refresh.cancel();
        refresh_generation = 0;
        catalog_refresh_retry_at_ms = 0;
        return true;
    }

    bool preempt_background_work() {
        bool worked = engine.cancel_background() > 0;

        const ReportArtifactPayloadLoadStatus payload_status =
            payload_loader.status();
        if (payload_status.active() &&
            payload_status.lane != StorageReadLane::Foreground) {
            payload_loader.cancel();
            worked = true;
        }

        if (summary_acquisition.active()) {
            summary_acquisition.cancel();
            worked = true;
        }

        if (spool_availability_probe.status().active()) {
            spool_availability_probe.cancel();
            worked = true;
        }

        worked = defer_active_catalog_refresh() || worked;

        return worked;
    }

    bool preempt_background_for_foreground() {
        if (!engine.status().foreground_active) return false;
        return preempt_background_work();
    }

    bool background_work_blocked() const {
        return background_suspended || engine.status().foreground_active;
    }

    void invalidate_spool_availability() {
        spool_availability_needed = true;
        spool_availability_retry_at_ms = 0;
        spool_availability_terminal_handled = false;
        spool_availability_probe.cancel();
        engine.publish_spool_availability({}, false);

        idle_cursor = 0;
        idle_retry_at_ms = 0;
        idle_retry_attempt = 0;
        idle_pass_failed = false;
    }

    bool start_spool_availability_probe(uint32_t now_ms) {
        if (!spool_availability_needed ||
            spool_availability_probe.status().active() ||
            !deadline_due(now_ms, spool_availability_retry_at_ms)) {
            return false;
        }

        const ReportEngineStatus engine_status = engine.status();
        if (engine_status.state != ReportEngineState::Idle ||
            engine_status.queued != 0 || payload_loader.status().active()) {
            return false;
        }

        spool_availability_generation = next_catalog_generation(
            spool_availability_generation);
        const OperationAdmission admitted =
            spool_availability_probe.request(
                spool_availability_generation);
        if (admitted == OperationAdmission::Busy) return false;
        if (admitted == OperationAdmission::Rejected) {
            spool_availability_retry_at_ms =
                now_ms + SPOOL_AVAILABILITY_RETRY_MS;
            command_failures++;
            return true;
        }

        spool_availability_terminal_handled = false;
        spool_availability_retry_at_ms = 0;
        engine.publish_spool_availability({}, false);
        return true;
    }

    bool observe_spool_availability_probe(uint32_t now_ms) {
        const ReportSpoolAvailabilityProbeStatus probe =
            spool_availability_probe.status();
        if (!probe.terminal() || spool_availability_terminal_handled) {
            return false;
        }

        spool_availability_terminal_handled = true;
        engine.publish_spool_availability(
            spool_availability_probe.availability(),
            probe.state == ReportSpoolAvailabilityProbeState::Ready);
        if (probe.state == ReportSpoolAvailabilityProbeState::Ready) {
            spool_availability_needed = false;
            spool_availability_retry_at_ms = 0;
            idle_cursor = 0;
            idle_retry_at_ms = 0;
            idle_retry_attempt = 0;
            idle_pass_failed = false;
            return true;
        }

        spool_availability_needed = true;
        spool_availability_retry_at_ms =
            now_ms + SPOOL_AVAILABILITY_RETRY_MS;
        return true;
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
        const bool therapy_ended =
            activity.therapy_active && !next.therapy_active;
        activity = next;
        background_suspended =
            activity.therapy_active || activity.realtime_stream_active ||
            activity.foreground_report_demand ||
            activity.ota_install_active || activity.export_work_claimed ||
            !activity.as11_rpc_available;
        if (therapy_ended) invalidate_spool_availability();
        if (!background_suspended || was_suspended) return true;

        (void)engine.cancel_background();
        idle_cursor = 0;

        if (summary_acquisition.active()) {
            summary_acquisition.cancel();
        }

        if (spool_availability_probe.status().active()) {
            spool_availability_probe.cancel();
        }

        const ReportArtifactPayloadLoadStatus payload_status =
            payload_loader.status();
        if (payload_status.active() &&
            payload_status.lane != StorageReadLane::Foreground) {
            payload_loader.cancel();
        }

        (void)defer_active_catalog_refresh();

        return true;
    }

#ifndef ARDUINO
    bool find_availability(
        const ReportArtifactKey &artifact,
        ReportArtifactAvailability &out) const {
        out = {};
        if (!artifact.valid()) return false;

        const std::shared_ptr<const ReportPublishedState> published =
            published_state();
        if (!published || !published->catalog) return false;

        const NightCatalogRecord *night = published->catalog
            ? published->catalog->find(artifact.sleep_day)
            : nullptr;
        if (!night || night->source_revision != artifact.source_revision) {
            return false;
        }

        return published->artifact_index &&
            published->artifact_index->availability(artifact, out);
    }
#endif

    ReportArtifactQuery query_artifact(SleepDayId sleep_day,
                                        ReportArtifactKind kind,
                                        int64_t range_start_ms,
                                        int64_t range_end_ms) const {
        ReportArtifactQuery out;
        if (!sleep_day.valid()) return out;

        const std::shared_ptr<const ReportPublishedState> published =
            published_state();
        if (!published || !published->catalog) {
            out.state = ReportArtifactQueryState::CatalogPending;
            return out;
        }

        const NightCatalogRecord *night =
            published->catalog->find(sleep_day);
        if (!night) {
            out.state = ReportArtifactQueryState::NightMissing;
            return out;
        }

        switch (kind) {
            case ReportArtifactKind::Result:
                out.artifact = ReportArtifactKey::result(
                    sleep_day, night->source_revision);
                break;
            case ReportArtifactKind::Overview:
                out.artifact = ReportArtifactKey::overview(
                    sleep_day, night->source_revision);
                break;
            case ReportArtifactKind::RangeTile:
                out.artifact = ReportArtifactKey::range_tile(
                    sleep_day,
                    night->source_revision,
                    range_start_ms,
                    range_end_ms);
                break;
        }

        if (!out.artifact.valid()) {
            out.state = ReportArtifactQueryState::InvalidArtifact;
            return out;
        }

        bool cached_without_pair = false;
        if (lock(0)) {
            const bool cached = payload_cache.describe_ready(
                out.artifact, out.descriptor);
            if (!cached && kind == ReportArtifactKind::RangeTile) {
                cached_without_pair = payload_cache.describe(
                    out.artifact, out.descriptor);
            }
            unlock();
            if (cached) {
                out.state = ReportArtifactQueryState::Ready;
                return out;
            }
        }

        ReportArtifactAvailability availability;
        if (cached_without_pair && published->artifact_index) {
            const ReportArtifactKey result = ReportArtifactKey::result(
                sleep_day, out.artifact.source_revision);
            ReportArtifactAvailability pair;
            if (published->artifact_index->availability(result, pair) &&
                pair.pair_ready()) {
                out.state = ReportArtifactQueryState::Ready;
                return out;
            }
        }

        if (!published->artifact_index ||
            !published->artifact_index->availability(
                out.artifact, availability)) {
            out.descriptor = {};
            out.state = ReportArtifactQueryState::ArtifactMissing;
            return out;
        }
        if (!availability.descriptor(out.artifact, out.descriptor)) {
            out.state = ReportArtifactQueryState::ArtifactIndexInvalid;
            return out;
        }

        out.state = ReportArtifactQueryState::Ready;
        return out;
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
            std::make_shared<ReportPublishedState>(
                ReportPublishedState{
                    catalog, artifact_index, display_summary});

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        std::atomic_store_explicit(
            &published, std::move(next), std::memory_order_release);
#pragma GCC diagnostic pop
        return true;
    }

    std::shared_ptr<const LargeByteBuffer> find_payload(
        const ReportArtifactPayloadDescriptor &payload) {
        if (!payload.valid() || !lock(20)) return {};

        std::shared_ptr<const LargeByteBuffer> out =
            payload_cache.find(payload);
        unlock();
        return out;
    }

    std::shared_ptr<const LargeByteBuffer> find_payload_if_present(
        const ReportArtifactPayloadDescriptor &payload) {
        if (!payload.valid() || !lock(0)) return {};

        std::shared_ptr<const LargeByteBuffer> out =
            payload_cache.find_if_present(payload);
        unlock();
        return out;
    }

    ReportArtifactPayloadSelection select_payload(
        const ReportArtifactPayloadDescriptor &payload,
        bool prefer_deflate) {
        if (!payload.valid() || !lock(20)) return {};

        ReportArtifactPayloadSelection out =
            payload_cache.select(payload, prefer_deflate);
        unlock();
        return out;
    }

    ReportArtifactPayloadSelection select_payload_if_present(
        const ReportArtifactPayloadDescriptor &payload,
        bool prefer_deflate) {
        if (!payload.valid() || !lock(0)) return {};

        ReportArtifactPayloadSelection out =
            payload_cache.select_if_present(payload, prefer_deflate);
        unlock();
        return out;
    }

    PlotPayloadResolveResult resolve_plot_payload(
        const ReportArtifactDescriptor &artifact,
        ReportPayloadKind payload_kind,
        const char *series_name,
        ReportArtifactPayloadDescriptor &out) {
        out = {};
        if (!artifact.valid() ||
            artifact.key.kind == ReportArtifactKind::Result ||
            artifact.size < PLOT_INDEX_PREFIX_BYTES ||
            (payload_kind != ReportPayloadKind::PlotIndex &&
             payload_kind != ReportPayloadKind::PlotEvents &&
             payload_kind != ReportPayloadKind::PlotSeries) ||
            (payload_kind == ReportPayloadKind::PlotSeries &&
             (!series_name || !series_name[0]))) {
            return PlotPayloadResolveResult::Invalid;
        }

        ReportArtifactPayloadDescriptor index;
        index.artifact = artifact;
        index.kind = ReportPayloadKind::PlotIndex;
        index.size = PLOT_INDEX_PREFIX_BYTES;
        index.crc32 = artifact.prefix_crc32;
        if (payload_kind == ReportPayloadKind::PlotIndex) {
            out = index;
            return PlotPayloadResolveResult::Ready;
        }

        const std::shared_ptr<const LargeByteBuffer> prefix =
            find_payload_if_present(index);
        if (!prefix) {
            out = index;
            return PlotPayloadResolveResult::IndexPending;
        }

        ReportPlotIndexView view;
        if (!report_plot_decode_prefix(
                prefix->data(), prefix->size(), view) ||
            view.total_size != artifact.size) {
            return PlotPayloadResolveResult::Invalid;
        }

        ReportPlotSectionDescriptor section;
        const bool found = payload_kind == ReportPayloadKind::PlotEvents
            ? view.find_events(section)
            : view.find_series(series_name, section);
        if (!found) return PlotPayloadResolveResult::SectionMissing;

        out.artifact = artifact;
        out.kind = payload_kind;
        out.offset = section.offset;
        out.size = section.length;
        out.crc32 = section.crc32;
        return out.valid() ? PlotPayloadResolveResult::Ready
                           : PlotPayloadResolveResult::Invalid;
    }

    bool payload_cached(
        const ReportArtifactPayloadDescriptor &payload) const {
        if (!payload.valid() || !lock(20)) return false;

        const bool cached = payload_cache.contains(payload);
        unlock();
        return cached;
    }

    PayloadLoadStartResult prepare_payload_allocation(
        const ReportArtifactPayloadDescriptor &payload) {
        if (!payload.valid()) return PayloadLoadStartResult::Rejected;
        if (!lock(20)) return PayloadLoadStartResult::Busy;

        if (payload_cache.contains(payload)) {
            unlock();
            return PayloadLoadStartResult::AlreadyCached;
        }
        if (!payload_cache.can_hold(payload)) {
            unlock();
            return PayloadLoadStartResult::TooLarge;
        }

#ifdef ARDUINO
        MemoryStatus memory = Memory::status();
        while (memory.psram_available &&
               memory.psram_free <
                   payload.size +
                       AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE &&
               payload_cache.evict_lru()) {
            memory = Memory::status();
        }
        const bool available = memory.psram_available &&
            memory.psram_free >=
                payload.size + AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE;
#else
        const bool available = true;
#endif

        unlock();
        return available ? PayloadLoadStartResult::Started
                         : PayloadLoadStartResult::MemoryUnavailable;
    }

    PayloadCacheInsertResult cache_payload_result(
        const ReportArtifactPayloadDescriptor &payload,
        std::shared_ptr<const LargeByteBuffer> bytes) {
        if (!payload.valid() || !bytes) {
            return PayloadCacheInsertResult::Rejected;
        }
        if (!lock(20)) {
            return PayloadCacheInsertResult::Retry;
        }

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
            return PayloadCacheInsertResult::Rejected;
        }
#endif

        const bool inserted = payload_cache.insert(payload, std::move(bytes));
        unlock();
        return inserted ? PayloadCacheInsertResult::Cached
                        : PayloadCacheInsertResult::Rejected;
    }

    bool cache_payload(const ReportArtifactPayloadDescriptor &payload,
                       std::shared_ptr<const LargeByteBuffer> bytes) {
        return cache_payload_result(payload, std::move(bytes)) ==
            PayloadCacheInsertResult::Cached;
    }

    PayloadCacheInsertResult cache_bundle(
        const std::shared_ptr<const ReportArtifactBundle> &bundle) {
        if (!bundle || !bundle->valid()) {
            return PayloadCacheInsertResult::Rejected;
        }

        if (bundle->key.kind == ReportArtifactKind::Result) {
            ReportArtifactDescriptor result;
            result.key = ReportArtifactKey::result(
                bundle->key.sleep_day, bundle->key.source_revision);
            result.size = bundle->result->size();
            result.crc32 = bundle->result_crc32;
            ReportArtifactDescriptor overview;
            overview.key = ReportArtifactKey::overview(
                bundle->key.sleep_day, bundle->key.source_revision);
            overview.size = bundle->overview->size();
            overview.crc32 = bundle->overview_crc32;
            overview.prefix_crc32 = bundle->overview_prefix_crc32;
            if (!lock(20)) return PayloadCacheInsertResult::Retry;

#ifdef ARDUINO
            MemoryStatus memory = Memory::status();
            while (memory.psram_available &&
                   memory.psram_free <
                       AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE &&
                   payload_cache.evict_lru()) {
                memory = Memory::status();
            }
            if (!memory.psram_available ||
                memory.psram_free <
                    AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE) {
                unlock();
                return PayloadCacheInsertResult::Rejected;
            }
#endif

            const bool cached = payload_cache.insert_pair(
                result, bundle->result, overview, bundle->overview);
            unlock();
            return cached ? PayloadCacheInsertResult::Cached
                          : PayloadCacheInsertResult::Rejected;
        }

        if (bundle->key.kind == ReportArtifactKind::RangeTile) {
            ReportArtifactDescriptor tile;
            tile.key = bundle->key;
            tile.size = bundle->range_tile->size();
            tile.crc32 = bundle->range_tile_crc32;
            tile.prefix_crc32 = bundle->range_tile_prefix_crc32;
            return cache_payload_result(
                ReportArtifactPayloadDescriptor::whole(tile),
                bundle->range_tile);
        }
        return PayloadCacheInsertResult::Rejected;
    }

    bool finish_built_bundle_cache() {
        if (!pending_built_bundle) return false;

        const PayloadCacheInsertResult result =
            cache_bundle(pending_built_bundle);
        if (result == PayloadCacheInsertResult::Retry) return false;

        if (result == PayloadCacheInsertResult::Rejected) {
#ifdef ARDUINO
            char day[9] = {};
            pending_built_bundle->key.sleep_day.format_yyyymmdd(
                day, sizeof(day));

            Log::logf(CAT_REPORT,
                      LOG_WARN,
                      "rebuilt payload cache unavailable night=%s",
                      day[0] ? day : "invalid");
#endif
        }

        pending_built_bundle.reset();
        return true;
    }

    PayloadLoadStartResult start_exact_payload_load(
        const ReportArtifactPayloadDescriptor &payload,
        uint32_t generation,
        StorageReadLane lane) {
        if (payload_loader.status().active()) {
            return PayloadLoadStartResult::Busy;
        }

        ReportArtifactAvailability availability;
        ReportArtifactDescriptor current;
        if (!artifact_index ||
            !artifact_index->availability(
                payload.artifact.key, availability) ||
            !availability.descriptor(payload.artifact.key, current) ||
            !same_artifact_descriptor(payload.artifact, current)) {
            return PayloadLoadStartResult::Superseded;
        }

        const PayloadLoadStartResult prepared =
            prepare_payload_allocation(payload);
        if (prepared != PayloadLoadStartResult::Started) return prepared;

        const OperationAdmission admitted =
            payload_loader.start(payload, generation, lane);
        if (admitted == OperationAdmission::Accepted) {
            return PayloadLoadStartResult::Started;
        }
        return admitted == OperationAdmission::Busy
            ? PayloadLoadStartResult::Busy
            : PayloadLoadStartResult::Rejected;
    }

    PayloadLoadStartResult start_payload_load(
        const ReportArtifactPayloadDescriptor &payload,
        uint32_t generation,
        StorageReadLane lane) {
        if (payload.kind == ReportPayloadKind::PlotIndex) {
            const ReportArtifactPayloadDescriptor whole =
                ReportArtifactPayloadDescriptor::whole(payload.artifact);
            const PayloadLoadStartResult promoted =
                start_exact_payload_load(whole, generation, lane);
            if (promoted != PayloadLoadStartResult::TooLarge &&
                promoted != PayloadLoadStartResult::MemoryUnavailable &&
                promoted != PayloadLoadStartResult::Rejected) {
                return promoted;
            }
        }

        return start_exact_payload_load(payload, generation, lane);
    }

    PayloadLoadStartResult start_payload_load(
        const ReportArtifactKey &artifact,
        uint32_t generation,
        StorageReadLane lane) {
        ReportArtifactAvailability availability;
        if (!artifact_index ||
            !artifact_index->availability(artifact, availability)) {
            return PayloadLoadStartResult::Superseded;
        }

        ReportArtifactDescriptor descriptor;
        if (!availability.descriptor(artifact, descriptor)) {
            return PayloadLoadStartResult::Superseded;
        }
        return start_exact_payload_load(
            ReportArtifactPayloadDescriptor::whole(descriptor),
            generation,
            lane);
    }

    bool finish_payload_load(uint32_t now_ms) {
        const ReportArtifactPayloadLoadStatus load_status =
            payload_loader.status();
        if (!load_status.terminal()) return false;

        if (load_status.state == ReportArtifactPayloadLoadState::Ready) {
            std::shared_ptr<const LargeByteBuffer> bytes =
                payload_loader.take_completed();
            const bool cached = bytes && cache_payload(
                load_status.payload, std::move(bytes));
            if (lock(20)) {
                if (cached) {
                    payload_load_failed = {};
                    payload_load_retry_at_ms = 0;
                    payload_load_error[0] = '\0';
                    clear_artifact_failure_locked(
                        load_status.payload.artifact.key);
                } else {
                    payload_load_failed = load_status.payload;
                    payload_load_retry_at_ms =
                        now_ms + ARTIFACT_FAILURE_RETRY_MS;
                    copy_cstr(payload_load_error,
                              sizeof(payload_load_error),
                              "report_payload_cache_failed");
                    if (load_status.payload.is_whole()) {
                        remember_artifact_failure_locked(
                            load_status.payload.artifact.key,
                            payload_load_error,
                            now_ms);
                    }
                }
                unlock();
            }
            return true;
        }

        if (load_status.state == ReportArtifactPayloadLoadState::Error) {
            if (lock(20)) {
                payload_load_failed = load_status.payload;
                payload_load_retry_at_ms =
                    now_ms + ARTIFACT_FAILURE_RETRY_MS;
                copy_cstr(payload_load_error,
                          sizeof(payload_load_error),
                          load_status.error[0]
                              ? load_status.error
                              : "report_payload_load_failed");
                if (load_status.payload.is_whole()) {
                    remember_artifact_failure_locked(
                        load_status.payload.artifact.key,
                        payload_load_error,
                        now_ms);
                }
                unlock();
            }
        }
        payload_loader.reset();
        return true;
    }

    bool start_pending_deflate() {
        if (payload_deflater.active() || payload_deflater.finished()) {
            return false;
        }

        ReportArtifactPayloadDescriptor payload;
        std::shared_ptr<const LargeByteBuffer> bytes;
        if (!lock(20)) return false;
        const bool pending =
            payload_cache.next_deflate_candidate(payload, bytes);
        unlock();
        if (!pending) return false;

        if (payload_deflater.start(
                payload,
                std::move(bytes),
                AC_REPORT_PAYLOAD_CACHE_PSRAM_RESERVE)) {
            return true;
        }

        if (!lock(20)) return false;
        (void)payload_cache.complete_deflate(payload, {});
        unlock();
        return true;
    }

    bool finish_payload_deflate() {
        if (!payload_deflater.finished()) return false;
        if (!lock(20)) return false;

        const ReportArtifactPayloadDescriptor payload =
            payload_deflater.payload();
        std::shared_ptr<const LargeByteBuffer> bytes =
            payload_deflater.take_completed();
        (void)payload_cache.complete_deflate(
            payload, std::move(bytes));
        unlock();

        payload_deflater.reset();
        return true;
    }

    bool payload_load_suppressed(
        const ReportArtifactPayloadDescriptor &payload,
        uint32_t now_ms) const {
        return payload_load_failed == payload &&
               !deadline_due(now_ms, payload_load_retry_at_ms);
    }

    bool find_payload_failure(
        const ReportArtifactPayloadDescriptor &payload,
        ReportArtifactFailureStatus &out,
        uint32_t lock_timeout_ms = 20) const {
        out = {};
        if (!payload.valid() || !lock(lock_timeout_ms)) return false;

        if (!(payload_load_failed == payload) ||
            deadline_due(last_step_ms, payload_load_retry_at_ms)) {
            unlock();
            return false;
        }

        copy_cstr(out.error, sizeof(out.error), payload_load_error);
        out.retry_after_ms = payload_load_retry_at_ms - last_step_ms;
        unlock();
        return out.valid();
    }

    bool find_failure(const ReportArtifactKey &artifact,
                      ReportArtifactFailureStatus &out,
                      uint32_t lock_timeout_ms = 20) const {
        out = {};
        if (!artifact.valid() || !lock(lock_timeout_ms)) return false;

        for (const ReportArtifactFailureEntry &entry : artifact_failures) {
            if (!entry.valid() || entry.artifact != artifact ||
                (entry.retryable &&
                 deadline_due(last_step_ms, entry.retry_at_ms))) {
                continue;
            }

            copy_cstr(out.error, sizeof(out.error), entry.error);
            out.retryable = entry.retryable;
            out.retry_after_ms = entry.retryable
                ? entry.retry_at_ms - last_step_ms
                : 0;
            unlock();
            return true;
        }

        unlock();
        return false;
    }

    bool publish_artifact_index(
        std::shared_ptr<const ReportArtifactIndex> next) {
        if (!next) return false;

        artifact_index = std::move(next);
        return publish_state();
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

    size_t idle_catalog_limit() const {
        return catalog ? catalog->size() : 0;
    }

    size_t idle_warm_limit() const {
        return catalog
            ? std::min(catalog->size(), AC_REPORT_IDLE_WARM_NIGHTS)
            : 0;
    }

    void observe_engine_completion(uint32_t now_ms) {
        const ReportEngineCompletion completion =
            engine.status().last_completion;
        if (!completion.valid() ||
            completion.request.ticket == observed_engine_completion) {
            return;
        }
        if (pending_built_bundle) return;

        if (!lock(20)) return;

        observed_engine_completion = completion.request.ticket;
        if (completion.request.force_rebuild) {
#ifdef ARDUINO
            char day[9] = {};
            completion.request.artifact.sleep_day.format_yyyymmdd(
                day, sizeof(day));

            if (completion.outcome.disposition ==
                OperationDisposition::Succeeded) {
                Log::logf(CAT_REPORT,
                          LOG_INFO,
                          "rebuild complete night=%s",
                          day[0] ? day : "invalid");
            } else {
                Log::logf(CAT_REPORT,
                          LOG_WARN,
                          "rebuild failed night=%s error=%s",
                          day[0] ? day : "invalid",
                          completion.error[0]
                              ? completion.error
                              : "report_build_failed");
            }
#endif
        }
        if (completion.request.priority !=
                ReportRequestPriority::Foreground) {
            if (strcmp(completion.error,
                       "report_spool_availability_pending") == 0 &&
                catalog && idle_cursor < idle_catalog_limit()) {
                const NightCatalogRecord *night = catalog->record(idle_cursor);
                if (night &&
                    night->sleep_day ==
                        completion.request.artifact.sleep_day &&
                    night->source_revision ==
                        completion.request.artifact.source_revision) {
                    idle_cursor++;
                    if (spool_availability_probe.status().state ==
                        ReportSpoolAvailabilityProbeState::Incomplete) {
                        idle_pass_failed = true;
                        idle_retry_at_ms =
                            spool_availability_retry_at_ms;
                    }
                }
                unlock();
                return;
            }

            if (completion.outcome.disposition ==
                    OperationDisposition::Failed &&
                catalog && idle_cursor < idle_catalog_limit()) {
                const NightCatalogRecord *night = catalog->record(idle_cursor);
                if (night &&
                    night->sleep_day ==
                        completion.request.artifact.sleep_day &&
                    night->source_revision ==
                        completion.request.artifact.source_revision) {
                    const ReportSourceDef *fallback_source =
                        report_source_def(completion.fallback_source);
                    const bool periodic_stall =
                        strcmp(completion.error, "fragment_timeout") == 0 &&
                        fallback_source && fallback_source->spool_type &&
                        report_source_is_sampled(*fallback_source);
                    idle_cursor = periodic_stall
                        ? idle_catalog_limit()
                        : idle_cursor + 1;
                    idle_pass_failed = true;
                    if (idle_retry_at_ms == 0) {
                        idle_retry_at_ms = now_ms +
                            next_background_retry_delay(
                                idle_retry_attempt,
                                ARTIFACT_FAILURE_RETRY_MS,
                                ARTIFACT_FAILURE_RETRY_MAX_MS);
                        advance_background_retry(idle_retry_attempt);
                    }
                }
            }
            unlock();
            return;
        }

        if (completion.outcome.disposition ==
            OperationDisposition::Succeeded) {
            clear_artifact_failure_locked(completion.request.artifact);
            unlock();
            return;
        }
        if (completion.outcome.disposition != OperationDisposition::Failed) {
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
        entry.retryable = report_artifact_failure_retryable(error);
        entry.retry_at_ms = entry.retryable
            ? now_ms + ARTIFACT_FAILURE_RETRY_MS
            : 0;
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
        payload_load_failed = {};
        payload_load_retry_at_ms = 0;
        payload_load_error[0] = '\0';
        unlock();
    }

    void accept_catalog(std::shared_ptr<const NightCatalog> next,
                        uint32_t generation) {
        catalog = std::move(next);
        display_summary = build_display_report_summary(*catalog);
        catalog_generation = generation;
        engine.publish_catalog(catalog);
        clear_artifact_failures();

        if (payload_loader.status().active()) {
            const ReportArtifactDescriptor loading =
                payload_loader.status().payload.artifact;
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

        std::shared_ptr<const ReportArtifactIndex> reconciled = artifact_index
            ? ReportArtifactIndexBuilder::reconcile(*artifact_index, *catalog)
            : ReportArtifactIndexBuilder::build(nullptr, 0);
        if (!reconciled) {
            command_failures++;
        } else {
            artifact_index = std::move(reconciled);
        }

        idle_cursor = 0;
        idle_retry_at_ms = 0;
        idle_retry_attempt = 0;
        idle_pass_failed = false;
        idle_generation = (idle_generation + 1) | 0x80000000u;

        if (!publish_state()) command_failures++;
    }

    bool schedule_catalog_work(uint32_t now_ms) {
        if (!catalog) return false;
        if (idle_cursor >= idle_catalog_limit()) {
            if (!idle_pass_failed) {
                idle_retry_at_ms = 0;
                idle_retry_attempt = 0;
                return false;
            }
            if (!deadline_due(now_ms, idle_retry_at_ms)) return false;

            idle_cursor = 0;
            idle_retry_at_ms = 0;
            idle_pass_failed = false;
            return true;
        }

        const ReportEngineStatus engine_status = engine.status();
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
            if (idle_cursor < idle_warm_limit() &&
                payload_warm_available) {
                const ReportArtifactDescriptor candidates[] = {
                    available.result,
                    available.overview,
                };
                for (const ReportArtifactDescriptor &candidate : candidates) {
                    const ReportArtifactPayloadDescriptor payload =
                        ReportArtifactPayloadDescriptor::whole(candidate);
                    if (!candidate.valid() || payload_cached(payload) ||
                        payload_load_suppressed(payload, now_ms)) {
                        continue;
                    }

                    const PayloadLoadStartResult started = start_payload_load(
                        candidate.key,
                        idle_generation,
                        StorageReadLane::Maintenance);
                    if (started == PayloadLoadStartResult::Started) return true;
                    if (started == PayloadLoadStartResult::Busy) return false;
                }
            }

            idle_cursor++;
            return true;
        }

        if ((night->source_flags &
             NIGHT_CATALOG_SOURCE_SUMMARY_EXPIRED) != 0) {
            idle_cursor++;
            return true;
        }

        if (engine_status.state != ReportEngineState::Idle ||
            engine_status.queued != 0) {
            return false;
        }

        const ReportRequestEnqueueResult queued = engine.request(
            result,
            priority,
            idle_generation);
        if (queued.status == ReportRequestEnqueueStatus::Full) return false;

        if (queued.status == ReportRequestEnqueueStatus::Invalid) {
            idle_cursor++;
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
            idle_cursor < idle_catalog_limit() ||
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

    ReportTaskOperationalSnapshot build_operational_status(
        const ReportTaskStatus &current) const {
        ReportTaskOperationalSnapshot out;
        out.catalog_nights = current.catalog_nights;

        if (!current.initialized) return out;

        const ReportEngineStatus &engine_status = current.engine;
        const ReportArtifactPayloadLoadStatus &payload_status =
            current.payload_load;

        out.condition = ReportTaskCondition::Working;
        if (catalog_store.active()) {
            out.operation = store_purpose == CatalogStorePurpose::Save
                ? ReportTaskOperation::SavingCatalog
                : ReportTaskOperation::LoadingCatalog;
            return out;
        }

        if (summary_acquisition.active() || catalog_refresh.active()) {
            out.operation = ReportTaskOperation::RefreshingCatalog;
            out.sleep_day = refresh_target.sleep_day;
            return out;
        }

        switch (engine_status.state) {
            case ReportEngineState::LookingUp:
                out.operation = ReportTaskOperation::LookingUp;
                out.sleep_day = engine_status.active_request.artifact.sleep_day;
                return out;
            case ReportEngineState::AcquiringFallback:
            case ReportEngineState::Executing:
                out.operation = ReportTaskOperation::Building;
                out.sleep_day = engine_status.active_request.artifact.sleep_day;
                return out;
            case ReportEngineState::Publishing:
                out.operation = ReportTaskOperation::Publishing;
                out.sleep_day = engine_status.active_request.artifact.sleep_day;
                return out;
            case ReportEngineState::Idle:
            case ReportEngineState::Queued:
            case ReportEngineState::WaitingForCatalog:
                break;
        }

        if (payload_status.active()) {
            out.operation = ReportTaskOperation::LoadingPayload;
            out.sleep_day = payload_status.payload.artifact.key.sleep_day;
            return out;
        }

        if (payload_deflater.active()) {
            out.operation = ReportTaskOperation::CompressingPayload;
            out.sleep_day = payload_deflater.payload().artifact.key.sleep_day;
            return out;
        }

        if (spool_availability_probe.status().active()) {
            out.operation = ReportTaskOperation::CheckingSpools;
            return out;
        }

        out.condition = ReportTaskCondition::Waiting;
        if (engine_status.state == ReportEngineState::WaitingForCatalog) {
            out.wait_reason = ReportTaskWaitReason::Catalog;
            out.sleep_day = engine_status.active_request.artifact.sleep_day;
            return out;
        }
        if (current.commands_queued || engine_status.queued ||
            engine_status.state == ReportEngineState::Queued) {
            out.wait_reason = ReportTaskWaitReason::Queue;
            return out;
        }

        out.wait_reason = activity_wait_reason();
        if (out.wait_reason != ReportTaskWaitReason::None) return out;

        if (idle_pass_failed && idle_retry_at_ms != 0) {
            out.wait_reason = ReportTaskWaitReason::Retry;
            out.retry_in_ms = deadline_remaining(last_step_ms,
                                                 idle_retry_at_ms);
            out.sleep_day =
                engine_status.last_completion.request.artifact.sleep_day;
            copy_cstr(out.error, sizeof(out.error),
                      engine_status.last_completion.error);
            return out;
        }

        const ReportSpoolAvailabilityProbeStatus probe =
            spool_availability_probe.status();
        if (spool_availability_needed &&
            spool_availability_retry_at_ms != 0) {
            out.wait_reason = ReportTaskWaitReason::Retry;
            out.retry_in_ms = deadline_remaining(
                last_step_ms, spool_availability_retry_at_ms);
            copy_cstr(out.error, sizeof(out.error), probe.error);
            return out;
        }

        if ((pending_refresh.valid() || refresh_generation != 0) &&
            catalog_refresh_retry_at_ms != 0) {
            out.wait_reason = ReportTaskWaitReason::Retry;
            out.retry_in_ms = deadline_remaining(
                last_step_ms, catalog_refresh_retry_at_ms);
            out.sleep_day = refresh_target.sleep_day;
            copy_cstr(out.error, sizeof(out.error),
                      current.catalog_refresh.error);
            return out;
        }

        if ((catalog_load_pending || pending_catalog_save) &&
            catalog_store_retry_at_ms != 0) {
            out.wait_reason = ReportTaskWaitReason::Retry;
            out.retry_in_ms = deadline_remaining(
                last_step_ms, catalog_store_retry_at_ms);
            copy_cstr(out.error, sizeof(out.error),
                      current.catalog_store.error);
            return out;
        }

        if (payload_load_error[0]) {
            out.condition = ReportTaskCondition::Failed;
            out.wait_reason = ReportTaskWaitReason::None;
            out.sleep_day = payload_load_failed.artifact.key.sleep_day;
            copy_cstr(out.error, sizeof(out.error), payload_load_error);
            return out;
        }

        const ReportEngineCompletion &completion =
            engine_status.last_completion;
        if (completion.valid() &&
            completion.request.priority == ReportRequestPriority::Foreground &&
            completion.outcome.disposition == OperationDisposition::Failed) {
            out.condition = ReportTaskCondition::Failed;
            out.wait_reason = ReportTaskWaitReason::None;
            out.sleep_day = completion.request.artifact.sleep_day;
            copy_cstr(out.error, sizeof(out.error), completion.error);
            return out;
        }

        if (current.catalog_refresh.state ==
                NightCatalogRefreshState::Error &&
            !current.catalog_refresh.retryable) {
            out.condition = ReportTaskCondition::Failed;
            out.wait_reason = ReportTaskWaitReason::None;
            out.sleep_day = refresh_target.sleep_day;
            copy_cstr(out.error, sizeof(out.error),
                      current.catalog_refresh.error);
            return out;
        }

        if (!startup_idle_grace_complete) {
            out.wait_reason = ReportTaskWaitReason::Startup;
            out.retry_in_ms = deadline_remaining(
                last_step_ms, AC_RUNTIME_STARTUP_IDLE_GRACE_MS);
            return out;
        }

        if (catalog_load_pending) {
            out.condition = ReportTaskCondition::Working;
            out.operation = ReportTaskOperation::LoadingCatalog;
            return out;
        }

        if (pending_refresh.valid() || refresh_generation != 0 ||
            engine.catalog_update_required()) {
            out.condition = ReportTaskCondition::Working;
            out.operation = ReportTaskOperation::RefreshingCatalog;
            out.sleep_day = pending_refresh.valid()
                ? pending_refresh.target.sleep_day
                : refresh_target.sleep_day;
            return out;
        }

        if (pending_catalog_save) {
            out.condition = ReportTaskCondition::Working;
            out.operation = ReportTaskOperation::SavingCatalog;
            return out;
        }

        if (spool_availability_needed) {
            out.condition = ReportTaskCondition::Working;
            out.operation = ReportTaskOperation::CheckingSpools;
            return out;
        }

        if (catalog && idle_cursor < idle_catalog_limit()) {
            out.condition = ReportTaskCondition::Working;
            out.operation = ReportTaskOperation::Reconciling;
            const NightCatalogRecord *night = catalog->record(idle_cursor);
            if (night) out.sleep_day = night->sleep_day;
            return out;
        }

        out.condition = ReportTaskCondition::Complete;
        out.wait_reason = ReportTaskWaitReason::None;
        return out;
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
                if ((commands[i].kind == ReportTaskCommandKind::Artifact &&
                     commands[i].priority ==
                         ReportRequestPriority::Foreground) ||
                    commands[i].kind ==
                        ReportTaskCommandKind::CacheArtifact) {
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
        next.payload_cache = payload_cache_status;
        next.payload_load = payload_loader.status();
        next.engine = engine.status();
        if (pending_built_bundle) {
            next.engine.last_completion = {};
        }
        next.foreground_active =
            foreground_command || next.engine.foreground_active ||
            (next.payload_load.active() &&
             next.payload_load.lane == StorageReadLane::Foreground);

        if (!initialized) {
            next.state = ReportTaskState::Stopped;
        } else if (store_purpose == CatalogStorePurpose::Load ||
                   catalog_load_pending) {
            next.state = ReportTaskState::LoadingCatalog;
        } else if (summary_acquisition.active() ||
                   catalog_refresh.active() || pending_refresh.valid() ||
                   refresh_generation != 0 ||
                   engine.catalog_update_required()) {
            next.state = ReportTaskState::RefreshingCatalog;
        } else if (pending_built_bundle) {
            next.state = ReportTaskState::Publishing;
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
            static_cast<bool>(pending_built_bundle) ||
            spool_availability_probe.status().active() ||
            next.payload_load.active() || payload_deflater.active() ||
            (next.state != ReportTaskState::Stopped &&
             next.state != ReportTaskState::Idle);
        next.operational = build_operational_status(next);

        if (!lock()) return;
        status = next;
        unlock();
    }

    ReportArtifactRequest build_slots[AC_REPORT_TASK_BUILD_CAPACITY] = {};
    ReportEngine engine;
    ReportArtifactPayloadCache payload_cache;
    ReportArtifactPayloadLoader payload_loader;
    ReportPayloadDeflater payload_deflater;
    std::shared_ptr<const ReportArtifactBundle> pending_built_bundle;
    ReportNightArtifactBuilder builder;
    ReportSummaryAcquisition summary_acquisition;
    ReportSpoolAvailabilityProbe spool_availability_probe;
    NightCatalogRefreshService catalog_refresh;
    NightCatalogStoreService catalog_store;
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
    ReportArtifactPayloadDescriptor payload_load_failed;
    uint32_t payload_load_retry_at_ms = 0;
    char payload_load_error[AC_STORAGE_ERROR_MAX] = {};
    PendingCatalogRefresh pending_refresh;
    uint32_t refresh_generation = 0;
    uint32_t catalog_refresh_retry_at_ms = 0;
    uint8_t catalog_refresh_retry_attempt = 0;

    std::shared_ptr<const NightCatalog> catalog;
    DisplayReportSummary display_summary;
    std::shared_ptr<const NightCatalog> pending_catalog_save;
    std::shared_ptr<const ReportArtifactIndex> artifact_index;
    std::shared_ptr<const ReportPublishedState> published;
    uint32_t catalog_generation = 0;
    uint32_t durable_catalog_generation = 0;
    size_t idle_cursor = 0;
    uint32_t idle_generation = 0x80000000u;
    uint32_t idle_retry_at_ms = 0;
    uint8_t idle_retry_attempt = 0;
    bool idle_pass_failed = false;
    uint32_t spool_availability_generation = 0;
    uint32_t spool_availability_retry_at_ms = 0;
    bool spool_availability_needed = true;
    bool spool_availability_terminal_handled = false;
    uint32_t legacy_cleanup_retry_at_ms = 0;
    bool legacy_cleanup_pending = true;

    ActivitySnapshot activity;
    ActivitySnapshot pending_activity;
    bool activity_pending = false;
    bool background_suspended = false;
    bool startup_idle_grace_complete = false;

    bool refresh_offset_valid = false;
    int32_t refresh_offset_minutes = 0;
    NightCatalogRefreshTarget refresh_target;

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
    runtime_->summary_acquisition.begin(spool_port);
    runtime_->spool_availability_probe.begin(spool_port);
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
    uint32_t generation,
    bool force_rebuild) {
    if (!runtime_ || !runtime_->initialized || !artifact.valid() ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::Artifact;
    command.artifact = artifact;
    command.priority = priority;
    command.force_rebuild = force_rebuild;
    command.generation = generation;
    return runtime_->enqueue(command);
}

OperationAdmission ReportTask::request_payload_cache(
    const ReportArtifactPayloadDescriptor &payload,
    uint32_t generation) {
    if (!runtime_ || !runtime_->initialized || !payload.valid() ||
        generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::CacheArtifact;
    command.artifact = payload.artifact.key;
    command.payload = payload;
    command.priority = ReportRequestPriority::Foreground;
    command.generation = generation;
    return runtime_->enqueue(command);
}

OperationAdmission ReportTask::request_payload_cache(
    const ReportArtifactKey &artifact,
    uint32_t generation) {
    const ReportArtifactQuery query = query_artifact(
        artifact.sleep_day,
        artifact.kind,
        artifact.range_start_ms,
        artifact.range_end_ms);
    if (query.state != ReportArtifactQueryState::Ready ||
        query.artifact.source_revision != artifact.source_revision) {
        return OperationAdmission::Rejected;
    }
    return request_payload_cache(
        ReportArtifactPayloadDescriptor::whole(query.descriptor),
        generation);
}

OperationAdmission ReportTask::request_catalog_refresh(
    bool current_offset_valid,
    int32_t current_offset_minutes,
    uint32_t generation,
    const NightCatalogRefreshTarget &target) {
    if (!runtime_ || !runtime_->initialized || generation == 0) {
        return OperationAdmission::Rejected;
    }

    ReportTaskCommand command;
    command.kind = ReportTaskCommandKind::RefreshCatalog;
    command.generation = generation;
    command.current_offset_valid = current_offset_valid;
    command.current_offset_minutes = current_offset_minutes;
    command.catalog_target = target;
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
    out.catalog_refresh_state = runtime_->status.catalog_refresh.state;
    out.catalog_refresh_generation =
        runtime_->status.catalog_refresh.generation;
    out.catalog_refresh_retryable =
        runtime_->status.catalog_refresh.retryable;

    runtime_->unlock();
    return out;
}

ReportTaskOperationalSnapshot ReportTask::operational_snapshot() const {
    if (!runtime_ || !runtime_->lock(20)) return {};

    const ReportTaskOperationalSnapshot out = runtime_->status.operational;
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
    out.engine_sleep_day = engine.active_request.artifact.sleep_day;
    out.executor_state = engine.executor.state;
    out.executor_operation_index = engine.executor.operation_index;
    out.executor_operation_count = engine.executor.operation_count;
    out.executor_record_index = engine.executor.record_index;
    out.executor_record_count = engine.executor.record_count;
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

ReportEngineCompletion ReportTask::last_artifact_completion() const {
    if (!runtime_ || !runtime_->lock(20)) return {};

    const ReportEngineCompletion out =
        runtime_->status.engine.last_completion;
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
    if (!runtime_) return {};

    const std::shared_ptr<const ReportPublishedState> published =
        runtime_->published_state();
    return published ? published->catalog : nullptr;
}

DisplayReportSummary ReportTask::display_summary_snapshot() const {
    if (!runtime_) return {};

    const std::shared_ptr<const ReportPublishedState> published =
        runtime_->published_state();
    return published ? published->display_summary : DisplayReportSummary{};
}

ReportArtifactQuery ReportTask::query_artifact(
    SleepDayId sleep_day,
    ReportArtifactKind kind,
    int64_t range_start_ms,
    int64_t range_end_ms) const {
    if (!runtime_ || !runtime_->initialized) return {};

    return runtime_->query_artifact(
        sleep_day, kind, range_start_ms, range_end_ms);
}

ReportPlotPayloadQuery ReportTask::query_plot_payload(
    SleepDayId sleep_day,
    ReportArtifactKind kind,
    ReportPayloadKind payload_kind,
    const char *series_name,
    int64_t range_start_ms,
    int64_t range_end_ms) const {
    ReportPlotPayloadQuery out;
    const ReportArtifactQuery artifact = query_artifact(
        sleep_day, kind, range_start_ms, range_end_ms);
    out.state = artifact.state;
    out.artifact = artifact.artifact;
    if (artifact.state != ReportArtifactQueryState::Ready || !runtime_) {
        return out;
    }

    const PlotPayloadResolveResult resolved =
        runtime_->resolve_plot_payload(
            artifact.descriptor, payload_kind, series_name, out.payload);
    switch (resolved) {
        case PlotPayloadResolveResult::Ready:
            out.state = ReportArtifactQueryState::Ready;
            break;
        case PlotPayloadResolveResult::IndexPending:
            out.state = ReportArtifactQueryState::PlotIndexPending;
            break;
        case PlotPayloadResolveResult::SectionMissing:
            out.state = ReportArtifactQueryState::PlotSectionMissing;
            break;
        case PlotPayloadResolveResult::Invalid:
            out.state = ReportArtifactQueryState::ArtifactIndexInvalid;
            break;
    }
    return out;
}

#ifndef ARDUINO
bool ReportTask::artifact_availability(
    const ReportArtifactKey &artifact,
    ReportArtifactAvailability &availability) const {
    if (!runtime_) {
        availability = {};
        return false;
    }
    return runtime_->find_availability(artifact, availability);
}
#endif

std::shared_ptr<const LargeByteBuffer> ReportTask::artifact_payload(
    const ReportArtifactDescriptor &artifact) const {
    return artifact_payload(
        ReportArtifactPayloadDescriptor::whole(artifact));
}

std::shared_ptr<const LargeByteBuffer> ReportTask::artifact_payload(
    const ReportArtifactPayloadDescriptor &payload) const {
    if (!runtime_) return {};
    return runtime_->find_payload(payload);
}

std::shared_ptr<const LargeByteBuffer>
ReportTask::artifact_payload_if_present(
    const ReportArtifactDescriptor &artifact) const {
    return artifact_payload_if_present(
        ReportArtifactPayloadDescriptor::whole(artifact));
}

std::shared_ptr<const LargeByteBuffer>
ReportTask::artifact_payload_if_present(
    const ReportArtifactPayloadDescriptor &payload) const {
    if (!runtime_) return {};
    return runtime_->find_payload_if_present(payload);
}

ReportArtifactPayloadSelection ReportTask::select_artifact_payload(
    const ReportArtifactDescriptor &artifact,
    bool prefer_deflate) const {
    return select_artifact_payload(
        ReportArtifactPayloadDescriptor::whole(artifact), prefer_deflate);
}

ReportArtifactPayloadSelection ReportTask::select_artifact_payload(
    const ReportArtifactPayloadDescriptor &payload,
    bool prefer_deflate) const {
    if (!runtime_) return {};
    return runtime_->select_payload(payload, prefer_deflate);
}

ReportArtifactPayloadSelection
ReportTask::select_artifact_payload_if_present(
    const ReportArtifactDescriptor &artifact,
    bool prefer_deflate) const {
    return select_artifact_payload_if_present(
        ReportArtifactPayloadDescriptor::whole(artifact), prefer_deflate);
}

ReportArtifactPayloadSelection
ReportTask::select_artifact_payload_if_present(
    const ReportArtifactPayloadDescriptor &payload,
    bool prefer_deflate) const {
    if (!runtime_) return {};
    return runtime_->select_payload_if_present(
        payload, prefer_deflate);
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

bool ReportTask::try_artifact_failure(
    const ReportArtifactKey &artifact,
    ReportArtifactFailureStatus &failure) const {
    if (!runtime_) {
        failure = {};
        return false;
    }
    return runtime_->find_failure(artifact, failure, 0);
}

bool ReportTask::payload_failure(
    const ReportArtifactPayloadDescriptor &payload,
    ReportArtifactFailureStatus &failure) const {
    if (!runtime_) {
        failure = {};
        return false;
    }
    return runtime_->find_payload_failure(payload, failure);
}

bool ReportTask::try_payload_failure(
    const ReportArtifactPayloadDescriptor &payload,
    ReportArtifactFailureStatus &failure) const {
    if (!runtime_) {
        failure = {};
        return false;
    }
    return runtime_->find_payload_failure(payload, failure, 0);
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

    if (runtime.payload_deflater.active()) {
        worked = runtime.payload_deflater.poll(
            AC_REPORT_DEFLATE_INPUT_CHUNK_BYTES) || worked;
    }
    worked = runtime.finish_payload_deflate() || worked;
    worked = runtime.start_pending_deflate() || worked;

    ReportTaskCommand command;
    const ReportEngineStatus command_engine_status = runtime.engine.status();
    const bool cache_load_available =
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
                if (!command.force_rebuild && runtime.artifact_index &&
                    runtime.artifact_index->availability(
                        command.artifact, available)) {
                    break;
                }

                const ReportRequestEnqueueResult queued =
                    runtime.engine.request(command.artifact,
                                           command.priority,
                                           command.generation,
                                           command.force_rebuild);
                if (queued.status == ReportRequestEnqueueStatus::Full ||
                    queued.status == ReportRequestEnqueueStatus::Invalid) {
                    runtime.command_failures++;
                } else {
                    if (command.force_rebuild &&
                        queued.ticket.generation == command.generation) {
#ifdef ARDUINO
                        char day[9] = {};
                        command.artifact.sleep_day.format_yyyymmdd(
                            day, sizeof(day));

                        Log::logf(CAT_REPORT,
                                  LOG_INFO,
                                  "rebuild started night=%s",
                                  day[0] ? day : "invalid");
#endif
                    }
                    if (command.priority ==
                        ReportRequestPriority::Foreground) {
                        worked = runtime.preempt_background_for_foreground() ||
                            worked;
                    }
                }
                break;
            }

            case ReportTaskCommandKind::CacheArtifact: {
                worked = runtime.preempt_background_work() || worked;
                const PayloadLoadStartResult started =
                    runtime.start_payload_load(
                        command.payload,
                        command.generation,
                        StorageReadLane::Foreground);

                if (started == PayloadLoadStartResult::Busy) {
                    (void)runtime.enqueue(command);
                    break;
                }
                if (started == PayloadLoadStartResult::Superseded ||
                    started == PayloadLoadStartResult::Started ||
                    started == PayloadLoadStartResult::AlreadyCached) {
                    break;
                }

                const char *error = "report_payload_load_start_failed";
                if (started == PayloadLoadStartResult::TooLarge) {
                    error = "report_payload_too_large";
                } else if (started ==
                           PayloadLoadStartResult::MemoryUnavailable) {
                    error = "report_payload_memory_unavailable";
                }

                if (runtime.lock(20)) {
                    runtime.payload_load_failed = command.payload;
                    runtime.payload_load_retry_at_ms =
                        now_ms + ARTIFACT_FAILURE_RETRY_MS;
                    copy_cstr(runtime.payload_load_error,
                              sizeof(runtime.payload_load_error),
                              error);
                    if (command.payload.is_whole()) {
                        runtime.remember_artifact_failure_locked(
                            command.artifact, error, now_ms);
                    }
                    runtime.unlock();
                }
                break;
            }

            case ReportTaskCommandKind::RefreshCatalog:
                if (runtime.spool_availability_probe.status().active()) {
                    runtime.spool_availability_probe.cancel();
                }
                runtime.pending_refresh.generation = command.generation;
                runtime.pending_refresh.current_offset_valid =
                    command.current_offset_valid;
                runtime.pending_refresh.current_offset_minutes =
                    command.current_offset_minutes;
                runtime.pending_refresh.target = command.catalog_target;
                runtime.pending_refresh.summary_attempted =
                    command.catalog_target.valid();
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

    if (runtime.spool_availability_probe.status().active()) {
        worked = runtime.spool_availability_probe.poll() || worked;
    }
    worked = runtime.observe_spool_availability_probe(now_ms) || worked;

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
                                 runtime.catalog_store_retry_attempt,
                                 CATALOG_STORE_RETRY_MIN_MS,
                                 CATALOG_STORE_RETRY_MAX_MS);
                advance_background_retry(
                    runtime.catalog_store_retry_attempt);
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
                    runtime.pending_refresh.target = runtime.refresh_target;
                    runtime.pending_refresh.summary_attempted = true;
                }
                runtime.catalog_refresh_retry_at_ms =
                    now_ms + next_background_retry_delay(
                                 runtime.catalog_refresh_retry_attempt,
                                 CATALOG_STORE_RETRY_MIN_MS,
                                 CATALOG_STORE_RETRY_MAX_MS);
                advance_background_retry(
                    runtime.catalog_refresh_retry_attempt);
                runtime.command_failures++;
            } else {
                runtime.catalog_refresh_retry_at_ms = 0;
                runtime.catalog_refresh_retry_attempt = 0;
                runtime.command_failures++;
            }
            runtime.refresh_generation = 0;
            runtime.refresh_target = {};
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

    if (runtime.pending_refresh.valid() &&
        runtime.pending_refresh.target.valid() &&
        !runtime.catalog && !runtime.catalog_load_pending &&
        runtime.store_purpose != CatalogStorePurpose::Load) {
        runtime.pending_refresh.target = {};
        runtime.pending_refresh.summary_attempted = false;
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
        std::shared_ptr<const NightCatalogSummarySnapshot> summary =
            runtime.summary_acquisition.snapshot();
        const ReportSummaryAcquisitionStatus summary_status =
            runtime.summary_acquisition.status();
        if (summary && runtime.catalog &&
            summary_status.state == ReportSummaryAcquisitionState::Ready &&
            summary_status.generation ==
                runtime.pending_refresh.generation) {
            summary = NightCatalogSummarySnapshot::preserve_expired_history(
                *summary, *runtime.catalog);
            if (!summary) {
                runtime.catalog_refresh_retry_at_ms =
                    now_ms + next_background_retry_delay(
                                 runtime.catalog_refresh_retry_attempt,
                                 CATALOG_STORE_RETRY_MIN_MS,
                                 CATALOG_STORE_RETRY_MAX_MS);
                advance_background_retry(
                    runtime.catalog_refresh_retry_attempt);
                runtime.command_failures++;
                worked = true;
                runtime.publish_status();
                return worked;
            }
            runtime.summary_acquisition.seed(summary);
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
            runtime.refresh_generation = runtime.pending_refresh.generation;
            runtime.refresh_offset_valid =
                runtime.pending_refresh.current_offset_valid;
            runtime.refresh_offset_minutes =
                runtime.pending_refresh.current_offset_minutes;
            runtime.refresh_target = runtime.pending_refresh.target;
            runtime.pending_refresh.clear();
            runtime.catalog_refresh_retry_at_ms = 0;
            worked = true;
        } else {
            runtime.catalog_refresh_retry_at_ms =
                now_ms + next_background_retry_delay(
                             runtime.catalog_refresh_retry_attempt,
                             CATALOG_STORE_RETRY_MIN_MS,
                             CATALOG_STORE_RETRY_MAX_MS);
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
        runtime.store_purpose == CatalogStorePurpose::None &&
        !runtime.pending_catalog_save;
    if (catalog_stable && !background_work_blocked &&
        startup_idle_work_allowed) {
        worked = runtime.start_spool_availability_probe(now_ms) || worked;
        worked = runtime.schedule_catalog_work(now_ms) || worked;
        worked = runtime.schedule_legacy_cache_cleanup(now_ms) || worked;
    }

    worked = runtime.finish_built_bundle_cache() || worked;
    if (!runtime.pending_built_bundle) {
        const bool batch_range_records = record_budget > 1 &&
            runtime.engine.foreground_range_execution_active();

        if (!batch_range_records) {
            worked = runtime.engine.poll(
                now_ms, std::min<size_t>(record_budget, 1)) || worked;
        } else {
#ifdef ARDUINO
            const uint32_t slice_started_us = micros();
#endif
            for (size_t record = 0; record < record_budget; ++record) {
                const bool engine_worked = runtime.engine.poll(now_ms, 1);
                worked = engine_worked || worked;
                if (!engine_worked ||
                    !runtime.engine.foreground_range_execution_active()) {
                    break;
                }

#ifdef ARDUINO
                const uint32_t elapsed_us =
                    static_cast<uint32_t>(micros() - slice_started_us);
                if (elapsed_us >=
                    AC_REPORT_RANGE_PLOT_POLL_BUDGET_MS * 1000UL) {
                    break;
                }
#endif
            }
        }
    }

    std::shared_ptr<const ReportArtifactBundle> built_bundle =
        runtime.engine.take_built_bundle();
    if (built_bundle) {
        if (runtime.pending_built_bundle) {
            runtime.command_failures++;
        } else {
            runtime.pending_built_bundle = std::move(built_bundle);
        }
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

    worked = runtime.finish_built_bundle_cache() || worked;
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
        const bool worked = step(
            millis(), AC_REPORT_RANGE_PLOT_POLL_CHUNK_CAP);
        const ReportTaskControlSnapshot control = control_snapshot();
        if (worked) {
            vTaskDelay(pdMS_TO_TICKS(AC_REPORT_TASK_WORK_TICK_MS));
        } else if (control.background_active) {
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
