#include "report_build_checkpoint.h"

#include <stdio.h>
#include <string.h>

#include "checked_size.h"
#include "little_endian.h"
#include "report_signal_store.h"

namespace aircannect {
namespace {

using LittleEndian::get_le16;
using LittleEndian::get_le32;
using LittleEndian::get_le64;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::put_le64;

void put_i64(uint8_t *out, int64_t value) {
    put_le64(out, static_cast<uint64_t>(value));
}

int64_t get_i64(const uint8_t *in) {
    return static_cast<int64_t>(get_le64(in));
}

bool section_input_valid(const uint8_t *data, size_t size) {
    return size == 0 || data != nullptr;
}

bool total_size(const ReportBuildCheckpoint &checkpoint,
                size_t &tail_bytes,
                size_t &total) {
    if (checkpoint.track_count > UINT32_MAX ||
        checkpoint.progress_size > UINT32_MAX ||
        checkpoint.metrics_size > UINT32_MAX ||
        checkpoint.events_size > UINT32_MAX ||
        !section_input_valid(checkpoint.progress, checkpoint.progress_size) ||
        !section_input_valid(checkpoint.metrics, checkpoint.metrics_size) ||
        !section_input_valid(checkpoint.events, checkpoint.events_size) ||
        (checkpoint.track_count > 0 && !checkpoint.tracks)) {
        return false;
    }

    tail_bytes = 0;
    for (size_t i = 0; i < checkpoint.track_count; ++i) {
        const ReportBuildTrackState &track = checkpoint.tracks[i];
        if (track.tail_metrics_size > UINT32_MAX ||
            !section_input_valid(track.tail_metrics,
                                 track.tail_metrics_size) ||
            !CheckedSize::add_to(tail_bytes, track.tail_metrics_size)) {
            return false;
        }
    }

    total = ReportBuildCheckpointCodec::HeaderBytes;
    if (!CheckedSize::add_array(total,
                                checkpoint.track_count,
                                ReportBuildCheckpointCodec::TrackBytes) ||
        !CheckedSize::add_to(total, tail_bytes) ||
        !CheckedSize::add_to(total, checkpoint.progress_size) ||
        !CheckedSize::add_to(total, checkpoint.metrics_size) ||
        !CheckedSize::add_to(total, checkpoint.events_size)) {
        return false;
    }

    return total <= ReportBuildCheckpointCodec::MaxBytes &&
           total <= UINT32_MAX;
}

bool total_size(uint32_t track_count,
                uint32_t tail_bytes,
                uint32_t progress_size,
                uint32_t metrics_size,
                uint32_t events_size,
                size_t &total) {
    total = ReportBuildCheckpointCodec::HeaderBytes;
    return CheckedSize::add_array(
               total,
               static_cast<size_t>(track_count),
               ReportBuildCheckpointCodec::TrackBytes) &&
           CheckedSize::add_to(total, tail_bytes) &&
           CheckedSize::add_to(total, progress_size) &&
           CheckedSize::add_to(total, metrics_size) &&
           CheckedSize::add_to(total, events_size) &&
           total <= ReportBuildCheckpointCodec::MaxBytes &&
           total <= UINT32_MAX;
}

void encode_track(uint8_t *out, const ReportBuildTrackState &track) {
    put_le64(out, track.valid_sample_count);
    put_i64(out + 8, track.first_valid_sample_ms);
    put_i64(out + 16, track.last_valid_sample_ms);
}

}  // namespace

bool ReportBuildCheckpointView::track(
    size_t index, ReportBuildTrackState &track) const {
    track = {};
    if (!track_records || index >= track_count) return false;

    size_t record_offset = 0;
    size_t table_bytes = 0;
    if (!CheckedSize::multiply(
            index, ReportBuildCheckpointCodec::TrackBytes, record_offset) ||
        !CheckedSize::multiply(track_count,
                               ReportBuildCheckpointCodec::TrackBytes,
                               table_bytes)) {
        return false;
    }

    const uint8_t *record = track_records + record_offset;
    track.valid_sample_count = get_le64(record);
    track.first_valid_sample_ms = get_i64(record + 8);
    track.last_valid_sample_ms = get_i64(record + 16);
    const uint32_t tail_offset = get_le32(record + 24);
    const uint32_t tail_size = get_le32(record + 28);
    if (tail_size > 0) {
        const uint8_t *tail_base = track_records + table_bytes;
        track.tail_metrics = tail_base + tail_offset;
        track.tail_metrics_size = tail_size;
    }
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportBuildCheckpointCodec::encode(
    const ReportBuildCheckpoint &checkpoint) {
    if (!checkpoint.sleep_day.valid() ||
        !checkpoint.source_revision.valid() || checkpoint.generation == 0) {
        return {};
    }

    size_t tail_bytes = 0;
    size_t total = 0;
    if (!total_size(checkpoint, tail_bytes, total)) return {};

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(total);
    if (!output) return {};

    uint8_t *bytes = output->data();
    memset(bytes, 0, total);

    put_le16(bytes, Version);
    put_le16(bytes + 2, HeaderBytes);
    put_le32(bytes + 4, static_cast<uint32_t>(total));
    put_le32(bytes + 8,
             static_cast<uint32_t>(checkpoint.sleep_day.epoch_days()));
    put_le32(bytes + 12, checkpoint.generation);
    put_le64(bytes + 16, checkpoint.source_revision.value());
    put_i64(bytes + 24, checkpoint.closed_before_ms);
    put_le32(bytes + 32, static_cast<uint32_t>(checkpoint.track_count));
    put_le32(bytes + 36, static_cast<uint32_t>(tail_bytes));
    put_le32(bytes + 40, static_cast<uint32_t>(checkpoint.progress_size));
    put_le32(bytes + 44, static_cast<uint32_t>(checkpoint.metrics_size));
    put_le32(bytes + 48, static_cast<uint32_t>(checkpoint.events_size));

    uint8_t *track_records = bytes + HeaderBytes;
    size_t tail_cursor = 0;
    for (size_t i = 0; i < checkpoint.track_count; ++i) {
        const ReportBuildTrackState &track = checkpoint.tracks[i];
        uint8_t *record = track_records + i * TrackBytes;
        encode_track(record, track);
        put_le32(record + 24, static_cast<uint32_t>(tail_cursor));
        put_le32(record + 28,
                 static_cast<uint32_t>(track.tail_metrics_size));
        tail_cursor += track.tail_metrics_size;
    }

    uint8_t *tail_records = track_records +
        checkpoint.track_count * TrackBytes;
    tail_cursor = 0;
    for (size_t i = 0; i < checkpoint.track_count; ++i) {
        const ReportBuildTrackState &track = checkpoint.tracks[i];
        if (track.tail_metrics_size > 0) {
            memcpy(tail_records + tail_cursor,
                   track.tail_metrics,
                   track.tail_metrics_size);
        }
        tail_cursor += track.tail_metrics_size;
    }

    uint8_t *progress = tail_records + tail_bytes;
    if (checkpoint.progress_size > 0) {
        memcpy(progress, checkpoint.progress, checkpoint.progress_size);
    }

    uint8_t *metrics = progress + checkpoint.progress_size;
    if (checkpoint.metrics_size > 0) {
        memcpy(metrics, checkpoint.metrics, checkpoint.metrics_size);
    }

    uint8_t *events = metrics + checkpoint.metrics_size;
    if (checkpoint.events_size > 0) {
        memcpy(events, checkpoint.events, checkpoint.events_size);
    }

    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportBuildCheckpointCodec::decode(
    const uint8_t *bytes,
    size_t length,
    ReportBuildCheckpointView &view) {
    view = {};
    if (!bytes || length < HeaderBytes || length > MaxBytes ||
        get_le16(bytes) != Version ||
        get_le16(bytes + 2) != HeaderBytes ||
        get_le32(bytes + 4) != length) {
        return false;
    }

    if (!SleepDayId::from_epoch_days(
            static_cast<int32_t>(get_le32(bytes + 8)), view.sleep_day)) {
        return false;
    }
    view.generation = get_le32(bytes + 12);
    view.source_revision = SourceRevision(get_le64(bytes + 16));
    view.closed_before_ms = get_i64(bytes + 24);
    view.track_count = get_le32(bytes + 32);
    const uint32_t tail_bytes = get_le32(bytes + 36);
    const uint32_t progress_size = get_le32(bytes + 40);
    const uint32_t metrics_size = get_le32(bytes + 44);
    const uint32_t events_size = get_le32(bytes + 48);
    size_t expected = 0;

    if (!view.sleep_day.valid() || !view.source_revision.valid() ||
        view.generation == 0 ||
        !total_size(static_cast<uint32_t>(view.track_count),
                    tail_bytes,
                    progress_size,
                    metrics_size,
                    events_size,
                    expected) ||
        expected != length) {
        view = {};
        return false;
    }

    view.progress_size = progress_size;
    view.metrics_size = metrics_size;
    view.events_size = events_size;

    view.track_records = bytes + HeaderBytes;
    const uint8_t *tail_records = view.track_records +
        view.track_count * TrackBytes;
    for (size_t i = 0; i < view.track_count; ++i) {
        const uint8_t *record = view.track_records + i * TrackBytes;
        const size_t tail_offset = get_le32(record + 24);
        const size_t tail_size = get_le32(record + 28);
        if (tail_offset > tail_bytes ||
            tail_size > static_cast<size_t>(tail_bytes) - tail_offset) {
            view = {};
            return false;
        }
    }

    view.progress = tail_records + tail_bytes;
    view.metrics = view.progress + view.progress_size;
    view.events = view.metrics + view.metrics_size;
    return true;
}

bool report_build_checkpoint_path(SleepDayId sleep_day,
                                  uint32_t generation,
                                  uint8_t slot,
                                  char *out,
                                  size_t out_size) {
    char day[9] = {};
    if (!out || generation == 0 || slot == 0 || slot > 2 ||
        !sleep_day.format_yyyymmdd(day, sizeof(day))) {
        return false;
    }

    const int written = snprintf(out,
                                 out_size,
                                 "%s/%s/g%08x/build.%u.state",
                                 REPORT_SIGNAL_STORE_ROOT,
                                 day,
                                 generation,
                                 static_cast<unsigned>(slot));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace aircannect
