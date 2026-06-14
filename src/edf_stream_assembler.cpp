#include "edf_stream_assembler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edf_time.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

size_t series_slot_count(size_t signal_count, size_t samples_per_record) {
    return signal_count * samples_per_record;
}

size_t bitset_size(size_t bits) {
    return (bits + 7) / 8;
}

bool bit_get(const uint8_t *bits, size_t index) {
    if (!bits) return false;
    return (bits[index / 8] & static_cast<uint8_t>(1u << (index % 8))) != 0;
}

int64_t abs_i64(int64_t value) {
    return value < 0 ? -value : value;
}

void bit_set(uint8_t *bits, size_t index, bool value) {
    if (!bits) return;
    const uint8_t mask = static_cast<uint8_t>(1u << (index % 8));
    if (value) bits[index / 8] |= mask;
    else bits[index / 8] &= static_cast<uint8_t>(~mask);
}

void *alloc_large_bytes(size_t bytes) {
#ifdef ARDUINO
    return Memory::calloc_large(1, bytes);
#else
    return calloc(1, bytes);
#endif
}

void free_large_bytes(void *ptr) {
#ifdef ARDUINO
    Memory::free(ptr);
#else
    free(ptr);
#endif
}

}  // namespace

bool EdfStreamAssembler::begin() {
    return allocate_buffers();
}

void EdfStreamAssembler::reset() {
    reset_session_counters();
    if (status_.buffers_ready) {
        SeriesBuffer brp = series(EdfSeriesId::Brp);
        SeriesBuffer pld = series(EdfSeriesId::Pld);
        SeriesBuffer sa2 = series(EdfSeriesId::Sa2);
        reset_record(brp);
        reset_record(pld);
        reset_record(sa2);
    }
}

void EdfStreamAssembler::release() {
    free_buffers();
    reset_timeline();
    status_ = {};
}

void EdfStreamAssembler::set_record_observer(EdfRecordObserver observer,
                                             void *context) {
    record_observer_ = observer;
    record_observer_context_ = context;
}

bool EdfStreamAssembler::start_session(const char *device_start_time) {
    if (!allocate_buffers()) return false;
    reset();
    status_.active = true;
    if (device_start_time && *device_start_time) {
        int64_t start_ms = 0;
        if (edf_parse_utc_ms(device_start_time, start_ms)) {
            status_.session_start_epoch_ms =
                edf_floor_epoch_ms_to_second(start_ms);
        }
    }
    return true;
}

void EdfStreamAssembler::set_current_records(uint32_t brp_record,
                                             uint32_t pld_record,
                                             uint32_t sa2_record) {
    if (!status_.buffers_ready) return;
    SeriesBuffer brp = series(EdfSeriesId::Brp);
    SeriesBuffer pld = series(EdfSeriesId::Pld);
    SeriesBuffer sa2 = series(EdfSeriesId::Sa2);
    reset_record(brp);
    reset_record(pld);
    reset_record(sa2);
    if (brp.status) brp.status->current_record = brp_record;
    if (pld.status) pld.status->current_record = pld_record;
    if (sa2.status) sa2.status->current_record = sa2_record;
}

