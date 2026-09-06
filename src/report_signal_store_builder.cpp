#include "report_signal_store_builder.h"

#include <algorithm>
#include <cmath>
#include <limits.h>
#include <new>
#include <optional>
#include <string.h>
#include <utility>

#include "large_object.h"
#include "edf_bytes.h"
#include "large_scratch_array.h"
#include "memory_manager.h"
#include "report_night_summary.h"
#include "report_build_checkpoint.h"
#include "report_planner.h"

namespace aircannect {
namespace {

constexpr size_t INITIAL_EVENT_CAPACITY = 64;
constexpr size_t RAW_BUFFER_BUDGET = 2 * 1024 * 1024;

int64_t align_block_start(int64_t timestamp_ms) {
    int64_t remainder = timestamp_ms % REPORT_SIGNAL_STORE_BLOCK_MS;
    if (remainder < 0) remainder += REPORT_SIGNAL_STORE_BLOCK_MS;
    return timestamp_ms - remainder;
}

uint32_t grid_phase(int64_t timestamp_ms, uint32_t interval_ms) {
    int64_t phase = timestamp_ms % interval_ms;
    if (phase < 0) phase += interval_ms;
    return static_cast<uint32_t>(phase);
}

int64_t first_grid_sample(int64_t start_ms,
                          uint32_t interval_ms,
                          uint32_t phase_ms) {
    const uint32_t start_phase = grid_phase(start_ms, interval_ms);
    const uint32_t delta = phase_ms >= start_phase
        ? phase_ms - start_phase
        : interval_ms - (start_phase - phase_ms);
    return start_ms + delta;
}

uint64_t expected_samples(const NightCatalogTimeRange &session,
                          uint32_t interval_ms,
                          uint32_t phase_ms) {
    const int64_t first = first_grid_sample(
        session.start_ms, interval_ms, phase_ms);
    if (first >= session.end_ms) return 0;

    return 1 + static_cast<uint64_t>(
        (session.end_ms - 1 - first) / interval_ms);
}

uint8_t lod_mask(uint32_t interval_ms) {
    uint8_t mask = 0;
    if (interval_ms < 1000) mask |= REPORT_SIGNAL_STORE_LOD_1S;
    if (interval_ms < 10000) mask |= REPORT_SIGNAL_STORE_LOD_10S;
    return mask;
}

struct TrackWork {
    ReportSignalStoreTrack track;
    ReportBuildTrackState closed;
    ReportBuildTrackState previous_full;
    ReportMetricAccumulator *tail_metrics = nullptr;
    bool source_present = false;
    int16_t *raw_blocks[REPORT_SIGNAL_STORE_MAX_BLOCKS] = {};
    uint8_t *sessions_seen = nullptr;
    uint8_t written_blocks[REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES] = {};
    int newest_slot = -1;
    int append_slot = 0;
    bool file_exists = false;
    bool recount_samples = false;
    int64_t last_accepted_ms = -1;
    size_t last_slot = 0;
    uint32_t next_sample_index = 0;
};

float fallback_scale(ReportSignalId signal) {
    switch (signal) {
        case ReportSignalId::Flow:
        case ReportSignalId::Leak: return 0.1f;
        case ReportSignalId::IeRatio: return 1.0f;
        case ReportSignalId::TidalVolume:
        case ReportSignalId::FlowLimitation:
        case ReportSignalId::InspiratoryDuration:
        case ReportSignalId::Snore: return 0.001f;
        default: return 0.01f;
    }
}

ReportSignalStoreTrack track_format(const ReportSeriesDescriptor &series,
                                    const EdfSignalScale *scale,
                                    int64_t timestamp_ms) {
    ReportSignalStoreTrack track;
    const float multiplier =
        report_series_canonical_value_milli(series, 1000) / 1000.0f;
    track.signal = series.signal;
    track.sample_interval_ms = series.sample_interval_ms;
    track.grid_phase_ms = grid_phase(timestamp_ms, series.sample_interval_ms);
    track.value_scale = scale ? scale->scale * multiplier
                              : fallback_scale(series.signal);
    track.value_offset = scale ? scale->offset * multiplier : 0.0f;
    track.missing_value = scale && scale->digital_min == INT16_MIN
        ? INT16_MAX : INT16_MIN;
    return track;
}

bool same_track_format(const ReportSignalStoreTrack &a,
                       const ReportSignalStoreTrack &b) {
    return a.signal == b.signal &&
        a.sample_interval_ms == b.sample_interval_ms &&
        a.grid_phase_ms == b.grid_phase_ms &&
        a.value_scale == b.value_scale && a.value_offset == b.value_offset &&
        a.missing_value == b.missing_value;
}

void destroy_bundle(ReportSignalStoreBundle *bundle) {
    if (!bundle) return;
    bundle->~ReportSignalStoreBundle();
    Memory::free(bundle);
}

}  // namespace

struct ReportSignalStoreBuilder::Runtime {
    ReportArtifactRequest request;
    const ReportReadPlan *plan = nullptr;
    TrackWork *tracks = nullptr;
    size_t track_count = 0;
    size_t track_capacity = 0;
    size_t last_track = SIZE_MAX;
    ReportSourceId last_source = ReportSourceId::Summary;
    bool last_original = false;
    ReportEventRecord *events = nullptr;
    size_t event_count = 0;
    size_t event_capacity = 0;
    int64_t first_block_start_ms = 0;
    uint16_t block_slot_count = 0;
    uint32_t store_generation = 0;
    ReportMetricAccumulator metrics;
    ReportMetricAccumulator closed_metrics;
    int64_t closed_before_ms = 0;
    uint8_t checkpoint_slot = 1;
    std::shared_ptr<const LargeByteBuffer> previous_checkpoint;
    bool active = false;
    std::shared_ptr<ReportSignalStoreBundle> completed;
    EdfSignalScale scales[static_cast<size_t>(ReportSignalId::Count)];
    bool configured[static_cast<size_t>(ReportSignalId::Count)] = {};
    size_t writing_track = SIZE_MAX;
    size_t writing_slot = 0;
    size_t writing_count = 0;
    bool completed_blocks_pending = false;
    size_t buffered_raw_bytes = 0;

    void release_track_blocks(TrackWork &track) {
        for (size_t slot = 0;
             slot < REPORT_SIGNAL_STORE_MAX_BLOCKS;
             ++slot) {
            Memory::free(track.raw_blocks[slot]);
            track.raw_blocks[slot] = nullptr;
        }
        Memory::free(track.sessions_seen);
        track.sessions_seen = nullptr;
    }

