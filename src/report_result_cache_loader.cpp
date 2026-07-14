#include "report_result_cache_loader.h"

#include <algorithm>
#include <memory>
#include <new>
#include <stdio.h>
#include <string.h>

#include <Arduino.h>
#include <FS.h>

#include "background_worker.h"
#include "board_report.h"
#include "debug_log.h"
#include "memory_manager.h"
#include "report_cache_paths.h"
#include "report_diagnostics.h"
#include "report_manager_limits.h"
#include "report_result_cache_payload.h"
#include "report_result_slot_cache.h"
#include "report_spool_types.h"
#include "storage_manager.h"

namespace aircannect {
namespace {

constexpr size_t REPORT_CACHE_READ_CHUNK = 4096;
constexpr uint32_t REPORT_CACHE_FAILED_RETRY_MS = 2000;

enum class LoadPhase : uint8_t {
    Prepare,
    OpenArtifact,
    ReadArtifact,
    Complete,
};

enum class OpenOutcome : uint8_t {
    Opened,
    Missing,
    Invalid,
    Failed,
};

enum class ReadOutcome : uint8_t {
    Working,
    Complete,
    Failed,
};

struct LoadKey {
    uint64_t night_start_ms = 0;
    char etag[AC_REPORT_RESULT_ETAG_MAX] = {};
};

struct ProbeEntry {
    bool valid = false;
    LoadKey key;
    uint8_t checked_mask = 0;
    uint8_t available_mask = 0;
    uint8_t failed_mask = 0;
    uint32_t retry_after_ms = 0;
    uint32_t last_used = 0;
};

using CachePathBuilder = bool (*)(uint64_t, const char *, char *, size_t);
using CachePayloadValidator = bool (*)(const uint8_t *, size_t);

struct CacheArtifactSpec {
    ReportCacheArtifact artifact;
    size_t min_size;
    size_t max_size;
    CachePathBuilder build_path;
    CachePayloadValidator payload_valid;
    const char *label;
};

constexpr CacheArtifactSpec CACHE_ARTIFACTS[] = {
    {
        ReportCacheArtifact::Result,
        16,
        REPORT_RESULT_JSON_CACHE_MAX,
        result_json_cache_path_for_etag,
        report_result_json_cache_payload_valid,
        "JSON",
    },
    {
        ReportCacheArtifact::Plot,
        8,
        AC_REPORT_PLOT_MAX_BYTES,
        result_plot_cache_path_for_etag,
        report_result_plot_cache_payload_valid,
        "plot",
    },
};

constexpr size_t CACHE_ARTIFACT_COUNT =
    sizeof(CACHE_ARTIFACTS) / sizeof(CACHE_ARTIFACTS[0]);

bool key_matches(const LoadKey &key,
                 uint64_t night_start_ms,
                 const char *etag) {
    return key.night_start_ms == night_start_ms && etag &&
           strcmp(key.etag, etag) == 0;
}

void set_key(LoadKey &key, uint64_t night_start_ms, const char *etag) {
    key.night_start_ms = night_start_ms;
    snprintf(key.etag, sizeof(key.etag), "%s", etag ? etag : "");
}

OpenOutcome open_cache_file(const char *path,
                            size_t min_size,
                            size_t max_size,
                            File &file,
                            size_t &expected_size,
                            std::shared_ptr<ReportSpoolBuffer> &buffer) {
    Storage::Guard guard;
    if (!Storage::mounted()) return OpenOutcome::Failed;
    if (!Storage::exists(path)) return OpenOutcome::Missing;

    file = Storage::open(path, "r");
    if (!file) return OpenOutcome::Failed;

    expected_size = static_cast<size_t>(file.size());
    if (expected_size < min_size || expected_size > max_size) {
        file.close();
        return OpenOutcome::Invalid;
    }

    buffer = std::make_shared<ReportSpoolBuffer>();
    if (!buffer) {
        file.close();
        return OpenOutcome::Failed;
    }

    buffer->set_max_size(expected_size);
    if (!buffer->reserve_capacity(expected_size)) {
        buffer.reset();
        file.close();
        return OpenOutcome::Failed;
    }

    return OpenOutcome::Opened;
}

ReadOutcome read_cache_file_step(File &file,
                                 size_t expected_size,
                                 size_t &offset,
                                 ReportSpoolBuffer &buffer) {
    if (!file || offset >= expected_size) return ReadOutcome::Complete;

    const size_t len =
        std::min(REPORT_CACHE_READ_CHUNK, expected_size - offset);
    size_t append_offset = 0;
    uint8_t *destination = buffer.append_uninitialized(len, append_offset);
    if (!destination) return ReadOutcome::Failed;

    int read = 0;
    {
        Storage::Guard guard;
        read = file.read(destination, len);
    }

    if (read <= 0) {
        buffer.truncate(append_offset);
        return ReadOutcome::Failed;
    }

    const size_t read_size = static_cast<size_t>(read);
    if (read_size < len) buffer.truncate(append_offset + read_size);
    offset += read_size;

    return offset >= expected_size ? ReadOutcome::Complete
                                   : ReadOutcome::Working;
}

void close_cache_file(File &file) {
    if (!file) return;

    Storage::Guard guard;
    file.close();
}

}  // namespace

struct ReportResultCacheLoader::State {
    LoadKey queue[AC_REPORT_RESULT_CACHE_LOAD_QUEUE_MAX] = {};
    size_t queue_count = 0;