EdfFramePrepareStatus EdfStreamAssembler::prepare_frame(
    const StreamFrameData &frame,
    size_t max_records_to_publish) {
    if (!status_.active) return EdfFramePrepareStatus::Rejected;

    int64_t frame_start_ms = 0;
    if (!parse_frame_start_ms(frame, frame_start_ms) ||
        !ensure_session_epoch(frame_start_ms)) {
        status_.timestamp_errors++;
        set_error("timestamp");
        return EdfFramePrepareStatus::Rejected;
    }
    FrameTiming timing;
    const bool have_timing =
        resolve_frame_timing(frame, frame_start_ms, timing);
    const int64_t effective_start_ms =
        have_timing ? timing.effective_start_ms : frame_start_ms;

    struct TargetRecord {
        bool seen = false;
        uint32_t record = UINT32_MAX;
    };
    TargetRecord targets[3];

    auto target_for_series = [&](EdfSeriesId series) -> TargetRecord & {
        switch (series) {
            case EdfSeriesId::Brp: return targets[0];
            case EdfSeriesId::Pld: return targets[1];
            case EdfSeriesId::Sa2:
            default:
                return targets[2];
        }
    };

    for (size_t i = 0; i < frame.signal_count; ++i) {
        const StreamSignalSpan &span = frame.signals[i];
        EdfSignalTarget target;
        if (!edf_signal_target_for_stream(span.id, target)) continue;

        const uint32_t source_interval =
            span.sample_interval_ms ? span.sample_interval_ms
                                    : frame.interval_ms;
        const uint32_t target_interval =
            target.sample_ms ? target.sample_ms : source_interval;
        if (target_interval == 0) continue;

        for (uint16_t sample = 0; sample < span.sample_count; ++sample) {
            const int64_t sample_ms =
                effective_start_ms +
                static_cast<int64_t>(sample) * source_interval;
            if (sample_ms < status_.session_start_epoch_ms) continue;

            const int64_t relative_ms =
                sample_ms - status_.session_start_epoch_ms;
            const uint32_t record_index =
                static_cast<uint32_t>(relative_ms / AC_EDF_RECORD_MS);
            const uint32_t record_ms =
                static_cast<uint32_t>(relative_ms % AC_EDF_RECORD_MS);
            const uint32_t target_slot = record_ms / target_interval;
            SeriesBuffer dst = series(target.series);
            if (target_slot >= dst.samples_per_record) continue;

            TargetRecord &entry = target_for_series(target.series);
            if (!entry.seen || record_index < entry.record) {
                entry.seen = true;
                entry.record = record_index;
            }
            break;
        }
    }

    size_t budget = max_records_to_publish;
    SeriesBuffer buffers[] = {
        series(EdfSeriesId::Brp),
        series(EdfSeriesId::Pld),
        series(EdfSeriesId::Sa2),
    };
    for (size_t i = 0; i < 3; ++i) {
        if (!targets[i].seen) continue;
        if (!advance_to_record(buffers[i], targets[i].record, &budget)) {
            return EdfFramePrepareStatus::Deferred;
        }
    }
    return EdfFramePrepareStatus::Ready;
}

void EdfStreamAssembler::end_session() {
    flush_partial_records();
    status_.active = false;
}

void EdfStreamAssembler::ingest_frame(const StreamFrameData &frame) {
    if (!status_.active) return;

    int64_t frame_start_ms = 0;
    if (!parse_frame_start_ms(frame, frame_start_ms) ||
        !ensure_session_epoch(frame_start_ms)) {
        status_.timestamp_errors++;
        set_error("timestamp");
        return;
    }
    FrameTiming timing;
    const bool have_timing =
        resolve_frame_timing(frame, frame_start_ms, timing);
    const int64_t effective_start_ms =
        have_timing ? timing.effective_start_ms : frame_start_ms;

    status_.frames++;

    for (size_t i = 0; i < frame.signal_count; ++i) {
        const StreamSignalSpan &span = frame.signals[i];
        EdfSignalTarget target;
        if (!edf_signal_target_for_stream(span.id, target)) {
            status_.unknown_signals++;
        }
    }

    SeriesBuffer buffers[] = {
        series(EdfSeriesId::Brp),
        series(EdfSeriesId::Pld),
        series(EdfSeriesId::Sa2),
    };
    for (SeriesBuffer &buffer : buffers) {
        ingest_series_frame(frame, effective_start_ms, buffer);
    }
    if (have_timing) {
        commit_frame_timing(frame, timing);
    }
}

