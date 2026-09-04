#include "report_signal_store.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "memory_manager.h"

namespace aircannect {
namespace {

bool signal_valid(ReportSignalId signal) {
    return report_signal_bit(signal) != 0;
}

bool bitmap_bit(const uint8_t *bitmap, size_t index) {
    return (bitmap[index / 8] & (1u << (index % 8))) != 0;
}

bool format_cadence(uint32_t interval_ms, char *out, size_t out_size) {
    if (!out || out_size == 0 || interval_ms == 0) return false;

    constexpr uint32_t MILLIOHZ_PER_HZ = 1000;
    constexpr uint32_t MILLIOHZ_PER_SECOND = 1000 * MILLIOHZ_PER_HZ;
    if ((MILLIOHZ_PER_SECOND % interval_ms) != 0) {
        const int written = snprintf(out, out_size, "%ums", interval_ms);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }

    const uint32_t millihz = MILLIOHZ_PER_SECOND / interval_ms;
    const uint32_t whole = millihz / MILLIOHZ_PER_HZ;
    uint32_t fraction = millihz % MILLIOHZ_PER_HZ;
    if (fraction == 0) {
        const int written = snprintf(out, out_size, "%uhz", whole);
        return written > 0 && static_cast<size_t>(written) < out_size;
    }

    while ((fraction % 10) == 0) fraction /= 10;
    const int written = snprintf(out,
                                 out_size,
                                 "%up%uhz",
                                 whole,
                                 fraction);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

const char *encoding_suffix(ReportSignalStoreEncoding encoding) {
    switch (encoding) {
        case ReportSignalStoreEncoding::Signed16: return "s16";
    }
    return nullptr;
}

}  // namespace

bool ReportSignalStoreFilePayload::valid() const {
    ReportSignalStoreFileView view;
    return bytes && report_signal_store_track_valid(track) &&
           ReportSignalStoreFileCodec::inspect(
               bytes->data(), bytes->size(), view) &&
           view.track.signal == track.signal &&
           view.track.sample_interval_ms == track.sample_interval_ms &&
           view.track.grid_phase_ms == track.grid_phase_ms &&
           view.track.track_index == track.track_index;
}

bool ReportSignalStoreFilePayload::path(char *out, size_t out_size) const {
    return valid() &&
           report_signal_store_signal_path(track, out, out_size);
}

ReportSignalStoreBundle::~ReportSignalStoreBundle() {
    for (size_t i = 0; i < signal_count_; ++i) {
        signals_[i].~ReportSignalStoreFilePayload();
    }
    Memory::free(signals_);
}

const ReportSignalStoreFilePayload *ReportSignalStoreBundle::signal(
    size_t index) const {
    return index < signal_count_ ? &signals_[index] : nullptr;
}

bool ReportSignalStoreBundle::allocate_signals(size_t count) {
    if (signals_ || signal_count_ != 0 || count == 0 ||
        count > SIZE_MAX / sizeof(ReportSignalStoreFilePayload)) {
        return false;
    }

    signals_ = static_cast<ReportSignalStoreFilePayload *>(
        Memory::alloc_large(
            count * sizeof(ReportSignalStoreFilePayload), false));
    if (!signals_) return false;

    for (size_t i = 0; i < count; ++i) {
        new (&signals_[i]) ReportSignalStoreFilePayload();
    }
    signal_count_ = count;
    return true;
}

void ReportSignalStoreBundle::release_signal_bytes(size_t index) {
    if (index < signal_count_) signals_[index].bytes.reset();
}

void ReportSignalStoreBundle::release_events() {
    events.reset();
}

bool ReportSignalStoreBundle::valid() const {
    if (!sleep_day.valid() || !source_revision.valid() || generation == 0 ||
        !metadata || !events || (signal_count_ > 0 && !signals_)) {
        return false;
    }

    ReportSignalStoreNightView night;
    if (!ReportSignalStoreNightCodec::decode(
            metadata->data(), metadata->size(), night) ||
        night.night.sleep_day != sleep_day ||
        night.night.source_revision != source_revision ||
        night.night.generation != generation ||
        night.night.track_count != signal_count_) {
        return false;
    }

    ReportSignalStoreEventFileView event_view;
    if (!ReportSignalStoreEventCodec::inspect(
            events->data(), events->size(), event_view) ||
        event_view.sleep_day != sleep_day ||
        event_view.source_revision != source_revision ||
        event_view.generation != generation ||
        event_view.event_count != night.night.event_count) {
        return false;
    }

    for (size_t i = 0; i < signal_count_; ++i) {
        ReportSignalStoreTrack indexed;
        if (!signals_[i].valid() || !night.track(i, indexed) ||
            indexed.signal != signals_[i].track.signal ||
            indexed.sample_interval_ms !=
                signals_[i].track.sample_interval_ms ||
            indexed.grid_phase_ms != signals_[i].track.grid_phase_ms ||
            indexed.track_index != signals_[i].track.track_index) {
            return false;
        }
    }
    return true;
}

bool report_signal_store_track_valid(
    const ReportSignalStoreTrack &track) {
    if (!track.sleep_day.valid() || !track.source_revision.valid() ||
        !signal_valid(track.signal) || track.generation == 0 ||
        track.encoding != ReportSignalStoreEncoding::Signed16 ||
        track.unit != report_signal_store_unit(track.signal) ||
        track.value_scale_milli !=
            report_signal_store_value_scale_milli(track.signal) ||
        track.sample_interval_ms == 0 ||
        (REPORT_SIGNAL_STORE_BLOCK_MS % track.sample_interval_ms) != 0 ||
        track.grid_phase_ms >= track.sample_interval_ms ||
        track.first_block_start_ms <= 0 ||
        (track.first_block_start_ms % REPORT_SIGNAL_STORE_BLOCK_MS) != 0 ||
        track.block_slot_count == 0 ||
        track.block_slot_count > REPORT_SIGNAL_STORE_MAX_BLOCKS ||
        track.present_block_count == 0 ||
        track.present_block_count > track.block_slot_count ||
        (track.lod_mask & ~(REPORT_SIGNAL_STORE_LOD_1S |
                            REPORT_SIGNAL_STORE_LOD_10S)) != 0 ||
        track.first_valid_sample_ms < track.first_block_start_ms ||
        track.last_valid_sample_ms < track.first_valid_sample_ms ||
        track.valid_sample_count == 0 ||
        track.expected_sample_count < track.valid_sample_count) {
        return false;
    }
    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_1S) != 0 &&
        track.sample_interval_ms >= 1000) {
        return false;
    }
    if ((track.lod_mask & REPORT_SIGNAL_STORE_LOD_10S) != 0 &&
        track.sample_interval_ms >= 10000) {
        return false;
    }

