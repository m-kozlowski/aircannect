#include "report_signal_store.h"

#include <limits.h>
#include <string.h>

#include "checked_size.h"
#include "little_endian.h"

namespace aircannect {
namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

constexpr uint8_t NIGHT_MAGIC[8] = {
    'A', 'C', 'R', 'N', 'I', 'G', '0', '1',
};
constexpr uint8_t EVENT_MAGIC[8] = {
    'A', 'C', 'R', 'E', 'V', 'T', '0', '1',
};
constexpr uint32_t KNOWN_NIGHT_FLAGS =
    REPORT_SIGNAL_STORE_NIGHT_SUMMARY_AVAILABLE |
    REPORT_SIGNAL_STORE_NIGHT_OPEN_SESSION;
constexpr size_t EVENT_BITMAP_OFFSET = 48;

void put_i32(uint8_t *out, int32_t value) {
    put_le32(out, static_cast<uint32_t>(value));
}

void put_i64(uint8_t *out, int64_t value) {
    put_le64(out, static_cast<uint64_t>(value));
}

int32_t get_i32(const uint8_t *in) {
    return static_cast<int32_t>(get_le32(in));
}

int64_t get_i64(const uint8_t *in) {
    return static_cast<int64_t>(get_le64(in));
}

void set_bit(uint8_t *bitmap, size_t index) {
    bitmap[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
}

bool bit(const uint8_t *bitmap, size_t index) {
    return (bitmap[index / 8] & (1u << (index % 8))) != 0;
}

bool night_header_valid(const ReportSignalStoreNight &night) {
    return night.sleep_day.valid() && night.source_revision.valid() &&
           night.generation != 0 && night.day_start_ms > 0 &&
           night.day_end_ms > night.day_start_ms &&
           night.timezone_offset_minutes >= -24 * 60 &&
           night.timezone_offset_minutes <= 24 * 60 &&
           (night.flags & ~KNOWN_NIGHT_FLAGS) == 0 &&
           (night.available_event_mask & ~REPORT_EVENT_ALL) == 0 &&
           night.session_count <= UINT16_MAX &&
           night.track_count <= UINT16_MAX && night.checkpoint_slot <= 2;
}

bool night_data_valid(const ReportSignalStoreNight &night) {
    return night_header_valid(night) &&
           (night.session_count == 0 || night.sessions) &&
           (night.track_count == 0 || night.tracks);
}

bool sessions_valid(const ReportSignalStoreNight &night) {
    uint64_t duration = 0;
    int64_t previous_end = 0;
    for (size_t i = 0; i < night.session_count; ++i) {
        const NightCatalogTimeRange &session = night.sessions[i];
        if (!session.valid() || session.start_ms < night.day_start_ms ||
            session.end_ms > night.day_end_ms ||
            (i > 0 && session.start_ms < previous_end)) {
            return false;
        }

        const uint64_t span = static_cast<uint64_t>(
            session.end_ms - session.start_ms);
        if (duration > UINT64_MAX - span) return false;
        duration += span;
        previous_end = session.end_ms;
    }
    return duration == night.closed_therapy_duration_ms;
}

bool tracks_valid(const ReportSignalStoreNight &night) {
    for (size_t i = 0; i < night.track_count; ++i) {
        const ReportSignalStoreTrack &track = night.tracks[i];
        if (!report_signal_store_track_valid(track) ||
            track.sleep_day != night.sleep_day ||
            track.source_revision != night.source_revision ||
            track.generation != night.generation) {
            return false;
        }
        for (size_t n = 0; n < i; ++n) {
            const ReportSignalStoreTrack &other = night.tracks[n];
            if (track.signal == other.signal &&
                track.sample_interval_ms == other.sample_interval_ms &&
                track.track_index == other.track_index) {
                return false;
            }
        }
    }
    return true;
}

void encode_track(uint8_t *out, const ReportSignalStoreTrack &track) {
    out[0] = static_cast<uint8_t>(track.signal);
    out[1] = static_cast<uint8_t>(track.encoding);
    out[2] = static_cast<uint8_t>(track.unit);
    out[3] = track.lod_mask;
    put_le16(out + 4, track.track_index);
    put_le16(out + 6, track.block_slot_count);
    put_le16(out + 8, track.present_block_count);
    put_le32(out + 12, track.sample_interval_ms);

    uint32_t scale_bits = 0;
    uint32_t offset_bits = 0;
    static_assert(sizeof(float) == sizeof(uint32_t), "float must be 32 bits");
    memcpy(&scale_bits, &track.value_scale, sizeof(scale_bits));
    memcpy(&offset_bits, &track.value_offset, sizeof(offset_bits));
    put_le32(out + 16, scale_bits);
    put_le32(out + 80, offset_bits);
    put_le16(out + 84, static_cast<uint16_t>(track.missing_value));

    put_le32(out + 20, track.grid_phase_ms);
    put_i64(out + 24, track.first_block_start_ms);
    put_i64(out + 32, track.first_valid_sample_ms);
    put_i64(out + 40, track.last_valid_sample_ms);
    put_le64(out + 48, track.valid_sample_count);
    put_le64(out + 56, track.expected_sample_count);
    memcpy(out + 64,
           track.present_blocks,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);
}

bool decode_track(const uint8_t *in,
                  const ReportSignalStoreNight &night,
                  ReportSignalStoreTrack &track) {
    track = {};
    track.sleep_day = night.sleep_day;
    track.source_revision = night.source_revision;
    track.generation = night.generation;
    track.signal = static_cast<ReportSignalId>(in[0]);
    track.encoding = static_cast<ReportSignalStoreEncoding>(in[1]);
    track.unit = static_cast<ReportSignalStoreUnit>(in[2]);
    track.lod_mask = in[3];
    track.track_index = get_le16(in + 4);
    track.block_slot_count = get_le16(in + 6);
    track.present_block_count = get_le16(in + 8);
    track.sample_interval_ms = get_le32(in + 12);

    const uint32_t scale_bits = get_le32(in + 16);
    const uint32_t offset_bits = get_le32(in + 80);
    memcpy(&track.value_scale, &scale_bits, sizeof(scale_bits));
    memcpy(&track.value_offset, &offset_bits, sizeof(offset_bits));
    track.missing_value = static_cast<int16_t>(get_le16(in + 84));

    track.grid_phase_ms = get_le32(in + 20);
    track.first_block_start_ms = get_i64(in + 24);
    track.first_valid_sample_ms = get_i64(in + 32);
    track.last_valid_sample_ms = get_i64(in + 40);
    track.valid_sample_count = get_le64(in + 48);
    track.expected_sample_count = get_le64(in + 56);
    memcpy(track.present_blocks,
           in + 64,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);
    return report_signal_store_track_valid(track);
}

bool event_data_valid(const ReportSignalStoreEventFileData &data) {
    if (!data.sleep_day.valid() || !data.source_revision.valid() ||
        data.generation == 0 || data.first_block_start_ms <= 0 ||
        (data.first_block_start_ms % REPORT_SIGNAL_STORE_BLOCK_MS) != 0 ||
        data.block_slot_count == 0 ||
        data.block_slot_count > REPORT_SIGNAL_STORE_MAX_BLOCKS ||
        data.event_count > UINT32_MAX ||
        (data.event_count > 0 && !data.events)) {
        return false;
    }

    const int64_t end_ms = data.first_block_start_ms +
        static_cast<int64_t>(data.block_slot_count) *
            REPORT_SIGNAL_STORE_BLOCK_MS;
    for (size_t i = 0; i < data.event_count; ++i) {
        const ReportEventRecord &event = data.events[i];
        if (event.start_ms < data.first_block_start_ms ||
            event.start_ms >= end_ms || event.duration_ms < 0 ||
            event.code == 0 ||
            (i > 0 &&
             !report_event_record_less(data.events[i - 1], event) &&
             !report_event_record_equal(data.events[i - 1], event))) {
            return false;
        }
    }
    return true;
}

bool event_slot(int64_t first_block_start_ms,
                uint16_t block_slot_count,
                int64_t event_start_ms,
                size_t &slot) {
    if (event_start_ms < first_block_start_ms) return false;
    const int64_t candidate =
        (event_start_ms - first_block_start_ms) /
        REPORT_SIGNAL_STORE_BLOCK_MS;
    if (candidate < 0 || candidate >= block_slot_count) return false;
    slot = static_cast<size_t>(candidate);
    return true;
}

}  // namespace

bool ReportSignalStoreNightView::session(
    size_t index,
    NightCatalogTimeRange &range) const {
    range = {};
    if (!session_records || index >= night.session_count) return false;

    const uint8_t *record = session_records + index *
        ReportSignalStoreNightCodec::SessionBytes;
    range.start_ms = get_i64(record);
    range.end_ms = get_i64(record + 8);
    return range.valid();
}

bool ReportSignalStoreNightView::track(
    size_t index,
    ReportSignalStoreTrack &track) const {
    track = {};
    return track_records && index < night.track_count &&
           decode_track(track_records + index *
                            ReportSignalStoreNightCodec::TrackBytes,
                        night,
                        track);
}

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreNightCodec::encode(
    const ReportSignalStoreNight &night) {
    if (!night_data_valid(night) || !sessions_valid(night) ||
        !tracks_valid(night)) {
        return {};
    }

    size_t total_bytes = HeaderBytes;
    if (!CheckedSize::add_array(total_bytes,
                                night.session_count,
                                SessionBytes) ||
        !CheckedSize::add_array(total_bytes,
                                night.track_count,
                                TrackBytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    memcpy(bytes, NIGHT_MAGIC, sizeof(NIGHT_MAGIC));
    put_le16(bytes + 8, Version);
    put_le16(bytes + 10, HeaderBytes);
    put_le32(bytes + 12, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 16, night.sleep_day.epoch_days());
    put_le32(bytes + 20, night.generation);
    put_le64(bytes + 24, night.source_revision.value());
    put_i64(bytes + 32, night.day_start_ms);
    put_i64(bytes + 40, night.day_end_ms);
    put_le64(bytes + 48, night.closed_therapy_duration_ms);
    put_le32(bytes + 56, night.flags);
    put_i32(bytes + 60, night.timezone_offset_minutes);
    put_le16(bytes + 64, static_cast<uint16_t>(night.session_count));
    put_le16(bytes + 66, static_cast<uint16_t>(night.track_count));
    bytes[68] = night.available_event_mask;
    bytes[69] = night.source_flags;
    bytes[70] = night.checkpoint_slot;
    put_le32(bytes + 72, night.event_count);
    put_le32(bytes + 80, night.requested_signal_mask);
    put_le32(bytes + 84, night.missing_required_signal_mask);
    put_le32(bytes + 88, night.missing_optional_signal_mask);
    bytes[92] = night.requested_event_mask;
    bytes[93] = night.missing_event_mask;

    const ReportNightMetrics &metrics = night.metrics;
    put_le32(bytes + 96, metrics.valid_mask);
    put_le32(bytes + 100, metrics.str_mask);
    put_le32(bytes + 104, metrics.summary_mask);
    put_i32(bytes + 108, metrics.leak_mean_milli);
    put_i32(bytes + 112, metrics.ahi_milli);
    put_i32(bytes + 116, metrics.obstructive_apnea_index_milli);
    put_i32(bytes + 120, metrics.central_apnea_index_milli);
    put_i32(bytes + 124, metrics.unknown_apnea_index_milli);
    put_i32(bytes + 128, metrics.hypopnea_index_milli);
    put_i32(bytes + 132, metrics.arousal_index_milli);
    put_i32(bytes + 136, metrics.mask_pressure_50_milli);
    put_i32(bytes + 140, metrics.leak_50_milli);
    put_le32(bytes + 144, metrics.duration_minutes);
    put_i32(bytes + 148, metrics.mask_pressure_95_milli);
    put_i32(bytes + 152, metrics.leak_95_milli);
    put_i32(bytes + 156, metrics.minute_ventilation_50_milli);
    put_i32(bytes + 160, metrics.minute_ventilation_95_milli);
    put_i32(bytes + 164, metrics.respiratory_rate_50_milli);
    put_i32(bytes + 168, metrics.respiratory_rate_95_milli);
    put_i32(bytes + 172, metrics.tidal_volume_50_milli);
    put_i32(bytes + 176, metrics.tidal_volume_95_milli);
    put_i32(bytes + 180, metrics.spo2_median_milli);
    put_le32(bytes + 184, metrics.spo2_threshold_minutes);
    put_le32(bytes + 188, metrics.csr_minutes);
    put_i32(bytes + 76, metrics.ipap_mean_milli);
    put_i32(bytes + 216, metrics.ipap_50_milli);
    put_i32(bytes + 220, metrics.ipap_95_milli);

    put_le32(bytes + 192, night.events.hypopnea);
    put_le32(bytes + 196, night.events.central_apnea);
    put_le32(bytes + 200, night.events.obstructive_apnea);
    put_le32(bytes + 204, night.events.unknown_apnea);
    put_le32(bytes + 208, night.events.arousal);
    put_le32(bytes + 212, night.events.csr);

    uint8_t *session_records = bytes + HeaderBytes;
    for (size_t i = 0; i < night.session_count; ++i) {
        put_i64(session_records + i * SessionBytes,
                night.sessions[i].start_ms);
        put_i64(session_records + i * SessionBytes + 8,
                night.sessions[i].end_ms);
    }

    uint8_t *track_records = session_records +
        night.session_count * SessionBytes;
    for (size_t i = 0; i < night.track_count; ++i) {
        encode_track(track_records + i * TrackBytes, night.tracks[i]);
    }
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportSignalStoreNightCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportSignalStoreNightView &view) {
    view = {};
    if (!bytes || length < HeaderBytes ||
        memcmp(bytes, NIGHT_MAGIC, sizeof(NIGHT_MAGIC)) != 0 ||
        get_le16(bytes + 8) != Version ||
        get_le16(bytes + 10) != HeaderBytes ||
        get_le32(bytes + 12) != length) {
        return false;
    }

    ReportSignalStoreNight night;
    if (!SleepDayId::from_epoch_days(get_i32(bytes + 16),
                                     night.sleep_day)) {
        return false;
    }
    night.generation = get_le32(bytes + 20);
    night.source_revision = SourceRevision(get_le64(bytes + 24));
    night.day_start_ms = get_i64(bytes + 32);
    night.day_end_ms = get_i64(bytes + 40);
    night.closed_therapy_duration_ms = get_le64(bytes + 48);
    night.flags = get_le32(bytes + 56);
    night.timezone_offset_minutes = get_i32(bytes + 60);
    night.session_count = get_le16(bytes + 64);
    night.track_count = get_le16(bytes + 66);
    night.available_event_mask = bytes[68];
    night.source_flags = bytes[69];
    night.checkpoint_slot = bytes[70];
    if (night.checkpoint_slot > 2) return false;
    night.event_count = get_le32(bytes + 72);
    night.requested_signal_mask = get_le32(bytes + 80);
    night.missing_required_signal_mask = get_le32(bytes + 84);
    night.missing_optional_signal_mask = get_le32(bytes + 88);
    night.requested_event_mask = bytes[92];
    night.missing_event_mask = bytes[93];

    ReportNightMetrics &metrics = night.metrics;
    metrics.valid_mask = get_le32(bytes + 96);
    metrics.str_mask = get_le32(bytes + 100);
    metrics.summary_mask = get_le32(bytes + 104);
    metrics.leak_mean_milli = get_i32(bytes + 108);
    metrics.ahi_milli = get_i32(bytes + 112);
    metrics.obstructive_apnea_index_milli = get_i32(bytes + 116);
    metrics.central_apnea_index_milli = get_i32(bytes + 120);
    metrics.unknown_apnea_index_milli = get_i32(bytes + 124);
    metrics.hypopnea_index_milli = get_i32(bytes + 128);
    metrics.arousal_index_milli = get_i32(bytes + 132);
    metrics.mask_pressure_50_milli = get_i32(bytes + 136);
    metrics.leak_50_milli = get_i32(bytes + 140);
    metrics.duration_minutes = get_le32(bytes + 144);
    metrics.mask_pressure_95_milli = get_i32(bytes + 148);
    metrics.leak_95_milli = get_i32(bytes + 152);
    metrics.minute_ventilation_50_milli = get_i32(bytes + 156);
    metrics.minute_ventilation_95_milli = get_i32(bytes + 160);
    metrics.respiratory_rate_50_milli = get_i32(bytes + 164);
    metrics.respiratory_rate_95_milli = get_i32(bytes + 168);
    metrics.tidal_volume_50_milli = get_i32(bytes + 172);
    metrics.tidal_volume_95_milli = get_i32(bytes + 176);
    metrics.spo2_median_milli = get_i32(bytes + 180);
    metrics.spo2_threshold_minutes = get_le32(bytes + 184);
    metrics.csr_minutes = get_le32(bytes + 188);
    metrics.ipap_mean_milli = get_i32(bytes + 76);
    metrics.ipap_50_milli = get_i32(bytes + 216);
    metrics.ipap_95_milli = get_i32(bytes + 220);

    night.events.hypopnea = get_le32(bytes + 192);
    night.events.central_apnea = get_le32(bytes + 196);
    night.events.obstructive_apnea = get_le32(bytes + 200);
    night.events.unknown_apnea = get_le32(bytes + 204);
    night.events.arousal = get_le32(bytes + 208);
    night.events.csr = get_le32(bytes + 212);
    if (!night_header_valid(night)) return false;

    size_t expected = HeaderBytes;
    if (!CheckedSize::add_array(expected,
                                night.session_count,
                                SessionBytes) ||
        !CheckedSize::add_array(expected,
                                night.track_count,
                                TrackBytes) ||
        expected != length) {
        return false;
    }

    view.night = night;
    view.session_records = bytes + HeaderBytes;
    view.track_records = view.session_records +
        night.session_count * SessionBytes;

    uint64_t duration = 0;
    int64_t previous_end = 0;
    for (size_t i = 0; i < night.session_count; ++i) {
        NightCatalogTimeRange session;
        if (!view.session(i, session) ||
            session.start_ms < night.day_start_ms ||
            session.end_ms > night.day_end_ms ||
            (i > 0 && session.start_ms < previous_end)) {
            view = {};
            return false;
        }
        duration += static_cast<uint64_t>(
            session.end_ms - session.start_ms);
        previous_end = session.end_ms;
    }
    if (duration != night.closed_therapy_duration_ms) {
        view = {};
        return false;
    }

    for (size_t i = 0; i < night.track_count; ++i) {
        ReportSignalStoreTrack track;
        if (!view.track(i, track)) {
            view = {};
            return false;
        }
        for (size_t n = 0; n < i; ++n) {
            ReportSignalStoreTrack other;
            if (!view.track(n, other) ||
                (track.signal == other.signal &&
                 track.sample_interval_ms == other.sample_interval_ms &&
                 track.track_index == other.track_index)) {
                view = {};
                return false;
            }
        }
    }
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportSignalStoreEventCodec::encode(
    const ReportSignalStoreEventFileData &data) {
    if (!event_data_valid(data)) return {};

    size_t total_bytes = HeaderBytes;
    if (!CheckedSize::add_array(total_bytes,
                                data.block_slot_count,
                                BlockDirectoryBytes) ||
        !CheckedSize::add_array(total_bytes,
                                data.event_count,
                                EventBytes) ||
        total_bytes > UINT32_MAX) {
        return {};
    }

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total_bytes);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total_bytes);
    memcpy(bytes, EVENT_MAGIC, sizeof(EVENT_MAGIC));
    put_le16(bytes + 8, Version);
    put_le16(bytes + 10, HeaderBytes);
    put_le32(bytes + 12, static_cast<uint32_t>(total_bytes));
    put_i32(bytes + 16, data.sleep_day.epoch_days());
    put_le32(bytes + 20, data.generation);
    put_le64(bytes + 24, data.source_revision.value());
    put_i64(bytes + 32, data.first_block_start_ms);
    put_le16(bytes + 40, data.block_slot_count);
    put_le32(bytes + 44, static_cast<uint32_t>(data.event_count));

    uint8_t *directory = bytes + HeaderBytes;
    uint8_t *records = directory +
        data.block_slot_count * BlockDirectoryBytes;
    size_t cursor = 0;
    for (size_t slot = 0; slot < data.block_slot_count; ++slot) {
        const size_t first = cursor;
        while (cursor < data.event_count) {
            size_t event_block = 0;
            if (!event_slot(data.first_block_start_ms,
                            data.block_slot_count,
                            data.events[cursor].start_ms,
                            event_block)) {
                return {};
            }
            if (event_block != slot) break;
            ++cursor;
        }

        const size_t count = cursor - first;
        put_le32(directory + slot * BlockDirectoryBytes,
                 static_cast<uint32_t>(first));
        put_le32(directory + slot * BlockDirectoryBytes + 4,
                 static_cast<uint32_t>(count));
        if (count != 0) set_bit(bytes + EVENT_BITMAP_OFFSET, slot);
    }
    if (cursor != data.event_count) return {};

    for (size_t i = 0; i < data.event_count; ++i) {
        uint8_t *record = records + i * EventBytes;
        put_i64(record, data.events[i].start_ms);
        put_i32(record + 8, data.events[i].duration_ms);
        put_le16(record + 12, data.events[i].code);
        put_le16(record + 14, data.events[i].flags);
    }
    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportSignalStoreEventCodec::inspect(
    const uint8_t *bytes,
    size_t length,
    ReportSignalStoreEventFileView &view) {
    view = {};
    if (!bytes || length < HeaderBytes ||
        memcmp(bytes, EVENT_MAGIC, sizeof(EVENT_MAGIC)) != 0 ||
        get_le16(bytes + 8) != Version ||
        get_le16(bytes + 10) != HeaderBytes ||
        get_le32(bytes + 12) != length) {
        return false;
    }

    if (!SleepDayId::from_epoch_days(get_i32(bytes + 16),
                                     view.sleep_day)) {
        return false;
    }
    view.generation = get_le32(bytes + 20);
    view.source_revision = SourceRevision(get_le64(bytes + 24));
    view.first_block_start_ms = get_i64(bytes + 32);
    view.block_slot_count = get_le16(bytes + 40);
    view.event_count = get_le32(bytes + 44);
    memcpy(view.present_blocks,
           bytes + EVENT_BITMAP_OFFSET,
           REPORT_SIGNAL_STORE_BLOCK_BITMAP_BYTES);
    if (!view.sleep_day.valid() || !view.source_revision.valid() ||
        view.generation == 0 || view.first_block_start_ms <= 0 ||
        (view.first_block_start_ms % REPORT_SIGNAL_STORE_BLOCK_MS) != 0 ||
        view.block_slot_count == 0 ||
        view.block_slot_count > REPORT_SIGNAL_STORE_MAX_BLOCKS) {
        view = {};
        return false;
    }

    size_t expected = HeaderBytes;
    if (!CheckedSize::add_array(expected,
                                view.block_slot_count,
                                BlockDirectoryBytes) ||
        !CheckedSize::add_array(expected,
                                view.event_count,
                                EventBytes) ||
        expected != length) {
        view = {};
        return false;
    }

    view.bytes = bytes;
    view.length = length;
    view.block_directory = bytes + HeaderBytes;
    view.event_records = view.block_directory +
        view.block_slot_count * BlockDirectoryBytes;

    uint32_t cursor = 0;
    ReportEventRecord previous;
    for (size_t slot = 0; slot < view.block_slot_count; ++slot) {
        const uint8_t *entry = view.block_directory +
            slot * BlockDirectoryBytes;
        const uint32_t first = get_le32(entry);
        const uint32_t count = get_le32(entry + 4);
        if (first != cursor || count > view.event_count - cursor ||
            bit(view.present_blocks, slot) != (count != 0)) {
            view = {};
            return false;
        }

        for (uint32_t i = 0; i < count; ++i) {
            ReportEventRecord current;
            if (!event(view, cursor + i, current)) {
                view = {};
                return false;
            }
            size_t event_block = 0;
            if (!event_slot(view.first_block_start_ms,
                            view.block_slot_count,
                            current.start_ms,
                            event_block) ||
                event_block != slot ||
                (cursor + i > 0 &&
                 !report_event_record_less(previous, current) &&
                 !report_event_record_equal(previous, current))) {
                view = {};
                return false;
            }
            previous = current;
        }
        cursor += count;
    }
    if (cursor != view.event_count) {
        view = {};
        return false;
    }
    return true;
}

bool ReportSignalStoreEventCodec::file_size(
    uint16_t block_slot_count,
    uint32_t event_count,
    size_t &size) {
    size = HeaderBytes;
    return block_slot_count > 0 &&
           block_slot_count <= REPORT_SIGNAL_STORE_MAX_BLOCKS &&
           CheckedSize::add_array(
               size, block_slot_count, BlockDirectoryBytes) &&
           CheckedSize::add_array(size, event_count, EventBytes) &&
           size <= UINT32_MAX;
}

bool ReportSignalStoreEventCodec::block(
    const ReportSignalStoreEventFileView &view,
    int64_t block_start_ms,
    uint32_t &first_event,
    uint32_t &event_count) {
    first_event = 0;
    event_count = 0;
    if (!view.block_directory ||
        (block_start_ms % REPORT_SIGNAL_STORE_BLOCK_MS) != 0 ||
        block_start_ms < view.first_block_start_ms) {
        return false;
    }

    const int64_t candidate =
        (block_start_ms - view.first_block_start_ms) /
        REPORT_SIGNAL_STORE_BLOCK_MS;
    if (candidate < 0 || candidate >= view.block_slot_count) return false;

    const uint8_t *entry = view.block_directory +
        static_cast<size_t>(candidate) * BlockDirectoryBytes;
    first_event = get_le32(entry);
    event_count = get_le32(entry + 4);
    return first_event <= view.event_count &&
           event_count <= view.event_count - first_event;
}

bool ReportSignalStoreEventCodec::event(
    const ReportSignalStoreEventFileView &view,
    size_t index,
    ReportEventRecord &event) {
    event = {};
    if (!view.event_records || index >= view.event_count) return false;

    const uint8_t *record = view.event_records + index * EventBytes;
    event.start_ms = get_i64(record);
    event.duration_ms = get_i32(record + 8);
    event.code = get_le16(record + 12);
    event.flags = get_le16(record + 14);
    return event.start_ms > 0 && event.duration_ms >= 0 && event.code != 0;
}

}  // namespace aircannect
