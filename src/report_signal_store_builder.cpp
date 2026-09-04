#include "report_signal_store_builder.h"

#include <algorithm>
#include <limits.h>
#include <new>
#include <string.h>
#include <utility>

#include "large_object.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

constexpr size_t INITIAL_EVENT_CAPACITY = 64;

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
    int16_t *raw_blocks[REPORT_SIGNAL_STORE_MAX_BLOCKS] = {};
    uint8_t *sessions_seen = nullptr;
};

bool track_less(const TrackWork &lhs, const TrackWork &rhs) {
    if (lhs.track.signal != rhs.track.signal) {
        return static_cast<uint8_t>(lhs.track.signal) <
               static_cast<uint8_t>(rhs.track.signal);
    }
    if (lhs.track.sample_interval_ms != rhs.track.sample_interval_ms) {
        return lhs.track.sample_interval_ms < rhs.track.sample_interval_ms;
    }
    return lhs.track.grid_phase_ms < rhs.track.grid_phase_ms;
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
    ReportEventRecord *events = nullptr;
    size_t event_count = 0;
    size_t event_capacity = 0;
    int64_t first_block_start_ms = 0;
    uint16_t block_slot_count = 0;
    uint32_t store_generation = 0;
    bool active = false;
    std::shared_ptr<ReportSignalStoreBundle> completed;

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
        }
        for (size_t i = 0; i < track_capacity; ++i) {
            tracks[i].~TrackWork();
        }
        Memory::free(tracks);
        tracks = nullptr;
        track_count = 0;
        track_capacity = 0;

        Memory::free(events);
        events = nullptr;
        event_count = 0;
        event_capacity = 0;

        request = {};
        plan = nullptr;
        first_block_start_ms = 0;
        block_slot_count = 0;
        store_generation = 0;
        active = false;
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
};

ReportSignalStoreBuilder::ReportSignalStoreBuilder() :
    runtime_(LargeObject::create<Runtime>()) {}

ReportSignalStoreBuilder::~ReportSignalStoreBuilder() {
    if (runtime_) runtime_->clear_work();
    LargeObject::destroy(runtime_);
}

