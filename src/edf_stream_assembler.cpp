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
        (void)edf_parse_utc_ms(device_start_time,
                               status_.session_start_epoch_ms);
    }
    return true;
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

    status_.frames++;

    for (size_t i = 0; i < frame.signal_count; ++i) {
        const StreamSignalSpan &span = frame.signals[i];
        EdfSignalTarget target;
        if (!edf_signal_target_for_stream(span.id, target)) {
            status_.unknown_signals++;
            continue;
        }

        SeriesBuffer dst = series(target.series);
        if (!dst.values || !dst.present || !dst.valid || !dst.status) {
            set_error("buffer_missing");
            continue;
        }

        const uint32_t source_interval =
            span.sample_interval_ms ? span.sample_interval_ms
                                    : frame.interval_ms;
        const uint32_t target_interval =
            target.sample_ms ? target.sample_ms : source_interval;
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
            if (target_slot == last_target_slot) continue;
            last_target_slot = target_slot;
            if (target_slot >= dst.samples_per_record) continue;

            const bool valid = frame.value_valid(value_index);
            const float value = valid ? frame.values[value_index] : 0.0f;
            store_sample(dst, target.signal_index, record_index,
                         static_cast<uint16_t>(target_slot), valid, value);
        }
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
    publish_record(series);
    series.status->records_completed++;
    status_.records_completed++;
    if (skipped) series.status->records_skipped++;
}

void EdfStreamAssembler::advance_to_record(SeriesBuffer &series,
                                           uint32_t new_record) {
    if (!series.status || new_record <= series.status->current_record) return;

    while (series.status->current_record < new_record) {
        const bool skipped = !record_has_samples(series);
        publish_current_record(series, skipped);
        reset_record(series);
        series.status->current_record++;
    }
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
        publish_current_record(buffer, false);
        reset_record(buffer);
    }
}

void EdfStreamAssembler::store_sample(SeriesBuffer &series,
                                      uint8_t signal_index,
                                      uint32_t record_index,
                                      uint16_t sample_index,
                                      bool valid,
                                      float value) {
    if (signal_index >= series.signal_count ||
        sample_index >= series.samples_per_record || !series.status) {
        return;
    }

    if (record_index < series.status->current_record) return;
    if (record_index != series.status->current_record) {
        advance_to_record(series, record_index);
    }

    const size_t slot =
        static_cast<size_t>(signal_index) * series.samples_per_record +
        sample_index;
    if (bit_get(series.present, slot)) {
        series.status->samples_duplicate++;
        status_.samples_duplicate++;
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
        status_.session_start_epoch_ms = frame_start_ms;
        return true;
    }
    return frame_start_ms >= status_.session_start_epoch_ms;
}

void EdfStreamAssembler::set_error(const char *error) {
    snprintf(status_.last_error, sizeof(status_.last_error),
             "%s", error ? error : "");
}

}  // namespace aircannect