    void clear_work() {
        for (size_t i = 0; i < track_count; ++i) {
            release_track_blocks(tracks[i]);
            LargeObject::destroy(tracks[i].tail_metrics);
        }
        for (size_t i = 0; i < track_capacity; ++i) {
            tracks[i].~TrackWork();
        }
        Memory::free(tracks);
        tracks = nullptr;
        track_count = 0;
        track_capacity = 0;
        last_track = SIZE_MAX;

        Memory::free(events);
        events = nullptr;
        event_count = 0;
        event_capacity = 0;

        metrics.clear();
        closed_metrics.clear();
        closed_before_ms = 0;
        checkpoint_slot = 1;
        previous_checkpoint.reset();

        request = {};
        plan = nullptr;
        first_block_start_ms = 0;
        block_slot_count = 0;
        store_generation = 0;
        active = false;
        memset(configured, 0, sizeof(configured));
        writing_track = SIZE_MAX;
        writing_slot = 0;
        writing_count = 0;
        completed_blocks_pending = false;
        buffered_raw_bytes = 0;
    }

    bool reserve_events(size_t required) {
        if (required <= event_capacity) return true;

        size_t next = event_capacity ? event_capacity * 2
                                     : INITIAL_EVENT_CAPACITY;
        if (next < required) next = required;
        if (next > SIZE_MAX / sizeof(ReportEventRecord)) return false;

        void *storage = Memory::realloc_large(
            events, next * sizeof(ReportEventRecord), false);
        if (!storage) return false;

        events = static_cast<ReportEventRecord *>(storage);
        event_capacity = next;
        return true;
    }

    bool prepare_tail(TrackWork &work) {
        if (work.tail_metrics) return true;
        work.tail_metrics = LargeObject::create<ReportMetricAccumulator>();
        return work.tail_metrics &&
            work.tail_metrics->begin(report_signal_bit(work.track.signal));
    }

    bool ensure_raw_block(TrackWork &work,
                          size_t slot,
                          uint32_t samples_per_block,
                          const char *&failure) {
        if (work.raw_blocks[slot]) return true;

        work.raw_blocks[slot] = static_cast<int16_t *>(Memory::alloc_large(
            static_cast<size_t>(samples_per_block) * sizeof(int16_t), false));
        if (!work.raw_blocks[slot]) {
            failure = "report_signal_store_block_allocation_failed";
            return false;
        }
        buffered_raw_bytes +=
            static_cast<size_t>(samples_per_block) * sizeof(int16_t);
        std::fill_n(work.raw_blocks[slot], samples_per_block,
                    work.track.missing_value);

        if (work.track.present_blocks[slot / 8] & (1u << (slot % 8))) {
            return true;
        }
        for (size_t later = slot + 1;
             later < block_slot_count;
             ++later) {
            if (work.written_blocks[later / 8] & (1u << (later % 8))) {
                failure = "report_signal_store_rebuild_required";
                return false;
            }
        }
        work.track.present_blocks[slot / 8] |=
            static_cast<uint8_t>(1u << (slot % 8));
        ++work.track.present_block_count;
        return true;
    }

    bool append_raw_range(uint16_t session_index,
                          const ReportSeriesDescriptor &series,
                          TrackWork &work,
                          size_t slot,
                          uint32_t sample_index,
                          uint32_t count,
                          int64_t first_timestamp_ms,
                          const uint8_t *raw_bytes,
                          const EdfSignalScale *scale,
                          int32_t single_canonical_value,
                          const char *&failure) {
        const uint32_t samples_per_block = static_cast<uint32_t>(
            REPORT_SIGNAL_STORE_BLOCK_MS / series.sample_interval_ms);
        const int64_t last_timestamp_ms = first_timestamp_ms +
            static_cast<int64_t>(count - 1) * series.sample_interval_ms;

        for (uint32_t i = 0; i < count; ++i) {
            const int16_t raw = edf_read_i16_le_sample(raw_bytes, i);
            if (raw == work.track.missing_value) {
                failure = "report_signal_store_value_invalid";
                return false;
            }
        }

        const bool write_sample = static_cast<int>(slot) >= work.append_slot;
        if (write_sample && !ensure_raw_block(
                work, slot, samples_per_block, failure)) {
            return false;
        }
        if (write_sample) {
            memcpy(work.raw_blocks[slot] + sample_index,
                   raw_bytes, static_cast<size_t>(count) * 2);
            if (work.newest_slot >= 0 &&
                static_cast<int>(slot) > work.newest_slot) {
                completed_blocks_pending = true;
            }
            work.newest_slot = std::max(work.newest_slot,
                                        static_cast<int>(slot));
        }

        work.last_accepted_ms = last_timestamp_ms;
        work.last_slot = slot;
        work.next_sample_index = sample_index + count;

        const uint32_t closed_count =
            last_timestamp_ms < closed_before_ms
                ? count
                : (first_timestamp_ms < closed_before_ms
                       ? static_cast<uint32_t>(
                             (closed_before_ms - first_timestamp_ms - 1) /
                                 series.sample_interval_ms + 1)
                       : 0);
        if (closed_count > 0) {
            if (work.closed.valid_sample_count == 0) {
                work.closed.first_valid_sample_ms = first_timestamp_ms;
            }
            work.closed.last_valid_sample_ms = first_timestamp_ms +
                static_cast<int64_t>(closed_count - 1) * series.sample_interval_ms;
            work.closed.valid_sample_count += closed_count;
        }

        if (report_signal_has_metric_consumer(series.signal)) {
            if (closed_count < count && !prepare_tail(work)) {
                failure = "report_signal_store_tail_allocation_failed";
                return false;
            }

            for (uint32_t i = 0; i < count; ++i) {
                const int32_t canonical_value = scale
                    ? report_series_canonical_value_milli(
                          series,
                          edf_report_physical_value_milli(
                              *scale, edf_read_i16_le_sample(raw_bytes, i)))
                    : single_canonical_value;
                if (i < closed_count) {
                    closed_metrics.accept(series.signal, canonical_value);
                } else {
                    work.tail_metrics->accept(series.signal, canonical_value);
                }
            }
        }

        ReportSignalStoreTrack &track = work.track;
        if (track.valid_sample_count == 0) {
            track.first_valid_sample_ms = first_timestamp_ms;
        }
        track.last_valid_sample_ms = last_timestamp_ms;
        track.valid_sample_count += count;

        if (!work.sessions_seen[session_index]) {
            work.sessions_seen[session_index] = 1;
            const ReportReadSession *session = plan->session(session_index);
            const uint64_t expected = expected_samples(
                session->output_window, series.sample_interval_ms,
                track.grid_phase_ms);
            if (track.expected_sample_count > UINT64_MAX - expected) {
                failure = "report_signal_store_coverage_overflow";
                return false;
            }
            track.expected_sample_count += expected;
        }
        return true;
    }