    size_t present = 0;
    for (size_t i = 0; i < track.block_slot_count; ++i) {
        if (bitmap_bit(track.present_blocks, i)) ++present;
    }
    return present == track.present_block_count;
}

bool report_signal_store_night_path(SleepDayId sleep_day,
                                    char *out,
                                    size_t out_size) {
    char day[9] = {};
    if (!out || !sleep_day.format_yyyymmdd(day, sizeof(day))) return false;

    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s/night.meta",
                                 REPORT_SIGNAL_STORE_ROOT,
                                 day);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool report_signal_store_signal_path(const ReportSignalStoreTrack &track,
                                     char *out,
                                     size_t out_size) {
    char day[9] = {};
    char cadence[24] = {};
    const char *name = report_signal_store_name(track.signal);
    const char *suffix = encoding_suffix(track.encoding);
    if (!out || !name || !name[0] || !suffix ||
        !track.sleep_day.format_yyyymmdd(day, sizeof(day)) ||
        !format_cadence(track.sample_interval_ms,
                        cadence,
                        sizeof(cadence))) {
        return false;
    }

    const int written = track.track_index == 0
        ? snprintf(out,
                   out_size,
                   "%s/%s/g%08x/signals/%s.%s.%s",
                   REPORT_SIGNAL_STORE_ROOT,
                   day,
                   track.generation,
                   name,
                   cadence,
                   suffix)
        : snprintf(out,
                   out_size,
                   "%s/%s/g%08x/signals/%s.%s.%u.%s",
                   REPORT_SIGNAL_STORE_ROOT,
                   day,
                   track.generation,
                   name,
                   cadence,
                   track.track_index,
                   suffix);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

bool report_signal_store_events_path(SleepDayId sleep_day,
                                     uint32_t generation,
                                     char *out,
                                     size_t out_size) {
    char day[9] = {};
    if (!out || generation == 0 ||
        !sleep_day.format_yyyymmdd(day, sizeof(day))) return false;

    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s/g%08x/events.evt",
                                 REPORT_SIGNAL_STORE_ROOT,
                                 day,
                                 generation);
    return written > 0 && static_cast<size_t>(written) < out_size;
}