    bool active = false;
    LoadKey current;
    LoadPhase phase = LoadPhase::Prepare;
    uint8_t checked_mask = 0;
    uint8_t available_mask = 0;
    uint8_t failed_mask = 0;

    size_t artifact_index = 0;
    char path[REPORT_CACHE_PATH_MAX] = {};
    File file;
    size_t expected_size = 0;
    size_t offset = 0;
    std::shared_ptr<ReportSpoolBuffer> payload;

    ProbeEntry probes[AC_REPORT_RESULT_CACHE_PROBE_MAX] = {};
    uint32_t probe_tick = 0;
};

ReportResultCacheLoader::~ReportResultCacheLoader() {
    if (state_) {
        close_cache_file(state_->file);
        state_->~State();
        Memory::free(state_);
        state_ = nullptr;
    }

    if (lock_) {
        vSemaphoreDelete(lock_);
        lock_ = nullptr;
    }
}

bool ReportResultCacheLoader::ensure_lock() {
    if (lock_) return true;

    lock_ = xSemaphoreCreateMutex();
    if (!lock_) log_report_alloc_failed("result_cache_loader_lock", 0);
    return lock_ != nullptr;
}

bool ReportResultCacheLoader::begin() {
    if (!ensure_lock()) return false;
    if (state_) return true;

    state_ = static_cast<State *>(Memory::alloc_large(sizeof(State), false));
    if (!state_) {
        log_report_alloc_failed("result_cache_loader", sizeof(State));
        return false;
    }

    new (state_) State();
    return true;
}

ReportCacheLoadRequest ReportResultCacheLoader::request(
    uint64_t night_start_ms,
    const char *etag,
    ReportCacheArtifact artifact) {
    if (!state_ || !lock_ || night_start_ms == 0 || !etag || !etag[0]) {
        return ReportCacheLoadRequest::Unavailable;
    }
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) {
        return ReportCacheLoadRequest::Unavailable;
    }

    const uint8_t artifact_mask = static_cast<uint8_t>(artifact);
    const uint32_t now_ms = millis();
    for (size_t i = 0; i < AC_REPORT_RESULT_CACHE_PROBE_MAX; ++i) {
        ProbeEntry &probe = state_->probes[i];
        if (!probe.valid ||
            !key_matches(probe.key, night_start_ms, etag)) {
            continue;
        }

        probe.last_used = ++state_->probe_tick;
        if ((probe.failed_mask & artifact_mask) != 0 &&
            static_cast<int32_t>(now_ms - probe.retry_after_ms) < 0) {
            xSemaphoreGive(lock_);
            return ReportCacheLoadRequest::Failed;
        }
        if ((probe.checked_mask & artifact_mask) != 0 &&
            (probe.available_mask & artifact_mask) == 0 &&
            (probe.failed_mask & artifact_mask) == 0) {
            xSemaphoreGive(lock_);
            return ReportCacheLoadRequest::Missing;
        }

        break;
    }