    void update_expected_coverage() {
        for (size_t i = 0; i < track_count; ++i) {
            TrackWork &work = tracks[i];
            memset(work.sessions_seen, 0, plan->session_count());
            uint64_t expected = 0;
            bool matched = false;

            for (size_t op = 0; op < plan->operation_count(); ++op) {
                const ReportReadOperation &operation = *plan->operation(op);
                const auto *file = plan->source_file(operation);
                const auto *section = plan->fallback_section(operation);
                const bool edf = operation.kind == ReportReadOperationKind::Numeric;
                if ((!edf || !file) &&
                    (operation.kind != ReportReadOperationKind::FallbackSeries ||
                     !section)) continue;

                size_t count = 0;
                const auto *mappings = plan->mappings(operation, count);
                for (size_t m = 0; m < count; ++m) {
                    const auto &mapping = mappings[m];
                    const auto &series = mapping.series;
                    if (series.signal != work.track.signal ||
                        series.sample_interval_ms != work.track.sample_interval_ms) {
                        continue;
                    }

                    const auto format = track_format(
                        series, edf ? &mapping.layout.scale : nullptr,
                        edf ? file->record_start_ms : section->coverage.start_ms);

                    if (!same_track_format(work.track, format) ||
                        work.sessions_seen[operation.session_index]) continue;

                    work.sessions_seen[operation.session_index] = 1;
                    matched = true;
                    expected += expected_samples(
                        plan->session(operation.session_index)->output_window,
                        series.sample_interval_ms, format.grid_phase_ms);
                }
            }
            if (matched) work.track.expected_sample_count = expected;
            work.source_present = matched;
        }
    }
};

ReportSignalStoreBuilder::ReportSignalStoreBuilder() :
    runtime_(LargeObject::create<Runtime>()) {}

ReportSignalStoreBuilder::~ReportSignalStoreBuilder() {
    if (runtime_) runtime_->clear_work();
    LargeObject::destroy(runtime_);
}

void ReportSignalStoreBuilder::begin(ReportSignalStoreService &store) {
    store_ = &store;
}

bool ReportSignalStoreBuilder::begin_build(
    const ReportArtifactRequest &request,
    const ReportReadPlan &plan,
    uint32_t store_generation,
    std::shared_ptr<const LargeByteBuffer> previous,
    std::shared_ptr<const LargeByteBuffer> checkpoint_bytes) {
    failure_reason_ = nullptr;
    if (!runtime_) {
        failure_reason_ = "report_signal_store_runtime_unavailable";
        return false;
    }

    runtime_->clear_work();
    runtime_->completed.reset();
    if (!request.ticket.valid() || request.artifact != plan.key() ||
        store_generation == 0 || !plan.night().sleep_day.valid() ||
        !plan.night().source_revision.valid() ||
        plan.night().day_end_ms <= plan.night().day_start_ms) {
        failure_reason_ = "report_signal_store_request_invalid";
        return false;
    }

    const int64_t first_block = align_block_start(
        plan.night().day_start_ms);
    const int64_t last_block = align_block_start(
        plan.night().day_end_ms - 1);
    const int64_t slot_count =
        (last_block - first_block) / REPORT_SIGNAL_STORE_BLOCK_MS + 1;
    if (slot_count <= 0 ||
        slot_count > static_cast<int64_t>(REPORT_SIGNAL_STORE_MAX_BLOCKS)) {
        failure_reason_ = "report_signal_store_day_layout_invalid";
        return false;
    }

    for (size_t i = 0; i < plan.session_count(); ++i) {
        const ReportReadSession *session = plan.session(i);
        if (!session || !session->output_window.valid() ||
            session->output_window.start_ms < plan.night().day_start_ms ||
            session->output_window.end_ms > plan.night().day_end_ms) {
            failure_reason_ = "report_signal_store_session_invalid";
            return false;
        }
    }

    ReportSignalStoreNightView previous_night;
    if (previous && !ReportSignalStoreNightCodec::decode(
            previous->data(), previous->size(), previous_night)) {
        failure_reason_ = "report_signal_store_previous_invalid";
        return false;
    }

    ReportBuildCheckpointView checkpoint;
    if (checkpoint_bytes &&
        (!ReportBuildCheckpointCodec::decode(
             checkpoint_bytes->data(), checkpoint_bytes->size(), checkpoint) ||
         checkpoint.track_count != previous_night.night.track_count)) {
        failure_reason_ = "report_signal_store_checkpoint_invalid";
        return false;
    }

    runtime_->checkpoint_slot = previous_night.night.checkpoint_slot == 1
        ? 2 : 1;
    runtime_->previous_checkpoint = checkpoint_bytes;
    for (size_t i = 0; i < plan.session_count(); ++i) {
        runtime_->closed_before_ms = std::max(
            runtime_->closed_before_ms,
            align_block_start(plan.session(i)->output_window.end_ms));
    }
    const size_t capacity = previous_night.night.track_count;
    if (capacity > 0) {
        if (capacity > SIZE_MAX / sizeof(TrackWork)) {
            failure_reason_ = "report_signal_store_track_count_invalid";
            return false;
        }

        runtime_->tracks = static_cast<TrackWork *>(Memory::alloc_large(
            capacity * sizeof(TrackWork), false));
        if (!runtime_->tracks) {
            failure_reason_ = "report_signal_store_track_allocation_failed";
            return false;
        }
        for (size_t i = 0; i < capacity; ++i) {
            new (&runtime_->tracks[i]) TrackWork();
        }
        runtime_->track_capacity = capacity;
    }

    for (size_t i = 0; i < previous_night.night.track_count; ++i) {
        TrackWork &work = runtime_->tracks[runtime_->track_count++];
        previous_night.track(i, work.track);
        memcpy(work.written_blocks, work.track.present_blocks,
               sizeof(work.written_blocks));
        work.file_exists = true;
        work.recount_samples = !checkpoint_bytes;
        work.append_slot = static_cast<int>(
            (align_block_start(work.track.last_valid_sample_ms) - first_block) /
            REPORT_SIGNAL_STORE_BLOCK_MS);
        if (checkpoint_bytes) {
            work.previous_full.valid_sample_count = work.track.valid_sample_count;
            work.previous_full.first_valid_sample_ms = work.track.first_valid_sample_ms;
            work.previous_full.last_valid_sample_ms = work.track.last_valid_sample_ms;
            checkpoint.track(i, work.closed);
            work.track.valid_sample_count = work.closed.valid_sample_count;
            work.track.first_valid_sample_ms = work.closed.first_valid_sample_ms;
            work.track.last_valid_sample_ms = work.closed.last_valid_sample_ms;
            // A lagging source can still extend before another source's tail.
            // The planner's per-source cursor controls which samples arrive.
            work.append_slot = 0;
        }
        work.track.source_revision = plan.night().source_revision;

        work.sessions_seen = static_cast<uint8_t *>(Memory::calloc_large(
            plan.session_count(), sizeof(uint8_t), false));

        if (!work.sessions_seen) {
            failure_reason_ = "report_signal_store_session_map_failed";
            return false;
        }
    }

    runtime_->request = request;
    runtime_->plan = &plan;
    runtime_->first_block_start_ms = first_block;
    runtime_->block_slot_count = static_cast<uint16_t>(slot_count);
    runtime_->store_generation = store_generation;
    if (!runtime_->metrics.begin(plan) ||
        !runtime_->closed_metrics.begin(plan) ||
        (checkpoint_bytes &&
         (!runtime_->closed_metrics.restore(
              checkpoint.metrics, checkpoint.metrics_size) ||
          !runtime_->closed_metrics.merge(runtime_->metrics)))) {
        failure_reason_ = "report_signal_store_metrics_allocation_failed";
        runtime_->clear_work();
        return false;
    }

    if (checkpoint_bytes) {
        ReportSignalStoreEventFileView old_events;
        if (!ReportSignalStoreEventCodec::inspect(
                checkpoint.events, checkpoint.events_size, old_events) ||
            !runtime_->reserve_events(old_events.event_count)) {
            failure_reason_ = "report_signal_store_checkpoint_events_invalid";
            return false;
        }
        for (size_t i = 0; i < old_events.event_count; ++i) {
            ReportSignalStoreEventCodec::event(
                old_events, i, runtime_->events[runtime_->event_count++]);
        }
    }
    runtime_->active = true;
    return true;
}

bool ReportSignalStoreBuilder::configure_series(
    const ReportSeriesDescriptor &series, const EdfSignalScale &scale) {
    const size_t index = static_cast<size_t>(series.signal);
    if (!runtime_ || index >= static_cast<size_t>(ReportSignalId::Count)) {
        return false;
    }
    if (scale.digital_min == INT16_MIN && scale.digital_max == INT16_MAX) {
        failure_reason_ = "report_signal_store_no_missing_value";
        return false;
    }

    runtime_->scales[index] = scale;
    runtime_->configured[index] = true;
    runtime_->last_track = SIZE_MAX;
    return true;
}

bool ReportSignalStoreBuilder::accept_series(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    const ReportSeriesSample &sample) {
    if (!runtime_ || !runtime_->active || !runtime_->plan) {
        failure_reason_ = "report_signal_store_series_context_invalid";
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    const uint32_t signal_bit = report_signal_bit(series.signal);
    if (!session || signal_bit == 0 || series.sample_interval_ms < 40 ||
        (REPORT_SIGNAL_STORE_BLOCK_MS % series.sample_interval_ms) != 0 ||
        sample.timestamp_ms < session->output_window.start_ms ||
        sample.timestamp_ms >= session->output_window.end_ms) {
        failure_reason_ = "report_signal_store_series_invalid";
        return false;
    }

    const size_t signal_index = static_cast<size_t>(series.signal);
    const bool original = sample.raw_valid && runtime_->configured[signal_index];
    uint32_t phase = 0;
    bool sequential = false;
    if (runtime_->last_track < runtime_->track_count) {
        const TrackWork &previous = runtime_->tracks[runtime_->last_track];
        sequential = previous.last_accepted_ms >= 0 &&
            previous.track.signal == series.signal &&
            previous.track.sample_interval_ms == series.sample_interval_ms &&
            sample.timestamp_ms - previous.last_accepted_ms ==
                series.sample_interval_ms;
        if (sequential) phase = previous.track.grid_phase_ms;
    }
    if (!sequential) {
        phase = grid_phase(sample.timestamp_ms, series.sample_interval_ms);
    }

    TrackWork *work = nullptr;
    if (runtime_->last_track < runtime_->track_count &&
        runtime_->last_source == series.source &&
        runtime_->last_original == original) {
        TrackWork &previous = runtime_->tracks[runtime_->last_track];
        if (previous.track.signal == series.signal &&
            previous.track.sample_interval_ms == series.sample_interval_ms &&
            previous.track.grid_phase_ms == phase) {
            work = &previous;
        }
    }

    std::optional<ReportSignalStoreTrack> format;
    if (!work) {
        format = track_format(
            series, original ? &runtime_->scales[signal_index] : nullptr,
            sample.timestamp_ms);
        for (size_t i = 0; i < runtime_->track_count; ++i) {
            if (same_track_format(runtime_->tracks[i].track, *format)) {
                work = &runtime_->tracks[i];
                break;
            }
        }
    }

    if (!work) {
        if (runtime_->track_count >= runtime_->track_capacity) {
            const size_t next = std::max<size_t>(8, runtime_->track_capacity * 2);
            if (next > UINT16_MAX || next > SIZE_MAX / sizeof(TrackWork)) {
                failure_reason_ = "report_signal_store_track_capacity_exceeded";
                return false;
            }

            void *storage = Memory::realloc_large(
                runtime_->tracks, next * sizeof(TrackWork), false);
            if (!storage) {
                failure_reason_ = "report_signal_store_track_allocation_failed";
                return false;
            }

            runtime_->tracks = static_cast<TrackWork *>(storage);
            for (size_t i = runtime_->track_capacity; i < next; ++i) {
                new (&runtime_->tracks[i]) TrackWork();
            }
            runtime_->track_capacity = next;
        }

        work = &runtime_->tracks[runtime_->track_count++];
        work->track.sleep_day = runtime_->plan->night().sleep_day;
        work->track.source_revision =
            runtime_->plan->night().source_revision;
        work->track.signal = series.signal;
        work->track.unit = report_signal_store_unit(series.signal);
        work->track.lod_mask = lod_mask(series.sample_interval_ms);
        work->track.block_slot_count = runtime_->block_slot_count;
        work->track.generation = runtime_->store_generation;
        work->track.sample_interval_ms = series.sample_interval_ms;
        work->track.value_scale = format->value_scale;
        work->track.value_offset = format->value_offset;
        work->track.missing_value = format->missing_value;
        work->track.grid_phase_ms = phase;
        work->track.first_block_start_ms =
            runtime_->first_block_start_ms;
        for (size_t i = 0; i + 1 < runtime_->track_count; ++i) {
            const auto &other = runtime_->tracks[i].track;
            if (other.signal == series.signal &&
                other.sample_interval_ms == series.sample_interval_ms) {
                work->track.track_index = std::max<uint16_t>(
                    work->track.track_index, other.track_index + 1);
            }
        }

        work->sessions_seen = static_cast<uint8_t *>(Memory::calloc_large(
            runtime_->plan->session_count(), sizeof(uint8_t), false));

        if (!work->sessions_seen) {
            failure_reason_ = "report_signal_store_session_map_failed";
            return false;
        }
    }

    runtime_->last_track = static_cast<size_t>(work - runtime_->tracks);
    runtime_->last_source = series.source;
    runtime_->last_original = original;
    const float scale = work->track.value_scale;
    const float offset = work->track.value_offset;
    const int16_t missing = work->track.missing_value;

    if (work->recount_samples) {
        work->track.valid_sample_count = 0;
        work->track.expected_sample_count = 0;
        work->track.first_valid_sample_ms = 0;
        work->track.last_valid_sample_ms = 0;
        work->recount_samples = false;
    }

    if (sample.timestamp_ms == work->last_accepted_ms) return true;
    if (sample.timestamp_ms < work->last_accepted_ms) {
        failure_reason_ = "report_signal_store_nonchronological_source";
        return false;
    }
    const uint32_t samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / series.sample_interval_ms);

    size_t slot = work->last_slot;
    uint32_t sample_index = work->next_sample_index;
    if (work->last_accepted_ms < 0 ||
        sample.timestamp_ms - work->last_accepted_ms !=
            series.sample_interval_ms ||
        sample_index >= samples_per_block) {
        const int64_t block_start = align_block_start(sample.timestamp_ms);
        const int64_t slot_value =
            (block_start - runtime_->first_block_start_ms) /
            REPORT_SIGNAL_STORE_BLOCK_MS;
        if (block_start < runtime_->first_block_start_ms || slot_value < 0 ||
            slot_value >= runtime_->block_slot_count) {
            failure_reason_ = "report_signal_store_sample_outside_night";
            return false;
        }
        slot = static_cast<size_t>(slot_value);

        const int64_t first_sample = first_grid_sample(
            block_start, series.sample_interval_ms, phase);
        const int64_t sample_delta = sample.timestamp_ms - first_sample;
        if (sample_delta < 0 || (sample_delta % series.sample_interval_ms) != 0) {
            failure_reason_ = "report_signal_store_sample_grid_invalid";
            return false;
        }
        sample_index = static_cast<uint32_t>(
            sample_delta / series.sample_interval_ms);
        if (sample_index >= samples_per_block) {
            failure_reason_ = "report_signal_store_sample_index_invalid";
            return false;
        }
    }
    const bool metrics_needed =
        report_signal_has_metric_consumer(series.signal);
    const int32_t canonical_value = !sample.raw_valid || metrics_needed
        ? report_series_canonical_value_milli(series, sample.value_milli)
        : 0;

    const long quantized = sample.raw_valid ? sample.raw
        : lround((canonical_value / 1000.0 - offset) / scale);
    if (quantized < INT16_MIN || quantized > INT16_MAX || quantized == missing) {
        failure_reason_ = "report_signal_store_value_invalid";
        return false;
    }
    const int16_t encoded = static_cast<int16_t>(quantized);
    uint8_t encoded_bytes[2];
    edf_write_i16_le(encoded_bytes, encoded);

    return runtime_->append_raw_range(
        session_index, series, *work, slot, sample_index, 1,
        sample.timestamp_ms, encoded_bytes, nullptr,
        metrics_needed ? canonical_value : 0, failure_reason_);
}

bool ReportSignalStoreBuilder::accept_raw_sample(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    int64_t timestamp_ms,
    int16_t raw,
    const EdfSignalScale *scale) {
    ReportSeriesSample sample;
    sample.timestamp_ms = timestamp_ms;
    sample.raw = raw;
    sample.raw_valid = true;
    if (report_signal_has_metric_consumer(series.signal)) {
        const size_t signal_index = static_cast<size_t>(series.signal);
        if (!runtime_ || signal_index >=
                static_cast<size_t>(ReportSignalId::Count) ||
            !runtime_->configured[signal_index]) {
            failure_reason_ = "report_signal_store_series_scale_missing";
            return false;
        }
        sample.value_milli = edf_report_physical_value_milli(
            scale ? *scale : runtime_->scales[signal_index], raw);
    }
    return accept_series(session_index, series, sample);
}

bool ReportSignalStoreBuilder::accept_raw_run(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    const EdfReportSeriesSpan &span,
    size_t begin,
    size_t end) {
    TrackWork &work = runtime_->tracks[runtime_->last_track];
    const uint32_t samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / series.sample_interval_ms);