bool EdfStreamAssembler::allocate_buffers() {
    if (status_.buffers_ready) return true;

    const size_t brp_slots =
        series_slot_count(AC_EDF_BRP_SIGNAL_COUNT,
                          AC_EDF_BRP_SAMPLES_PER_RECORD);
    const size_t pld_slots =
        series_slot_count(AC_EDF_PLD_SIGNAL_COUNT,
                          AC_EDF_PLD_SAMPLES_PER_RECORD);
    const size_t sa2_slots =
        series_slot_count(AC_EDF_SA2_SIGNAL_COUNT,
                          AC_EDF_SA2_SAMPLES_PER_RECORD);

    brp_values_ = static_cast<float *>(alloc_large_bytes(sizeof(float) *
                                                         brp_slots));
    pld_values_ = static_cast<float *>(alloc_large_bytes(sizeof(float) *
                                                         pld_slots));
    sa2_values_ = static_cast<float *>(alloc_large_bytes(sizeof(float) *
                                                         sa2_slots));
    brp_present_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        brp_slots)));
    pld_present_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        pld_slots)));
    sa2_present_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        sa2_slots)));
    brp_valid_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        brp_slots)));
    pld_valid_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        pld_slots)));
    sa2_valid_ = static_cast<uint8_t *>(alloc_large_bytes(bitset_size(
        sa2_slots)));

    if (!brp_values_ || !pld_values_ || !sa2_values_ || !brp_present_ ||
        !pld_present_ || !sa2_present_ || !brp_valid_ || !pld_valid_ ||
        !sa2_valid_) {
        free_buffers();
        set_error("alloc_failed");
        return false;
    }

    status_.buffers_ready = true;
    status_.brp.allocated = true;
    status_.pld.allocated = true;
    status_.sa2.allocated = true;
    reset();
    return true;
}

void EdfStreamAssembler::free_buffers() {
    free_large_bytes(brp_values_);
    free_large_bytes(pld_values_);
    free_large_bytes(sa2_values_);
    free_large_bytes(brp_present_);
    free_large_bytes(pld_present_);
    free_large_bytes(sa2_present_);
    free_large_bytes(brp_valid_);
    free_large_bytes(pld_valid_);
    free_large_bytes(sa2_valid_);
    brp_values_ = nullptr;
    pld_values_ = nullptr;
    sa2_values_ = nullptr;
    brp_present_ = nullptr;
    pld_present_ = nullptr;
    sa2_present_ = nullptr;
    brp_valid_ = nullptr;
    pld_valid_ = nullptr;
    sa2_valid_ = nullptr;
    status_.buffers_ready = false;
}

void EdfStreamAssembler::reset_session_counters() {
    const bool buffers_ready = status_.buffers_ready;
    status_ = {};
    status_.buffers_ready = buffers_ready;
    status_.brp.allocated = buffers_ready;
    status_.pld.allocated = buffers_ready;
    status_.sa2.allocated = buffers_ready;
    reset_timeline();
}

void EdfStreamAssembler::reset_timeline() {
    timeline_active_ = false;
    timeline_stream_id_ = 0;
    timeline_next_frame_start_ms_ = 0;
}

void EdfStreamAssembler::reset_record(SeriesBuffer &series) {
    const size_t slots = series_slot_count(series.signal_count,
                                           series.samples_per_record);
    if (series.values) memset(series.values, 0, sizeof(float) * slots);
    if (series.present) memset(series.present, 0, bitset_size(slots));
    if (series.valid) memset(series.valid, 0, bitset_size(slots));
    if (series.status) series.status->slots_filled = 0;
}

bool EdfStreamAssembler::record_has_samples(
    const SeriesBuffer &series) const {
    return series.status && series.status->slots_filled > 0;
}

bool EdfStreamAssembler::last_present_sample(const SeriesBuffer &series,
                                             uint8_t signal_index,
                                             uint16_t &sample_index) const {
    if (signal_index >= series.signal_count || !series.present) return false;
    const size_t base =
        static_cast<size_t>(signal_index) * series.samples_per_record;
    for (size_t sample = series.samples_per_record; sample > 0; --sample) {
        const size_t index = base + sample - 1;
        if (bit_get(series.present, index)) {
            sample_index = static_cast<uint16_t>(sample - 1);
            return true;
        }
    }
    return false;
}

bool EdfStreamAssembler::record_tail_complete(
    const SeriesBuffer &series) const {
    if (!record_has_samples(series) || series.samples_per_record == 0) {
        return false;
    }

    bool saw_signal = false;
    for (uint8_t signal = 0; signal < series.signal_count; ++signal) {
        uint16_t last = 0;
        if (!last_present_sample(series, signal, last)) continue;
        saw_signal = true;
        const uint16_t final_sample =
            static_cast<uint16_t>(series.samples_per_record - 1);
        if (last != final_sample) return false;
    }
    return saw_signal;
}