    if (state_->active &&
        key_matches(state_->current, night_start_ms, etag)) {
        xSemaphoreGive(lock_);
        return ReportCacheLoadRequest::Pending;
    }
    for (size_t i = 0; i < state_->queue_count; ++i) {
        if (!key_matches(state_->queue[i], night_start_ms, etag)) continue;

        xSemaphoreGive(lock_);
        return ReportCacheLoadRequest::Pending;
    }

    if (state_->queue_count >= AC_REPORT_RESULT_CACHE_LOAD_QUEUE_MAX) {
        xSemaphoreGive(lock_);
        return ReportCacheLoadRequest::Full;
    }

    set_key(state_->queue[state_->queue_count++], night_start_ms, etag);
    xSemaphoreGive(lock_);

    if (BackgroundWorker *worker = background_worker()) worker->wake();
    return ReportCacheLoadRequest::Queued;
}

bool ReportResultCacheLoader::active() const {
    if (!state_ || !lock_) return false;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(1)) != pdTRUE) return true;

    const bool active = state_->active || state_->queue_count > 0;
    xSemaphoreGive(lock_);
    return active;
}

bool ReportResultCacheLoader::service() {
    if (!state_ || !lock_) return false;

    if (!state_->active) {
        if (xSemaphoreTake(lock_, pdMS_TO_TICKS(20)) != pdTRUE) return false;
        if (state_->queue_count == 0) {
            xSemaphoreGive(lock_);
            return false;
        }

        state_->active = true;
        state_->current = state_->queue[0];
        for (size_t i = 1; i < state_->queue_count; ++i) {
            state_->queue[i - 1] = state_->queue[i];
        }
        state_->queue[--state_->queue_count] = LoadKey{};
        state_->phase = LoadPhase::Prepare;
        state_->checked_mask = 0;
        state_->available_mask = 0;
        state_->failed_mask = 0;
        state_->artifact_index = 0;
        state_->path[0] = '\0';
        state_->expected_size = 0;
        state_->offset = 0;
        state_->payload.reset();
        xSemaphoreGive(lock_);
    }

    switch (state_->phase) {
        case LoadPhase::Prepare:
            state_->artifact_index = 0;
            state_->phase = LoadPhase::OpenArtifact;
            break;

        case LoadPhase::OpenArtifact: {
            const CacheArtifactSpec &artifact =
                CACHE_ARTIFACTS[state_->artifact_index];
            const uint8_t artifact_mask =
                static_cast<uint8_t>(artifact.artifact);
            state_->checked_mask |= artifact_mask;

            if (!artifact.build_path(state_->current.night_start_ms,
                                     state_->current.etag,
                                     state_->path,
                                     sizeof(state_->path))) {
                state_->failed_mask |= artifact_mask;
                state_->artifact_index++;
                state_->phase = state_->artifact_index < CACHE_ARTIFACT_COUNT
                    ? LoadPhase::OpenArtifact
                    : LoadPhase::Complete;
                break;
            }

            const OpenOutcome outcome = open_cache_file(state_->path,
                                                        artifact.min_size,
                                                        artifact.max_size,
                                                        state_->file,
                                                        state_->expected_size,
                                                        state_->payload);

            if (outcome == OpenOutcome::Opened) {
                state_->offset = 0;
                state_->phase = LoadPhase::ReadArtifact;
            } else {
                if (outcome == OpenOutcome::Failed) {
                    state_->failed_mask |= artifact_mask;
                }
                if (outcome == OpenOutcome::Invalid) {
                    Log::logf(CAT_REPORT,
                              LOG_WARN,
                              "Result cache %s rejected night=%llu\n",
                              artifact.label,
                              static_cast<unsigned long long>(
                                  state_->current.night_start_ms));
                }

                state_->payload.reset();
                state_->artifact_index++;
                state_->phase = state_->artifact_index < CACHE_ARTIFACT_COUNT
                    ? LoadPhase::OpenArtifact
                    : LoadPhase::Complete;
            }
            break;
        }

        case LoadPhase::ReadArtifact: {
            const CacheArtifactSpec &artifact =
                CACHE_ARTIFACTS[state_->artifact_index];
            const uint8_t artifact_mask =
                static_cast<uint8_t>(artifact.artifact);
            const ReadOutcome outcome = read_cache_file_step(
                state_->file,
                state_->expected_size,
                state_->offset,
                *state_->payload);
            if (outcome == ReadOutcome::Working) break;

            close_cache_file(state_->file);
            bool published = false;
            if (outcome == ReadOutcome::Complete &&
                artifact.payload_valid(state_->payload->data(),
                                       state_->payload->size())) {
                const std::shared_ptr<ReportSpoolBuffer> empty;
                published = artifact.artifact == ReportCacheArtifact::Result
                    ? slots_.publish(ReportResultState::Ready,
                                     state_->current.night_start_ms,
                                     state_->current.etag,
                                     state_->payload,
                                     empty)
                    : slots_.publish(ReportResultState::Ready,
                                     state_->current.night_start_ms,
                                     state_->current.etag,
                                     empty,
                                     state_->payload);
            }

            if (published) {
                state_->available_mask |= artifact_mask;
            } else if (outcome == ReadOutcome::Failed) {
                state_->failed_mask |= artifact_mask;
            } else {
                Log::logf(CAT_REPORT,
                          LOG_WARN,
                          "Result cache %s rejected night=%llu\n",
                          artifact.label,
                          static_cast<unsigned long long>(
                              state_->current.night_start_ms));
            }

            state_->payload.reset();
            state_->artifact_index++;
            state_->phase = state_->artifact_index < CACHE_ARTIFACT_COUNT
                ? LoadPhase::OpenArtifact
                : LoadPhase::Complete;
            break;
        }

        case LoadPhase::Complete:
            finish_current();
            break;
    }

    return true;
}