    // The caller supplies a regular, valid run on this track's time grid.
    // Split only where it crosses a stored 15-minute block.
    while (begin < end) {
        const int64_t timestamp_ms = span.timestamp_at(begin);
        const int64_t block_start = align_block_start(timestamp_ms);
        const size_t slot = static_cast<size_t>(
            (block_start - runtime_->first_block_start_ms) /
                REPORT_SIGNAL_STORE_BLOCK_MS);
        const int64_t first_sample = first_grid_sample(
            block_start, series.sample_interval_ms, work.track.grid_phase_ms);
        const uint32_t sample_index = static_cast<uint32_t>(
            (timestamp_ms - first_sample) / series.sample_interval_ms);
        const uint32_t count = static_cast<uint32_t>(std::min(
            end - begin, static_cast<size_t>(samples_per_block - sample_index)));

        if (!runtime_->append_raw_range(
                session_index, series, work, slot, sample_index, count,
                timestamp_ms, span.data + begin * 2, &span.scale, 0,
                failure_reason_)) {
            return false;
        }
        begin += count;
    }
    return true;
}

bool ReportSignalStoreBuilder::accept_series_span(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    const EdfReportSeriesSpan &span) {
    const size_t signal_index = static_cast<size_t>(series.signal);
    if (!runtime_ || !runtime_->active || !runtime_->plan || !span.valid() ||
        session_index >= runtime_->plan->session_count() ||
        signal_index >= static_cast<size_t>(ReportSignalId::Count) ||
        !runtime_->configured[signal_index] ||
        series.sample_interval_ms < 40 ||
        (REPORT_SIGNAL_STORE_BLOCK_MS % series.sample_interval_ms) != 0) {
        failure_reason_ = "report_signal_store_series_span_invalid";
        return false;
    }

    const auto &window = runtime_->plan->session(session_index)->output_window;
    if (span.timestamp_at(0) < window.start_ms ||
        span.timestamp_at(span.sample_count - 1) >= window.end_ms) {
        failure_reason_ = "report_signal_store_series_invalid";
        return false;
    }

    size_t first_valid = span.sample_count;
    for (uint32_t i = 0; i < span.sample_count; ++i) {
        if (!span.missing_at(i)) {
            first_valid = i;
            break;
        }
    }
    if (first_valid == span.sample_count) return true;

    if (!accept_raw_sample(
            session_index, series, span.timestamp_at(
                static_cast<uint32_t>(first_valid)),
            span.raw_at(static_cast<uint32_t>(first_valid)), &span.scale)) {
        return false;
    }

    const bool regular = static_cast<uint64_t>(span.samples_per_record) *
            series.sample_interval_ms == span.record_duration_ms;
    if (!regular) {
        for (size_t i = first_valid + 1; i < span.sample_count; ++i) {
            if (span.missing_at(static_cast<uint32_t>(i))) continue;
            if (!accept_raw_sample(
                    session_index, series, span.timestamp_at(
                        static_cast<uint32_t>(i)),
                    span.raw_at(static_cast<uint32_t>(i)), &span.scale)) {
                return false;
            }
        }
        return true;
    }

    size_t cursor = first_valid + 1;
    while (cursor < span.sample_count) {
        while (cursor < span.sample_count && span.missing_at(
                   static_cast<uint32_t>(cursor))) {
            ++cursor;
        }
        if (cursor == span.sample_count) break;

        const size_t begin = cursor;
        while (cursor < span.sample_count && !span.missing_at(
                   static_cast<uint32_t>(cursor))) {
            ++cursor;
        }
        if (!accept_raw_run(session_index, series, span, begin, cursor)) {
            return false;
        }
    }
    return true;
}