uint32_t EdfStreamAssembler::count_missing_record_samples(
    const SeriesBuffer &series) const {
    if (!series.present) return 0;
    const size_t slots = series_slot_count(series.signal_count,
                                           series.samples_per_record);
    uint32_t missing = 0;
    for (size_t slot = 0; slot < slots; ++slot) {
        if (!bit_get(series.present, slot)) missing++;
    }
    return missing;
}

void EdfStreamAssembler::count_late_frame_samples(
    const StreamFrameData &frame,
    int64_t frame_start_ms,
    SeriesBuffer &series) {
    if (!series.status) return;
    const uint32_t current_record = series.status->current_record;
    for (size_t i = 0; i < frame.signal_count; ++i) {
        const StreamSignalSpan &span = frame.signals[i];
        EdfSignalTarget target;
        if (!edf_signal_target_for_stream(span.id, target) ||
            target.series != series.id) {
            continue;
        }

        const uint32_t source_interval =
            span.sample_interval_ms ? span.sample_interval_ms
                                    : frame.interval_ms;
        const uint32_t target_interval =
            target.sample_ms ? target.sample_ms : source_interval;
        if (target_interval == 0) continue;

        for (uint16_t sample = 0; sample < span.sample_count; ++sample) {
            const int64_t sample_ms =
                frame_start_ms +
                static_cast<int64_t>(sample) * source_interval;
            if (sample_ms < status_.session_start_epoch_ms) continue;
            const int64_t relative_ms =
                sample_ms - status_.session_start_epoch_ms;
            const uint32_t record_index =
                static_cast<uint32_t>(relative_ms / AC_EDF_RECORD_MS);
            if (record_index >= current_record) continue;
            const uint32_t record_ms =
                static_cast<uint32_t>(relative_ms % AC_EDF_RECORD_MS);
            const uint32_t target_slot = record_ms / target_interval;
            if (target_slot >= series.samples_per_record) continue;
            series.status->samples_late++;
            status_.samples_late++;
        }
    }
}

void EdfStreamAssembler::ingest_series_frame(const StreamFrameData &frame,
                                             int64_t frame_start_ms,
                                             SeriesBuffer &series) {
    if (!series.values || !series.present || !series.valid || !series.status ||
        series.samples_per_record == 0) {
        set_error("buffer_missing");
        return;
    }

    count_late_frame_samples(frame, frame_start_ms, series);

    while (true) {
        const uint32_t current_record = series.status->current_record;
        bool saw_future = false;
        uint32_t next_record = UINT32_MAX;

        for (size_t i = 0; i < frame.signal_count; ++i) {
            const StreamSignalSpan &span = frame.signals[i];
            EdfSignalTarget target;
            if (!edf_signal_target_for_stream(span.id, target) ||
                target.series != series.id) {
                continue;
            }

            const uint32_t source_interval =
                span.sample_interval_ms ? span.sample_interval_ms
                                        : frame.interval_ms;
            const uint32_t target_interval =
                target.sample_ms ? target.sample_ms : source_interval;
            if (target_interval == 0) continue;
            const bool count_duplicate =
                source_interval == 0 || source_interval >= target_interval;
            uint32_t last_target_slot = UINT32_MAX;

            for (uint16_t sample = 0; sample < span.sample_count; ++sample) {
                const size_t value_index = span.value_offset + sample;
                if (value_index >= frame.value_count) break;

                const int64_t sample_ms =
                    frame_start_ms +
                    static_cast<int64_t>(sample) * source_interval;
                if (sample_ms < status_.session_start_epoch_ms) continue;
                const int64_t relative_ms =
                    sample_ms - status_.session_start_epoch_ms;
                const uint32_t record_index =
                    static_cast<uint32_t>(relative_ms / AC_EDF_RECORD_MS);
                const uint32_t record_ms =
                    static_cast<uint32_t>(relative_ms % AC_EDF_RECORD_MS);
                const uint32_t target_slot = record_ms / target_interval;
                if (target_slot >= series.samples_per_record) continue;

                if (record_index < current_record) continue;
                if (record_index > current_record) {
                    saw_future = true;
                    if (record_index < next_record) next_record = record_index;
                    continue;
                }
                if (target_slot == last_target_slot) continue;
                last_target_slot = target_slot;

                const bool valid = frame.value_valid(value_index);
                const float value = valid ? frame.values[value_index] : 0.0f;
                store_sample(series, target.signal_index, record_index,
                             static_cast<uint16_t>(target_slot), valid, value,
                             count_duplicate);
            }
        }

        if (!saw_future) break;
        if (!advance_to_record(series, next_record)) break;
    }
}

