#include "report_night_histogram.h"

#include <stdio.h>
#include <string.h>

#include "little_endian.h"

namespace aircannect {
namespace {

constexpr uint16_t Version = 1;
constexpr size_t Count = ReportCalculatedMetrics::Count;
constexpr size_t Bins = ReportMetricAccumulator::HistogramBins;

uint32_t metric_interval(const ReportSignalStoreNightView &night, size_t index) {
    const auto signal = index == 11 ? ReportSignalId::InspiratoryDuration
        : ReportMetricAccumulator::metric_signal(index);
    uint32_t interval = 0;

    for (size_t i = 0; i < night.night.track_count; ++i) {
        ReportSignalStoreTrack track;
        night.track(i, track);
        if (track.signal != signal || !track.valid_sample_count) continue;

        // Legacy pooled histograms cannot separate different sampling cadences.
        if (interval && interval != track.sample_interval_ms) return 0;
        interval = track.sample_interval_ms;
    }
    return interval;
}

}  // namespace

void ReportNightHistogram::bin(size_t entry, uint16_t &index, uint32_t &value) const {
    const auto *p = bins + entry * ReportNightHistogramCodec::BinBytes;
    index = LittleEndian::get_le16(p);
    value = LittleEndian::get_le32(p + 2);
}

bool ReportNightHistogramView::matches(const ReportSignalStoreNight &night) const {
    return sleep_day == night.sleep_day && source_revision == night.source_revision &&
        generation == night.generation && checkpoint_slot == night.checkpoint_slot;
}

std::shared_ptr<const LargeByteBuffer> ReportNightHistogramCodec::encode(
    const ReportMetricAccumulator &metrics, const ReportSignalStoreNightView &night,
    ReportNightHistogramView *accepted) {
    ReportNightHistogramView view;
    view.sleep_day = night.night.sleep_day;
    view.source_revision = night.night.source_revision;
    view.generation = night.night.generation;
    view.checkpoint_slot = night.night.checkpoint_slot;
    ReportMetricAccumulator::Histogram inputs[Count];
    uint16_t mask = 0;
    size_t size = HeaderBytes;

    for (size_t i = 0; i < Count; ++i) {
        auto &h = view.metrics[i];
        if (!metrics.histogram(i, inputs[i])) continue;
        h.interval_ms = metric_interval(night, i);
        if (!h.interval_ms) continue;

        for (size_t bin = 0; bin < Bins; ++bin) h.count += inputs[i].bins[bin] != 0;
        h.sum_milli = inputs[i].sum_milli;
        h.samples = inputs[i].samples;
        h.minimum = inputs[i].minimum;
        h.maximum = inputs[i].maximum;
        h.extrema_valid = inputs[i].extrema_valid;
        mask |= 1u << i;
        size += MetricBytes + h.count * BinBytes;
    }

    auto bytes = LargeByteBuffer::allocate(size);
    if (!bytes) return {};
    uint8_t *p = bytes->data();
    memset(p, 0, HeaderBytes);
    LittleEndian::put_le16(p, Version);
    LittleEndian::put_le16(p + 2, HeaderBytes);
    LittleEndian::put_le32(p + 4, size);
    LittleEndian::put_le32(p + 8, view.sleep_day.epoch_days());
    LittleEndian::put_le32(p + 12, view.generation);
    LittleEndian::put_le64(p + 16, view.source_revision.value());
    LittleEndian::put_le16(p + 24, mask);
    p[26] = view.checkpoint_slot;
    p += HeaderBytes;

    for (size_t i = 0; i < Count; ++i) {
        if (!(mask & (1u << i))) continue;
        auto &h = view.metrics[i];
        LittleEndian::put_le64(p, static_cast<uint64_t>(h.sum_milli));
        LittleEndian::put_le64(p + 8, h.samples);
        LittleEndian::put_le32(p + 16, h.minimum);
        LittleEndian::put_le32(p + 20, h.maximum);
        LittleEndian::put_le32(p + 24, h.interval_ms);
        LittleEndian::put_le16(p + 28, h.count);
        LittleEndian::put_le16(p + 30, h.extrema_valid ? 1 : 0);
        p += MetricBytes;
        h.bins = p;

        for (size_t bin = 0; bin < Bins; ++bin) {
            if (!inputs[i].bins[bin]) continue;
            LittleEndian::put_le16(p, bin);
            LittleEndian::put_le32(p + 2, inputs[i].bins[bin]);
            p += BinBytes;
        }
    }

    auto result = LargeByteBuffer::freeze(std::move(bytes));
    if (result && accepted) *accepted = view;
    return result;
}

bool ReportNightHistogramCodec::decode(const uint8_t *bytes, size_t size,
                                      ReportNightHistogramView &out) {
    out = {};
    if (!bytes || size < HeaderBytes || size > MaxBytes ||
        LittleEndian::get_le16(bytes) != Version ||
        LittleEndian::get_le16(bytes + 2) != HeaderBytes ||
        LittleEndian::get_le32(bytes + 4) != size) return false;

    ReportNightHistogramView view;
    if (!SleepDayId::from_epoch_days(
            static_cast<int32_t>(LittleEndian::get_le32(bytes + 8)), view.sleep_day)) {
        return false;
    }
    view.generation = LittleEndian::get_le32(bytes + 12);
    view.source_revision = SourceRevision(LittleEndian::get_le64(bytes + 16));
    view.checkpoint_slot = bytes[26];
    const uint16_t mask = LittleEndian::get_le16(bytes + 24);
    if (!view.generation || !view.source_revision.valid() ||
        view.checkpoint_slot < 1 || view.checkpoint_slot > 2 ||
        (mask >> Count)) return false;

    size_t offset = HeaderBytes;
    for (size_t i = 0; i < Count; ++i) {
        if (!(mask & (1u << i))) continue;
        if (size - offset < MetricBytes) return false;
        const auto *p = bytes + offset;
        auto &h = view.metrics[i];
        h.sum_milli = static_cast<int64_t>(LittleEndian::get_le64(p));
        h.samples = LittleEndian::get_le64(p + 8);
        h.minimum = static_cast<int32_t>(LittleEndian::get_le32(p + 16));
        h.maximum = static_cast<int32_t>(LittleEndian::get_le32(p + 20));
        h.interval_ms = LittleEndian::get_le32(p + 24);
        h.count = LittleEndian::get_le16(p + 28);
        const uint16_t flags = LittleEndian::get_le16(p + 30);
        h.extrema_valid = flags != 0;
        offset += MetricBytes;
        if (!h.samples || !h.interval_ms || flags > 1 || h.count > Bins ||
            !h.count || h.count * BinBytes > size - offset) return false;
        h.bins = bytes + offset;

        uint16_t previous = 0;
        for (size_t entry = 0; entry < h.count; ++entry) {
            uint16_t bin;
            uint32_t value;
            h.bin(entry, bin, value);
            if (!value || bin >= Bins || (entry && bin <= previous)) return false;
            previous = bin;
        }
        offset += h.count * BinBytes;
    }

    if (offset != size) return false;
    out = view;
    return true;
}

bool ReportNightHistogramCodec::path(const ReportSignalStoreNight &night,
                                     char *out, size_t size) {
    char day[9];
    if (!out || !night.sleep_day.format_yyyymmdd(day, sizeof(day))) return false;
    const int length = snprintf(out, size, "%s/%s/g%08x/statistics.%u.bin",
        REPORT_SIGNAL_STORE_ROOT, day, night.generation, night.checkpoint_slot);
    return length > 0 && static_cast<size_t>(length) < size;
}

}  // namespace aircannect