bool ReportSignalStoreBuilder::flush_blocks(bool include_partial) {
    if (!runtime_ || failure_reason_) return false;
    if (!store_) {
        failure_reason_ = "report_signal_store_writer_unavailable";
        return false;
    }

    if (!include_partial && !runtime_->completed_blocks_pending &&
        runtime_->writing_track == SIZE_MAX) return true;

    if (runtime_->writing_track != SIZE_MAX) {
        store_->poll();
        if (!store_->status().terminal()) return false;
        if (store_->status().state != ReportSignalStoreState::Ready) {
            // The service retains the error until the engine resets it.
            failure_reason_ = store_->status().error;
            return false;
        }

        TrackWork &work = runtime_->tracks[runtime_->writing_track];
        for (size_t i = 0; i < runtime_->writing_count; ++i) {
            const size_t slot = runtime_->writing_slot + i;
            Memory::free(work.raw_blocks[slot]);
            work.raw_blocks[slot] = nullptr;
            runtime_->buffered_raw_bytes -=
                (REPORT_SIGNAL_STORE_BLOCK_MS / work.track.sample_interval_ms) *
                sizeof(int16_t);
            work.written_blocks[slot / 8] |=
                static_cast<uint8_t>(1u << (slot % 8));
        }
        work.file_exists = true;
        runtime_->writing_track = SIZE_MAX;
        runtime_->writing_slot = 0;
        runtime_->writing_count = 0;
        store_->reset();
    }

    for (size_t i = 0; i < runtime_->track_count; ++i) {
        TrackWork &work = runtime_->tracks[i];
        for (size_t slot = 0; slot < runtime_->block_slot_count; ++slot) {
            if (!work.raw_blocks[slot] ||
                (!include_partial &&
                 static_cast<int>(slot) >= work.newest_slot)) {
                continue;
            }

            const bool existing_block =
                (work.written_blocks[slot / 8] & (1u << (slot % 8))) != 0;
            const auto lane =
                runtime_->request.priority == ReportRequestPriority::Foreground
                ? StorageAtomicWriteLane::Foreground
                : StorageAtomicWriteLane::Maintenance;

            const size_t raw_block_bytes =
                (REPORT_SIGNAL_STORE_BLOCK_MS / work.track.sample_interval_ms) *
                sizeof(int16_t);
            const size_t batch_limit = std::min(
                ReportSignalStoreService::MaxWriteBatchBlocks,
                (AC_STORAGE_RANGE_WRITE_MAX_BYTES -
                 ReportSignalStoreFileCodec::HeaderBytes) / raw_block_bytes);
            size_t block_count = 1;
            if (!existing_block) {
                for (; block_count < batch_limit; ++block_count) {
                    const size_t candidate = slot + block_count;
                    if (candidate >= runtime_->block_slot_count ||
                        (!include_partial &&
                         static_cast<int>(candidate) >= work.newest_slot) ||
                        !work.raw_blocks[candidate] ||
                        (work.written_blocks[candidate / 8] &
                         (1u << (candidate % 8)))) {
                        break;
                    }
                }

                if (!include_partial && block_count < batch_limit &&
                    slot + block_count >= static_cast<size_t>(work.newest_slot)) {
                    // Batch by bytes: slow signals need not close a file
                    // after only a few kilobytes. Gaps still end a batch.
                    continue;
                }
            }

            OperationAdmission admitted = OperationAdmission::Rejected;
            if (existing_block) {
                admitted = store_->start_block(
                    work.track, slot, work.raw_blocks[slot], true,
                    work.file_exists, runtime_->request.ticket.generation,
                    lane, include_partial);
            } else {
                int16_t *raw_blocks[
                    ReportSignalStoreService::MaxWriteBatchBlocks] = {};
                for (size_t batch = 0; batch < block_count; ++batch) {
                    raw_blocks[batch] = work.raw_blocks[slot + batch];
                }
                admitted = store_->start_blocks(
                    work.track, slot, raw_blocks, block_count,
                    work.file_exists, runtime_->request.ticket.generation,
                    lane, include_partial);
            }

            if (admitted == OperationAdmission::Busy) return false;
            if (admitted != OperationAdmission::Accepted) {
                failure_reason_ = "report_signal_store_block_write_rejected";
                return false;
            }

            runtime_->writing_track = i;
            runtime_->writing_slot = slot;
            runtime_->writing_count = block_count;
            return false;
        }
    }
    runtime_->completed_blocks_pending = false;
    return true;
}