ReportSignalStoreUnit report_signal_store_unit(ReportSignalId signal) {
    switch (signal) {
        case ReportSignalId::Flow:
        case ReportSignalId::Leak:
        case ReportSignalId::MinuteVentilation:
            return ReportSignalStoreUnit::LitresPerMinute;
        case ReportSignalId::InspiratoryPressure:
        case ReportSignalId::ExpiratoryPressure:
        case ReportSignalId::MaskPressure:
            return ReportSignalStoreUnit::CentimetresWater;
        case ReportSignalId::InspiratoryDuration:
            return ReportSignalStoreUnit::Seconds;
        case ReportSignalId::RespiratoryRate:
            return ReportSignalStoreUnit::BreathsPerMinute;
        case ReportSignalId::IeRatio:
            return ReportSignalStoreUnit::Ratio;
        case ReportSignalId::TidalVolume:
            return ReportSignalStoreUnit::Litres;
        case ReportSignalId::SpO2:
            return ReportSignalStoreUnit::Percent;
        case ReportSignalId::Pulse:
            return ReportSignalStoreUnit::BeatsPerMinute;
        case ReportSignalId::FlowLimitation:
        case ReportSignalId::Snore:
        case ReportSignalId::Invalid:
        case ReportSignalId::Count:
            return ReportSignalStoreUnit::None;
    }
    return ReportSignalStoreUnit::None;
}

uint32_t report_signal_store_value_scale_milli(ReportSignalId signal) {
    switch (signal) {
        case ReportSignalId::Flow:
        case ReportSignalId::InspiratoryPressure:
        case ReportSignalId::ExpiratoryPressure:
        case ReportSignalId::Leak:
        case ReportSignalId::MinuteVentilation:
        case ReportSignalId::MaskPressure:
        case ReportSignalId::RespiratoryRate:
        case ReportSignalId::SpO2:
        case ReportSignalId::Pulse:
            return 10;
        case ReportSignalId::InspiratoryDuration:
        case ReportSignalId::IeRatio:
        case ReportSignalId::FlowLimitation:
        case ReportSignalId::Snore:
        case ReportSignalId::TidalVolume:
            return 1;
        case ReportSignalId::Invalid:
        case ReportSignalId::Count:
            return 0;
    }
    return 0;
}

bool report_signal_store_quantize(ReportSignalId signal,
                                  int32_t value_milli,
                                  int16_t &encoded) {
    const uint32_t scale = report_signal_store_value_scale_milli(signal);
    if (!signal_valid(signal) || scale == 0) return false;

    int64_t quantized = 0;
    if (value_milli >= 0) {
        quantized =
            (static_cast<int64_t>(value_milli) + scale / 2) / scale;
    } else {
        quantized = -((-static_cast<int64_t>(value_milli) + scale / 2) /
                      scale);
    }
    if (quantized <= REPORT_SIGNAL_STORE_MISSING_S16 ||
        quantized > INT16_MAX) {
        return false;
    }

    encoded = static_cast<int16_t>(quantized);
    return true;
}

}  // namespace aircannect