bool EdfStreamAssembler::resolve_frame_timing(
    const StreamFrameData &frame,
    int64_t reported_start_ms,
    FrameTiming &timing) const {
    timing = {};
    timing.reported_start_ms = reported_start_ms;
    timing.effective_start_ms = reported_start_ms;

    uint32_t coverage_ms = 0;
    uint32_t min_interval_ms = UINT32_MAX;

    for (size_t i = 0; i < frame.signal_count; ++i) {
        const StreamSignalSpan &span = frame.signals[i];
        EdfSignalTarget target;
        if (!edf_signal_target_for_stream(span.id, target)) continue;
        (void)target;

        const uint32_t source_interval =
            span.sample_interval_ms ? span.sample_interval_ms
                                    : frame.interval_ms;
        if (source_interval == 0 || span.sample_count == 0) continue;
        if (source_interval < min_interval_ms) {
            min_interval_ms = source_interval;
        }
        const uint64_t span_coverage =
            static_cast<uint64_t>(source_interval) * span.sample_count;
        if (span_coverage > coverage_ms) {
            coverage_ms = span_coverage > UINT32_MAX
                              ? UINT32_MAX
                              : static_cast<uint32_t>(span_coverage);
        }
    }

    if (coverage_ms == 0 || min_interval_ms == UINT32_MAX) {
        return false;
    }

    timing.coverage_ms = coverage_ms;
    timing.tolerance_ms =
        min_interval_ms > 2 ? (min_interval_ms / 2u) - 1u : 0u;
    timing.eligible = true;

    const bool same_stream =
        !timeline_active_ || timeline_stream_id_ == 0 || frame.stream_id == 0 ||
        frame.stream_id == timeline_stream_id_;
    if (!same_stream) {
        return true;
    }

    if (!timeline_active_) return true;

    // AS11 frame startTime wobbles by a few ms around the report cadence.
    // Keep the continuous sample timeline when the error is below half a
    // source sample; larger jumps are treated as real gaps/resyncs.
    const int64_t jitter =
        reported_start_ms - timeline_next_frame_start_ms_;
    if (jitter >= INT32_MIN && jitter <= INT32_MAX) {
        timing.jitter_ms = static_cast<int32_t>(jitter);
    }
    if (abs_i64(jitter) <= timing.tolerance_ms) {
        timing.effective_start_ms = timeline_next_frame_start_ms_;
        timing.corrected = jitter != 0;
    } else {
        timing.resync = true;
    }
    return true;
}

void EdfStreamAssembler::commit_frame_timing(
    const StreamFrameData &frame,
    const FrameTiming &timing) {
    if (!timing.eligible || timing.coverage_ms == 0) return;

    if (timing.corrected) {
        status_.timestamp_jitter_corrections++;
        status_.last_timestamp_jitter_ms = timing.jitter_ms;
    } else if (timing.resync) {
        status_.timestamp_resyncs++;
        status_.last_timestamp_jitter_ms = timing.jitter_ms;
    }

    timeline_active_ = true;
    timeline_stream_id_ = frame.stream_id;
    timeline_next_frame_start_ms_ =
        timing.effective_start_ms + timing.coverage_ms;
    status_.last_sample_epoch_ms = timeline_next_frame_start_ms_;
}

void EdfStreamAssembler::publish_record(const SeriesBuffer &series) {
    if (!record_observer_ || !series.status) return;
    EdfCompletedRecordView record;
    record.series = series.id;
    record.record_index = series.status->current_record;
    record.signal_count = series.signal_count;
    record.samples_per_record = series.samples_per_record;
    record.values = series.values;
    record.present = series.present;
    record.valid = series.valid;
    record_observer_(record_observer_context_, record);
}

void EdfStreamAssembler::publish_current_record(SeriesBuffer &series,
                                                bool skipped) {
    if (!series.status) return;
    const uint32_t missing = count_missing_record_samples(series);
    publish_record(series);
    series.status->records_completed++;
    status_.records_completed++;
    series.status->samples_missing += missing;
    status_.samples_missing += missing;
    if (skipped) series.status->records_skipped++;
}