bool ReportSignalStoreBuilder::ready() {
    return flush_blocks(
        runtime_ && runtime_->buffered_raw_bytes > RAW_BUFFER_BUDGET);
}

bool ReportSignalStoreBuilder::end_operation() { return ready(); }

bool ReportSignalStoreBuilder::accept_event(
    uint16_t session_index,
    const ReportEventRecord &event) {
    if (!runtime_ || !runtime_->active || !runtime_->plan) {
        failure_reason_ = "report_signal_store_event_context_invalid";
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    if (!session || event.code == 0 || event.duration_ms < 0 ||
        !report_event_overlaps_window(
            event,
            session->output_window.start_ms,
            session->output_window.end_ms) ||
        event.start_ms < runtime_->first_block_start_ms ||
        event.start_ms >= runtime_->first_block_start_ms +
            static_cast<int64_t>(runtime_->block_slot_count) *
                REPORT_SIGNAL_STORE_BLOCK_MS) {
        failure_reason_ = "report_signal_store_event_invalid";
        return false;
    }

    if (!runtime_->reserve_events(runtime_->event_count + 1)) {
        failure_reason_ = "report_signal_store_event_allocation_failed";
        return false;
    }
    runtime_->events[runtime_->event_count++] = event;
    return true;
}

bool ReportSignalStoreBuilder::finish_build() {
    if (!runtime_ || !runtime_->active || !runtime_->plan) {
        failure_reason_ = "report_signal_store_finish_state_invalid";
        return false;
    }

    if (!flush_blocks(true)) return false;

    if (runtime_->event_count > 1) {
        std::sort(runtime_->events,
                  runtime_->events + runtime_->event_count,
                  report_event_record_less);
    }
    size_t unique_events = 0;
    for (size_t i = 0; i < runtime_->event_count; ++i) {
        if (unique_events > 0 &&
            report_event_record_equal(runtime_->events[unique_events - 1],
                                      runtime_->events[i])) {
            continue;
        }
        runtime_->events[unique_events++] = runtime_->events[i];
    }
    runtime_->event_count = unique_events;

    ReportEventCounts event_counts;
    uint64_t csr_duration_ms = 0;
    for (size_t i = 0; i < runtime_->event_count; ++i) {
        const ReportEventRecord &event = runtime_->events[i];
        report_night_count_event(event_counts, event);
        if (static_cast<ReportEventCode>(event.code) == ReportEventCode::Csr &&
            event.duration_ms > 0) {
            const uint64_t duration = static_cast<uint64_t>(event.duration_ms);
            csr_duration_ms = csr_duration_ms > UINT64_MAX - duration
                ? UINT64_MAX
                : csr_duration_ms + duration;
        }
    }

    void *bundle_storage = Memory::alloc_large(
        sizeof(ReportSignalStoreBundle), false);
    if (!bundle_storage) {
        failure_reason_ = "report_signal_store_bundle_allocation_failed";
        return false;
    }
    ReportSignalStoreBundle *raw_bundle =
        new (bundle_storage) ReportSignalStoreBundle();
    std::shared_ptr<ReportSignalStoreBundle> bundle(
        raw_bundle, destroy_bundle);
    if (runtime_->track_count > 0 &&
        !bundle->allocate_signals(runtime_->track_count)) {
        failure_reason_ = "report_signal_store_files_allocation_failed";
        return false;
    }

    bundle->sleep_day = runtime_->plan->night().sleep_day;
    bundle->source_revision = runtime_->plan->night().source_revision;
    bundle->generation = runtime_->store_generation;
    bundle->checkpoint_slot = runtime_->checkpoint_slot;

    runtime_->update_expected_coverage();

    ReportBuildCheckpointView previous_checkpoint;
    if (runtime_->previous_checkpoint) {
        ReportBuildCheckpointCodec::decode(
            runtime_->previous_checkpoint->data(),
            runtime_->previous_checkpoint->size(), previous_checkpoint);
    }

    for (size_t i = 0; i < runtime_->track_count; ++i) {
        TrackWork &work = runtime_->tracks[i];
        if (!work.source_present && i < previous_checkpoint.track_count) {
            if (!runtime_->prepare_tail(work) ||
                (work.closed.tail_metrics_size && !work.tail_metrics->restore(
                    work.closed.tail_metrics, work.closed.tail_metrics_size))) {
                failure_reason_ = "report_signal_store_retained_tail_invalid";
                return false;
            }

            work.track.valid_sample_count = work.previous_full.valid_sample_count;
            work.track.first_valid_sample_ms = work.previous_full.first_valid_sample_ms;
            work.track.last_valid_sample_ms = work.previous_full.last_valid_sample_ms;
            if (runtime_->closed_before_ms > previous_checkpoint.closed_before_ms) {
                if (!runtime_->closed_metrics.merge(*work.tail_metrics)) {
                    failure_reason_ = "report_signal_store_metrics_merge_failed";
                    return false;
                }
                work.closed = work.previous_full;
                work.tail_metrics->clear();
            }
        }

        if (work.tail_metrics && !runtime_->metrics.merge(*work.tail_metrics)) {
            failure_reason_ = "report_signal_store_metrics_merge_failed";
            return false;
        }
    }

    for (size_t i = 0; i < runtime_->track_count; ++i) {
        TrackWork &work = runtime_->tracks[i];
        ReportSignalStoreFilePayload &payload = bundle->signals_[i];
        payload.track = work.track;
        runtime_->release_track_blocks(work);
    }

    ReportSignalStoreEventFileData event_data;
    event_data.sleep_day = bundle->sleep_day;
    event_data.source_revision = bundle->source_revision;
    event_data.generation = bundle->generation;
    event_data.first_block_start_ms = runtime_->first_block_start_ms;
    event_data.block_slot_count = runtime_->block_slot_count;
    event_data.events = runtime_->events;
    event_data.event_count = runtime_->event_count;
    bundle->events = ReportSignalStoreEventCodec::encode(event_data);
    if (!bundle->events) {
        failure_reason_ = "report_signal_store_events_encode_failed";
        return false;
    }

    uint64_t duration_ms = 0;
    for (size_t i = 0; i < runtime_->plan->session_count(); ++i) {
        const ReportReadSession *session = runtime_->plan->session(i);
        const uint64_t span = static_cast<uint64_t>(
            session->output_window.end_ms - session->output_window.start_ms);
        if (duration_ms > UINT64_MAX - span) {
            failure_reason_ = "report_signal_store_duration_overflow";
            return false;
        }
        duration_ms += span;
    }

    ReportSignalStoreNight night;
    night.sleep_day = bundle->sleep_day;
    night.source_revision = bundle->source_revision;
    night.day_start_ms = runtime_->plan->night().day_start_ms;
    night.day_end_ms = runtime_->plan->night().day_end_ms;
    night.closed_therapy_duration_ms = duration_ms;
    night.generation = bundle->generation;
    night.flags = runtime_->plan->night().metrics.valid_mask != 0
        ? static_cast<uint32_t>(
              REPORT_SIGNAL_STORE_NIGHT_SUMMARY_AVAILABLE)
        : 0u;
    night.timezone_offset_minutes =
        runtime_->plan->night().timezone_offset_valid
            ? runtime_->plan->night().timezone_offset_minutes : 0;
    night.available_event_mask =
        runtime_->plan->requested_event_mask() &
        ~runtime_->plan->missing_event_mask();
    night.source_flags = runtime_->plan->night().source_flags;
    night.checkpoint_slot = bundle->checkpoint_slot;
    night.event_count = static_cast<uint32_t>(runtime_->event_count);
    night.requested_signal_mask = runtime_->plan->requested_signal_mask();
    night.missing_required_signal_mask =
        runtime_->plan->missing_required_signal_mask();
    night.missing_optional_signal_mask =
        runtime_->plan->missing_optional_signal_mask();
    night.requested_event_mask = runtime_->plan->requested_event_mask();
    night.missing_event_mask = runtime_->plan->missing_event_mask();
    night.events = event_counts;
    report_night_metrics_from_catalog(
        runtime_->plan->night().metrics, night.metrics);
    if (!runtime_->plan->night().metrics.has(
            NightCatalogMetric::DurationMinutes)) {
        const uint64_t minutes = (duration_ms + 30000ULL) / 60000ULL;
        night.metrics.duration_minutes = minutes > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(minutes);
    }
    if (!runtime_->metrics.merge(runtime_->closed_metrics)) {
        failure_reason_ = "report_signal_store_metrics_merge_failed";
        return false;
    }
    report_night_complete_metrics(
        night.metrics,
        night.events,
        runtime_->metrics.finish(),
        night.requested_event_mask,
        night.missing_event_mask,
        csr_duration_ms);
    night.session_count = runtime_->plan->session_count();
    night.track_count = runtime_->track_count;

    // ReportReadSession is not a flat NightCatalogTimeRange array.
    NightCatalogTimeRange *sessions = nullptr;
    if (night.session_count > 0) {
        sessions = static_cast<NightCatalogTimeRange *>(Memory::alloc_large(
            night.session_count * sizeof(NightCatalogTimeRange), false));
        if (!sessions) {
            failure_reason_ = "report_signal_store_sessions_allocation_failed";
            return false;
        }
        for (size_t i = 0; i < night.session_count; ++i) {
            sessions[i] = runtime_->plan->session(i)->output_window;
        }
        night.sessions = sessions;
    }

    ReportSignalStoreTrack *tracks = nullptr;
    if (night.track_count > 0) {
        tracks = static_cast<ReportSignalStoreTrack *>(Memory::alloc_large(
            night.track_count * sizeof(ReportSignalStoreTrack), false));
        if (!tracks) {
            Memory::free(sessions);
            failure_reason_ = "report_signal_store_index_allocation_failed";
            return false;
        }
        for (size_t i = 0; i < night.track_count; ++i) {
            tracks[i] = bundle->signals_[i].track;
        }
        night.tracks = tracks;
    }

    bundle->metadata = ReportSignalStoreNightCodec::encode(night);
    Memory::free(tracks);
    Memory::free(sessions);

    const auto progress = ReportPlanner::capture_progress(
        *runtime_->plan, runtime_->closed_before_ms,
        previous_checkpoint.progress, previous_checkpoint.progress_size);
    const auto metrics = runtime_->closed_metrics.snapshot();
    LargeScratchArray<ReportBuildTrackState> closed_tracks;
    LargeScratchArray<std::shared_ptr<const LargeByteBuffer>> tail_snapshots;
    if (!progress || !metrics ||
        !closed_tracks.allocate(runtime_->track_count) ||
        !tail_snapshots.allocate(runtime_->track_count)) {
        failure_reason_ = "report_signal_store_checkpoint_allocation_failed";
        return false;
    }
    for (size_t i = 0; i < runtime_->track_count; ++i) {
        const TrackWork &work = runtime_->tracks[i];
        auto &closed = *closed_tracks.append();
        closed = work.closed;
        closed.tail_metrics = nullptr;
        closed.tail_metrics_size = 0;
        if (work.tail_metrics) {
            auto &snapshot = *tail_snapshots.append();
            snapshot = work.tail_metrics->snapshot();
            if (!snapshot) {
                failure_reason_ = "report_signal_store_tail_snapshot_failed";
                return false;
            }
            closed.tail_metrics = snapshot->data();
            closed.tail_metrics_size = snapshot->size();
        }
    }

    ReportBuildCheckpoint checkpoint;
    checkpoint.sleep_day = bundle->sleep_day;
    checkpoint.source_revision = bundle->source_revision;
    checkpoint.generation = bundle->generation;
    checkpoint.closed_before_ms = runtime_->closed_before_ms;
    checkpoint.tracks = closed_tracks.data();
    checkpoint.track_count = closed_tracks.size();
    checkpoint.progress = progress->data();
    checkpoint.progress_size = progress->size();
    checkpoint.metrics = metrics->data();
    checkpoint.metrics_size = metrics->size();
    checkpoint.events = bundle->events->data();
    checkpoint.events_size = bundle->events->size();
    bundle->checkpoint = ReportBuildCheckpointCodec::encode(checkpoint);

    if (!bundle->metadata || !bundle->checkpoint || !bundle->valid()) {
        failure_reason_ = "report_signal_store_metadata_encode_failed";
        return false;
    }

    runtime_->completed = std::move(bundle);
    runtime_->clear_work();
    failure_reason_ = nullptr;
    return true;
}

void ReportSignalStoreBuilder::discard_build() {
    if (!runtime_) return;
    if (store_) store_->cancel();
    runtime_->clear_work();
    runtime_->completed.reset();
}

std::shared_ptr<ReportSignalStoreBundle>
ReportSignalStoreBuilder::take_completed() {
    if (!runtime_) return {};
    return std::move(runtime_->completed);
}

}  // namespace aircannect