bool ReportSignalStoreBuilder::begin_build(
    const ReportArtifactRequest &request,
    const ReportReadPlan &plan,
    uint32_t store_generation) {
    failure_reason_ = nullptr;
    if (!runtime_) {
        failure_reason_ = "report_signal_store_runtime_unavailable";
        return false;
    }

    runtime_->clear_work();
    runtime_->completed.reset();
    if (!request.ticket.valid() || request.artifact != plan.key() ||
        request.artifact.kind != ReportArtifactKind::Result ||
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

    if (plan.mapping_count() > 0) {
        if (plan.mapping_count() > SIZE_MAX / sizeof(TrackWork)) {
            failure_reason_ = "report_signal_store_track_count_invalid";
            return false;
        }

        runtime_->tracks = static_cast<TrackWork *>(Memory::alloc_large(
            plan.mapping_count() * sizeof(TrackWork), false));
        if (!runtime_->tracks) {
            failure_reason_ = "report_signal_store_track_allocation_failed";
            return false;
        }
        for (size_t i = 0; i < plan.mapping_count(); ++i) {
            new (&runtime_->tracks[i]) TrackWork();
        }
        runtime_->track_capacity = plan.mapping_count();
    }

    runtime_->request = request;
    runtime_->plan = &plan;
    runtime_->first_block_start_ms = first_block;
    runtime_->block_slot_count = static_cast<uint16_t>(slot_count);
    runtime_->store_generation = store_generation;
    runtime_->active = true;
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
    if (!session || signal_bit == 0 || series.sample_interval_ms == 0 ||
        (REPORT_SIGNAL_STORE_BLOCK_MS % series.sample_interval_ms) != 0 ||
        sample.timestamp_ms < session->output_window.start_ms ||
        sample.timestamp_ms >= session->output_window.end_ms) {
        failure_reason_ = "report_signal_store_series_invalid";
        return false;
    }

    const uint32_t phase = grid_phase(
        sample.timestamp_ms, series.sample_interval_ms);
    TrackWork *work = nullptr;
    for (size_t i = 0; i < runtime_->track_count; ++i) {
        ReportSignalStoreTrack &track = runtime_->tracks[i].track;
        if (track.signal == series.signal &&
            track.sample_interval_ms == series.sample_interval_ms &&
            track.grid_phase_ms == phase) {
            work = &runtime_->tracks[i];
            break;
        }
    }

    if (!work) {
        if (runtime_->track_count >= runtime_->track_capacity) {
            failure_reason_ = "report_signal_store_track_capacity_exceeded";
            return false;
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
        work->track.value_scale_milli =
            report_signal_store_value_scale_milli(series.signal);
        work->track.grid_phase_ms = phase;
        work->track.first_block_start_ms =
            runtime_->first_block_start_ms;
        work->sessions_seen = static_cast<uint8_t *>(Memory::calloc_large(
            runtime_->plan->session_count(), sizeof(uint8_t), false));
        if (!work->sessions_seen) {
            failure_reason_ = "report_signal_store_session_map_failed";
            return false;
        }
    }

    const int64_t block_start = align_block_start(sample.timestamp_ms);
    const int64_t slot_value =
        (block_start - runtime_->first_block_start_ms) /
        REPORT_SIGNAL_STORE_BLOCK_MS;
    if (block_start < runtime_->first_block_start_ms || slot_value < 0 ||
        slot_value >= runtime_->block_slot_count) {
        failure_reason_ = "report_signal_store_sample_outside_night";
        return false;
    }
    const size_t slot = static_cast<size_t>(slot_value);

    const uint32_t samples_per_block = static_cast<uint32_t>(
        REPORT_SIGNAL_STORE_BLOCK_MS / series.sample_interval_ms);
    const int64_t first_sample = first_grid_sample(
        block_start, series.sample_interval_ms, phase);
    const int64_t sample_delta = sample.timestamp_ms - first_sample;
    if (sample_delta < 0 ||
        (sample_delta % series.sample_interval_ms) != 0) {
        failure_reason_ = "report_signal_store_sample_grid_invalid";
        return false;
    }
    const uint64_t sample_index = static_cast<uint64_t>(
        sample_delta / series.sample_interval_ms);
    if (sample_index >= samples_per_block) {
        failure_reason_ = "report_signal_store_sample_index_invalid";
        return false;
    }

    int16_t encoded = 0;
    if (!report_signal_store_quantize(
            series.signal,
            report_series_canonical_value_milli(
                series, sample.value_milli),
            encoded)) {
        failure_reason_ = "report_signal_store_value_invalid";
        return false;
    }

    if (!work->raw_blocks[slot]) {
        work->raw_blocks[slot] = static_cast<int16_t *>(Memory::alloc_large(
            static_cast<size_t>(samples_per_block) * sizeof(int16_t), false));
        if (!work->raw_blocks[slot]) {
            failure_reason_ = "report_signal_store_block_allocation_failed";
            return false;
        }
        std::fill_n(work->raw_blocks[slot],
                    samples_per_block,
                    REPORT_SIGNAL_STORE_MISSING_S16);
        work->track.present_blocks[slot / 8] |=
            static_cast<uint8_t>(1u << (slot % 8));
        ++work->track.present_block_count;
    }

    int16_t &target = work->raw_blocks[slot][sample_index];
    if (target != REPORT_SIGNAL_STORE_MISSING_S16) {
        if (target != encoded) {
            failure_reason_ = "report_signal_store_sample_conflict";
            return false;
        }
        return true;
    }
    target = encoded;

    ReportSignalStoreTrack &track = work->track;
    if (track.valid_sample_count == 0) {
        track.first_valid_sample_ms = sample.timestamp_ms;
        track.last_valid_sample_ms = sample.timestamp_ms;
    } else {
        track.first_valid_sample_ms = std::min(
            track.first_valid_sample_ms, sample.timestamp_ms);
        track.last_valid_sample_ms = std::max(
            track.last_valid_sample_ms, sample.timestamp_ms);
    }
    ++track.valid_sample_count;

    if (!work->sessions_seen[session_index]) {
        work->sessions_seen[session_index] = 1;
        const uint64_t count = expected_samples(
            session->output_window, series.sample_interval_ms, phase);
        if (track.expected_sample_count > UINT64_MAX - count) {
            failure_reason_ = "report_signal_store_coverage_overflow";
            return false;
        }
        track.expected_sample_count += count;
    }
    return true;
}

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

    if (runtime_->track_count > 1) {
        std::sort(runtime_->tracks,
                  runtime_->tracks + runtime_->track_count,
                  track_less);
    }
    uint16_t track_index = 0;
    for (size_t i = 0; i < runtime_->track_count; ++i) {
        if (i == 0 ||
            runtime_->tracks[i].track.signal !=
                runtime_->tracks[i - 1].track.signal ||
            runtime_->tracks[i].track.sample_interval_ms !=
                runtime_->tracks[i - 1].track.sample_interval_ms) {
            track_index = 0;
        } else if (track_index == UINT16_MAX) {
            failure_reason_ = "report_signal_store_track_index_overflow";
            return false;
        } else {
            ++track_index;
        }
        runtime_->tracks[i].track.track_index = track_index;
    }

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

    for (size_t i = 0; i < runtime_->track_count; ++i) {
        TrackWork &work = runtime_->tracks[i];
        ReportSignalStoreFileData data;
        data.track = work.track;
        data.raw_block_slots = work.raw_blocks;
        data.raw_block_slot_count = runtime_->block_slot_count;

        ReportSignalStoreFilePayload &payload = bundle->signals_[i];
        payload.track = work.track;
        payload.bytes = ReportSignalStoreFileCodec::encode(data);
        if (!payload.bytes) {
            failure_reason_ = "report_signal_store_file_encode_failed";
            return false;
        }
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
    night.event_count = static_cast<uint32_t>(runtime_->event_count);
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
    if (!bundle->metadata || !bundle->valid()) {
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
    runtime_->clear_work();
    runtime_->completed.reset();
}

std::shared_ptr<ReportSignalStoreBundle>
ReportSignalStoreBuilder::take_completed() {
    if (!runtime_) return {};
    return std::move(runtime_->completed);
}

}  // namespace aircannect