bool EdfStreamAssembler::advance_to_record(SeriesBuffer &series,
                                           uint32_t new_record,
                                           size_t *publish_budget) {
    if (!series.status || new_record <= series.status->current_record) {
        return true;
    }

    while (series.status->current_record < new_record) {
        if (publish_budget && *publish_budget == 0) return false;
        const bool skipped = !record_has_samples(series);
        publish_current_record(series, skipped);
        if (publish_budget) --(*publish_budget);
        reset_record(series);
        series.status->current_record++;
    }
    return true;
}

void EdfStreamAssembler::flush_partial_records() {
    if (!status_.buffers_ready) return;
    SeriesBuffer buffers[] = {
        series(EdfSeriesId::Brp),
        series(EdfSeriesId::Pld),
        series(EdfSeriesId::Sa2),
    };
    for (SeriesBuffer &buffer : buffers) {
        if (!record_has_samples(buffer)) continue;
        if (record_tail_complete(buffer)) {
            publish_current_record(buffer, false);
        } else if (buffer.status) {
            buffer.status->records_dropped_partial++;
            status_.records_dropped_partial++;
        }
        reset_record(buffer);
    }
}

void EdfStreamAssembler::store_sample(SeriesBuffer &series,
                                      uint8_t signal_index,
                                      uint32_t record_index,
                                      uint16_t sample_index,
                                      bool valid,
                                      float value,
                                      bool count_duplicate) {
    if (signal_index >= series.signal_count ||
        sample_index >= series.samples_per_record || !series.status) {
        return;
    }

    if (record_index < series.status->current_record) {
        series.status->samples_late++;
        status_.samples_late++;
        return;
    }
    if (record_index != series.status->current_record) {
        advance_to_record(series, record_index);
    }

    const size_t slot =
        static_cast<size_t>(signal_index) * series.samples_per_record +
        sample_index;
    if (bit_get(series.present, slot)) {
        if (count_duplicate) {
            series.status->samples_duplicate++;
            status_.samples_duplicate++;
        }
        return;
    }

    bit_set(series.present, slot, true);
    bit_set(series.valid, slot, valid);
    series.values[slot] = value;
    series.status->slots_filled++;

    if (valid) {
        series.status->samples_accepted++;
        status_.samples_accepted++;
    } else {
        series.status->samples_invalid++;
        status_.samples_invalid++;
    }
}

EdfStreamAssembler::SeriesBuffer EdfStreamAssembler::series(EdfSeriesId id) {
    switch (id) {
        case EdfSeriesId::Brp:
            return {id, AC_EDF_BRP_SIGNAL_COUNT,
                    AC_EDF_BRP_SAMPLES_PER_RECORD, AC_EDF_BRP_SAMPLE_MS,
                    brp_values_, brp_present_, brp_valid_, &status_.brp};
        case EdfSeriesId::Pld:
            return {id, AC_EDF_PLD_SIGNAL_COUNT,
                    AC_EDF_PLD_SAMPLES_PER_RECORD, AC_EDF_PLD_SAMPLE_MS,
                    pld_values_, pld_present_, pld_valid_, &status_.pld};
        case EdfSeriesId::Sa2:
        default:
            return {id, AC_EDF_SA2_SIGNAL_COUNT,
                    AC_EDF_SA2_SAMPLES_PER_RECORD, AC_EDF_SA2_SAMPLE_MS,
                    sa2_values_, sa2_present_, sa2_valid_, &status_.sa2};
    }
}

bool EdfStreamAssembler::parse_frame_start_ms(const StreamFrameData &frame,
                                              int64_t &start_ms) {
    return edf_parse_utc_ms(frame.start_time, start_ms);
}

bool EdfStreamAssembler::ensure_session_epoch(int64_t frame_start_ms) {
    if (status_.session_start_epoch_ms == 0) {
        status_.session_start_epoch_ms =
            edf_floor_epoch_ms_to_second(frame_start_ms);
    }
    return true;
}

void EdfStreamAssembler::set_error(const char *error) {
    snprintf(status_.last_error, sizeof(status_.last_error),
             "%s", error ? error : "");
}

}  // namespace aircannect