void ReportResultCacheLoader::finish_current() {
    close_cache_file(state_->file);
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE) return;

    size_t pick = 0;
    bool found = false;
    for (size_t i = 0; i < AC_REPORT_RESULT_CACHE_PROBE_MAX; ++i) {
        if (state_->probes[i].valid &&
            key_matches(state_->probes[i].key,
                        state_->current.night_start_ms,
                        state_->current.etag)) {
            pick = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < AC_REPORT_RESULT_CACHE_PROBE_MAX; ++i) {
            if (!state_->probes[i].valid) {
                pick = i;
                found = true;
                break;
            }
            if (state_->probes[i].last_used <
                state_->probes[pick].last_used) {
                pick = i;
            }
        }
    }

    ProbeEntry &probe = state_->probes[pick];
    probe.valid = true;
    probe.key = state_->current;
    probe.checked_mask = state_->checked_mask;
    probe.available_mask = state_->available_mask;
    probe.failed_mask = state_->failed_mask;
    probe.retry_after_ms = state_->failed_mask
                               ? millis() + REPORT_CACHE_FAILED_RETRY_MS
                               : 0;
    probe.last_used = ++state_->probe_tick;

    state_->active = false;
    state_->current = LoadKey{};
    state_->phase = LoadPhase::Prepare;
    state_->checked_mask = 0;
    state_->available_mask = 0;
    state_->failed_mask = 0;
    state_->artifact_index = 0;
    state_->path[0] = '\0';
    state_->expected_size = 0;
    state_->offset = 0;
    state_->payload.reset();
    xSemaphoreGive(lock_);
}

void ReportResultCacheLoader::invalidate(uint64_t night_start_ms, bool all) {
    if (!state_ || !lock_) return;
    if (xSemaphoreTake(lock_, pdMS_TO_TICKS(5)) != pdTRUE) return;

    for (size_t i = 0; i < AC_REPORT_RESULT_CACHE_PROBE_MAX; ++i) {
        ProbeEntry &probe = state_->probes[i];
        if (probe.valid &&
            (all || probe.key.night_start_ms == night_start_ms)) {
            probe = ProbeEntry{};
        }
    }

    size_t write = 0;
    for (size_t read = 0; read < state_->queue_count; ++read) {
        const LoadKey &key = state_->queue[read];
        if (all || key.night_start_ms == night_start_ms) continue;

        if (write != read) state_->queue[write] = key;
        write++;
    }
    const size_t compact_count = write;
    while (write < state_->queue_count) {
        state_->queue[write++] = LoadKey{};
    }
    state_->queue_count = compact_count;

    xSemaphoreGive(lock_);
}

}  // namespace aircannect
