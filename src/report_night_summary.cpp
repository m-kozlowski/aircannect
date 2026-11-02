#include "report_night_summary.h"

#include <algorithm>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "large_byte_buffer.h"
#include "large_object.h"
#include "little_endian.h"
#include "memory_manager.h"

namespace aircannect {
namespace {

constexpr size_t METRIC_HISTOGRAM_BINS = ReportMetricAccumulator::HistogramBins;

enum class MetricHistogramId : uint8_t {
    Pressure,
    Leak,
    MinuteVentilation,
    RespiratoryRate,
    TidalVolume,
    Spo2,
    Ipap,
    Epap,
    FlowLimitation,
    Snore,
    InspirationTime,
    ExpirationTime,
    IeRatio,
    TargetVentilation,
    Pulse,
    Count,
};

constexpr size_t METRIC_HISTOGRAM_COUNT =
    static_cast<size_t>(MetricHistogramId::Count);
constexpr uint16_t METRIC_CHECKPOINT_VERSION = 4;
constexpr size_t METRIC_CHECKPOINT_HEADER_BYTES = 32;
constexpr size_t METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET = 6;
constexpr size_t METRIC_CHECKPOINT_SUM_OFFSET = 0;
constexpr size_t METRIC_CHECKPOINT_SAMPLES_OFFSET = 8;
constexpr size_t METRIC_CHECKPOINT_BINS_OFFSET = 28;
constexpr size_t METRIC_CHECKPOINT_HISTOGRAM_BYTES =
    METRIC_CHECKPOINT_BINS_OFFSET + METRIC_HISTOGRAM_BINS * sizeof(uint32_t);
constexpr uint16_t METRIC_CHECKPOINT_ACTIVE_MASK =
    static_cast<uint16_t>((1u << METRIC_HISTOGRAM_COUNT) - 1u);

static_assert(METRIC_CHECKPOINT_HEADER_BYTES +
                  METRIC_HISTOGRAM_COUNT * METRIC_CHECKPOINT_HISTOGRAM_BYTES <=
              UINT32_MAX,
              "metric checkpoint length must fit its wire field");

struct MetricHistogramConfig {
    ReportSignalId signal;
    int32_t minimum_milli;
    int32_t maximum_milli;
    bool quadratic_bins = false;
};

constexpr MetricHistogramConfig METRIC_HISTOGRAM_CONFIGS[] = {
    {ReportSignalId::MaskPressure, 0, 40000},
    {ReportSignalId::Leak, 0, 120000},
    {ReportSignalId::MinuteVentilation, 0, 30000},
    {ReportSignalId::RespiratoryRate, 0, 90000},
    {ReportSignalId::TidalVolume, 0, 4000},
    {ReportSignalId::SpO2, 0, 100000},
    {ReportSignalId::InspiratoryPressure, 0, 40000},
    {ReportSignalId::ExpiratoryPressure, 0, 40000},
    {ReportSignalId::FlowLimitation, 0, 1000},
    {ReportSignalId::Snore, 0, 5000},
    {ReportSignalId::InspiratoryDuration, 0, 10000},
    // Fine resolution for ordinary breaths, while retaining long Te values.
    {ReportSignalId::Invalid, 0, 1000000, true},
    {ReportSignalId::IeRatio, 0, 400000},
    {ReportSignalId::TargetMinuteVentilation, 0, 30000},
    {ReportSignalId::Pulse, 0, 300000},
};
static_assert(sizeof(METRIC_HISTOGRAM_CONFIGS) /
                  sizeof(METRIC_HISTOGRAM_CONFIGS[0]) ==
              METRIC_HISTOGRAM_COUNT,
              "metric histogram config must cover every metric");

struct MetricHistogram {
    std::unique_ptr<LargeByteBuffer> bins;
    size_t first_bin = 0;
    size_t bin_count = 0;
    int64_t sum_milli = 0;
    uint64_t samples = 0;
    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    bool extrema_valid = true;
    bool active = false;

    uint32_t *data() { return reinterpret_cast<uint32_t *>(bins->data()); }
    const uint32_t *data() const {
        return bins ? reinterpret_cast<const uint32_t *>(bins->data()) : nullptr;
    }

    uint32_t count(size_t bin) const {
        return bin >= first_bin && bin - first_bin < bin_count
            ? data()[bin - first_bin] : 0;
    }

    bool reserve(size_t first, size_t last) {
        if (bin_count && first >= first_bin && last < first_bin + bin_count) {
            return true;
        }

        constexpr size_t Quantum = 64;
        const size_t begin = std::min(first, bin_count ? first_bin : first) /
            Quantum * Quantum;
        const size_t end = (std::max(last + 1, first_bin + bin_count) +
            Quantum - 1) / Quantum * Quantum;
        auto next = LargeByteBuffer::allocate((end - begin) * sizeof(uint32_t));
        if (!next) return false;

        memset(next->data(), 0, next->size());
        if (bins) {
            memcpy(next->data() + (first_bin - begin) * sizeof(uint32_t),
                   bins->data(), bins->size());
        }
        bins = std::move(next);
        first_bin = begin;
        bin_count = end - begin;
        return true;
    }
};

int64_t saturating_add_int64(int64_t left, int64_t right) {
    if (right > 0 && left > INT64_MAX - right) return INT64_MAX;
    if (right < 0 && left < INT64_MIN - right) return INT64_MIN;
    return left + right;
}

uint64_t saturating_add_uint64(uint64_t left, uint64_t right) {
    return left > UINT64_MAX - right ? UINT64_MAX : left + right;
}

uint32_t saturating_add_uint32(uint32_t left, uint32_t right) {
    return left > UINT32_MAX - right ? UINT32_MAX : left + right;
}

void add_histogram(MetricHistogram &target,
                   const MetricHistogram &source) {
    target.active |= source.active;
    target.minimum = std::min(target.minimum, source.minimum);
    target.maximum = std::max(target.maximum, source.maximum);
    target.extrema_valid &= source.extrema_valid;
    for (size_t i = 0; i < source.bin_count; ++i) {
        auto &count = target.data()[source.first_bin + i - target.first_bin];
        count = saturating_add_uint32(count, source.data()[i]);
    }
    target.sum_milli = saturating_add_int64(
        target.sum_milli, source.sum_milli);
    target.samples = saturating_add_uint64(
        target.samples, source.samples);
}

void bind_histograms(MetricHistogram histograms[METRIC_HISTOGRAM_COUNT],
                     const bool active[METRIC_HISTOGRAM_COUNT]) {
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        histograms[i] = {};
        if (!active[i]) continue;

        histograms[i].active = true;
    }
}

uint16_t histogram_active_mask(
    const bool active[METRIC_HISTOGRAM_COUNT]) {
    uint16_t mask = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (active[i]) mask |= static_cast<uint16_t>(1u << i);
    }
    return mask;
}

size_t checkpoint_size(uint16_t active_mask, uint16_t version) {
    size_t active_count = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if ((active_mask & (1u << i)) != 0) ++active_count;
    }
    return (version == 1 ? 7 : version == 2 ? 8 : METRIC_CHECKPOINT_HEADER_BYTES) +
        active_count * (version == 1 ? 16 + METRIC_HISTOGRAM_BINS * 4
                              : METRIC_CHECKPOINT_HISTOGRAM_BYTES);
}

uint32_t metric_bit(NightCatalogMetric metric) {
    return 1u << static_cast<uint8_t>(metric);
}

int metric_histogram_index(ReportSignalId signal) {
    if (signal == ReportSignalId::Invalid) return -1;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (METRIC_HISTOGRAM_CONFIGS[i].signal == signal) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int32_t to_milli(float value) {
    const double scaled = static_cast<double>(value) * 1000.0;
    if (scaled >= static_cast<double>(INT32_MAX)) return INT32_MAX;
    if (scaled <= static_cast<double>(INT32_MIN)) return INT32_MIN;
    return static_cast<int32_t>(llround(scaled));
}

int32_t index_milli(uint32_t count, uint32_t duration_min) {
    if (duration_min == 0) return 0;

    const uint64_t numerator =
        static_cast<uint64_t>(count) * 60ULL * 1000ULL;
    const uint64_t value =
        (numerator + duration_min / 2) / duration_min;
    return value > static_cast<uint64_t>(INT32_MAX)
        ? INT32_MAX
        : static_cast<int32_t>(value);
}

bool add_metric_sample(MetricHistogram &histogram,
                       const MetricHistogramConfig &config,
                       int32_t value_milli) {
    if (!histogram.active) return true;

    const int32_t bounded = std::max(
        config.minimum_milli,
        std::min(config.maximum_milli, value_milli));
    const uint64_t offset = static_cast<uint64_t>(
        bounded - config.minimum_milli);
    const uint64_t span = static_cast<uint64_t>(
        config.maximum_milli - config.minimum_milli);
    const size_t bin = config.quadratic_bins
        ? static_cast<size_t>(llround(sqrt(static_cast<double>(offset) / span) *
                                     (METRIC_HISTOGRAM_BINS - 1)))
        : static_cast<size_t>(
            (offset * (METRIC_HISTOGRAM_BINS - 1) + span / 2) / span);
    if (!histogram.reserve(bin, bin)) return false;
    auto &count = histogram.data()[bin - histogram.first_bin];
    if (count != UINT32_MAX) ++count;

    histogram.minimum = std::min(histogram.minimum, value_milli);
    histogram.maximum = std::max(histogram.maximum, value_milli);

    histogram.sum_milli = saturating_add_int64(
        histogram.sum_milli, static_cast<int64_t>(value_milli));
    histogram.samples = saturating_add_uint64(histogram.samples, 1);
    return true;
}

bool metric_mean(const MetricHistogram &histogram, int32_t &out) {
    if (!histogram.active || !histogram.bins || histogram.samples == 0 ||
        histogram.samples > static_cast<uint64_t>(INT64_MAX)) {
        return false;
    }

    const int64_t divisor = static_cast<int64_t>(histogram.samples);
    int64_t mean = histogram.sum_milli / divisor;
    const int64_t remainder = histogram.sum_milli % divisor;
    const int64_t threshold = divisor / 2 + divisor % 2;
    if (remainder >= threshold && mean < INT64_MAX) ++mean;
    if (remainder <= -threshold && mean > INT64_MIN) --mean;

    out = mean > INT32_MAX ? INT32_MAX
        : mean < INT32_MIN ? INT32_MIN
                           : static_cast<int32_t>(mean);
    return true;
}

bool metric_value_at_rank(const MetricHistogram &histogram,
                          const MetricHistogramConfig &config,
                          uint64_t rank,
                          int32_t &out) {
    if (rank >= histogram.samples) return false;

    uint64_t cumulative = 0;
    for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
        cumulative += histogram.count(bin);
        if (cumulative <= rank) continue;

        out = ReportMetricAccumulator::bin_value(&config - METRIC_HISTOGRAM_CONFIGS, bin);
        return true;
    }
    return false;
}

bool metric_percentile(const MetricHistogram &histogram,
                       const MetricHistogramConfig &config,
                       uint32_t numerator,
                       uint32_t denominator,
                       int32_t &out) {
    if (!histogram.active || !histogram.bins || histogram.samples == 0 ||
        denominator == 0 || numerator > denominator) {
        return false;
    }

    const uint64_t scaled_rank =
        (histogram.samples - 1) * numerator;
    const uint64_t lower_rank = scaled_rank / denominator;
    const uint32_t remainder = static_cast<uint32_t>(
        scaled_rank % denominator);
    int32_t lower = 0;
    if (!metric_value_at_rank(histogram, config, lower_rank, lower)) {
        return false;
    }
    if (remainder == 0) {
        out = lower;
        return true;
    }

    int32_t upper = 0;
    if (!metric_value_at_rank(histogram, config, lower_rank + 1, upper)) {
        return false;
    }
    out = static_cast<int32_t>(
        (static_cast<int64_t>(lower) * (denominator - remainder) +
         static_cast<int64_t>(upper) * remainder + denominator / 2) /
        denominator);
    return true;
}

void finish_metric(const MetricHistogram &histogram,
                   const MetricHistogramConfig &config,
                   ReportMetricStatistics &out) {
    out.valid = metric_mean(histogram, out.mean_milli) &&
        metric_percentile(histogram, config, 50, 100, out.p50_milli) &&
        metric_percentile(histogram, config, 95, 100, out.p95_milli);
    out.extrema_valid = out.valid && histogram.extrema_valid;
    if (out.extrema_valid) {
        out.min_milli = histogram.minimum;
        out.max_milli = histogram.maximum;
    }
}

}  // namespace

struct ReportMetricAccumulator::Runtime {
    MetricHistogram histograms[METRIC_HISTOGRAM_COUNT];
    uint16_t incomplete_mask = 0;
    bool leak_duration_incomplete = false;
    uint64_t leak_recorded_ms = 0;
    uint64_t leak_above_threshold_ms = 0;

    void clear() {
        incomplete_mask = 0;
        leak_duration_incomplete = false;
        leak_recorded_ms = 0;
        leak_above_threshold_ms = 0;
        for (MetricHistogram &histogram : histograms) histogram = {};
    }
};

ReportMetricAccumulator::ReportMetricAccumulator() :
    runtime_(LargeObject::create<Runtime>()) {}

ReportMetricAccumulator::~ReportMetricAccumulator() {
    if (runtime_) runtime_->clear();
    LargeObject::destroy(runtime_);
}

bool ReportMetricAccumulator::begin(uint32_t signal_mask) {
    if (!runtime_) return false;
    runtime_->clear();

    bool active[METRIC_HISTOGRAM_COUNT] = {};
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        active[i] = (signal_mask & report_signal_bit(
            METRIC_HISTOGRAM_CONFIGS[i].signal)) != 0;
    }
    // The paired Ti/I:E samples contribute to Ti's checkpoint and tail.
    active[static_cast<size_t>(MetricHistogramId::ExpirationTime)] =
        (signal_mask & report_signal_bit(ReportSignalId::InspiratoryDuration)) != 0;

    bind_histograms(runtime_->histograms, active);
    return true;
}

bool ReportMetricAccumulator::begin(const ReportReadPlan &plan,
                                    const uint8_t *checkpoint,
                                    size_t checkpoint_size) {
    if (!runtime_) return false;

    uint32_t signal_mask = 0;
    for (size_t i = 0; i < plan.mapping_count(); ++i) {
        const ReportReadMapping *mapping = plan.mapping(i);
        if (!mapping) {
            runtime_->clear();
            return false;
        }

        signal_mask |= report_signal_bit(mapping->series.signal);
    }
    if (!checkpoint) return begin(signal_mask);

    runtime_->clear();
    return restore(checkpoint, checkpoint_size, signal_mask);
}

bool ReportMetricAccumulator::accept(ReportSignalId signal,
                                     int32_t value_milli,
                                     uint32_t duration_ms) {
    if (!runtime_) return false;
    const int index = metric_histogram_index(signal);
    if (index < 0 || !runtime_->histograms[index].active) return true;
    if (!add_metric_sample(runtime_->histograms[index],
                      METRIC_HISTOGRAM_CONFIGS[index],
                      value_milli)) return false;

    if (signal == ReportSignalId::Leak) {
        runtime_->leak_recorded_ms = saturating_add_uint64(
            runtime_->leak_recorded_ms, duration_ms);
        if (value_milli > ReportCalculatedMetrics::LeakThresholdMilli) {
            runtime_->leak_above_threshold_ms = saturating_add_uint64(
                runtime_->leak_above_threshold_ms, duration_ms);
        }
    }
    return true;
}

bool ReportMetricAccumulator::accept_expiration_time(int32_t value_milli) {
    if (!runtime_) return false;
    constexpr size_t index = static_cast<size_t>(MetricHistogramId::ExpirationTime);
    return add_metric_sample(runtime_->histograms[index], METRIC_HISTOGRAM_CONFIGS[index],
                      value_milli);
}

ReportCalculatedMetrics ReportMetricAccumulator::finish() const {
    ReportCalculatedMetrics out;
    if (!runtime_) return out;

    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (runtime_->incomplete_mask & (1u << i)) continue;
        finish_metric(runtime_->histograms[i],
                      METRIC_HISTOGRAM_CONFIGS[i],
                      out.at(i));
    }
    if (!runtime_->leak_duration_incomplete) {
        out.leak_recorded_ms = runtime_->leak_recorded_ms;
        out.leak_above_threshold_ms = runtime_->leak_above_threshold_ms;
    }
    return out;
}

ReportMetricStatistics &ReportCalculatedMetrics::at(size_t index) {
    ReportMetricStatistics *values[] = {&pressure, &leak, &minute_ventilation,
        &respiratory_rate, &tidal_volume, &spo2, &ipap, &epap,
        &flow_limitation, &snore, &inspiration_time, &expiration_time,
        &ie_ratio, &target_ventilation, &pulse};
    return *values[index];
}

const ReportMetricStatistics &ReportCalculatedMetrics::at(size_t index) const {
    return const_cast<ReportCalculatedMetrics *>(this)->at(index);
}

bool report_signal_has_metric_consumer(ReportSignalId signal) {
    return metric_histogram_index(signal) >= 0;
}

std::shared_ptr<const LargeByteBuffer> ReportMetricAccumulator::snapshot()
    const {
    if (!runtime_) return {};

    bool active[METRIC_HISTOGRAM_COUNT] = {};
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        active[i] = runtime_->histograms[i].active;
    }
    const uint16_t active_mask = histogram_active_mask(active);
    uint16_t counts[METRIC_HISTOGRAM_COUNT] = {};
    bool sparse[METRIC_HISTOGRAM_COUNT] = {};
    size_t size = METRIC_CHECKPOINT_HEADER_BYTES;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (!active[i]) continue;

        for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
            counts[i] += runtime_->histograms[i].count(bin) != 0;
        }
        sparse[i] = counts[i] * 6u < METRIC_HISTOGRAM_BINS * 4u;
        if (!sparse[i]) counts[i] = METRIC_HISTOGRAM_BINS;
        size += METRIC_CHECKPOINT_BINS_OFFSET + counts[i] * (sparse[i] ? 6u : 4u);
    }

    std::unique_ptr<LargeByteBuffer> output = LargeByteBuffer::allocate(size);
    if (!output) return {};

    memset(output->data(), 0, output->size());
    LittleEndian::put_le16(output->data(), METRIC_CHECKPOINT_VERSION);
    LittleEndian::put_le32(
        output->data() + 2, static_cast<uint32_t>(output->size()));
    LittleEndian::put_le16(output->data() + METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET, active_mask);
    LittleEndian::put_le16(output->data() + 8, runtime_->incomplete_mask);
    output->data()[10] = runtime_->leak_duration_incomplete ? 1 : 0;
    LittleEndian::put_le64(output->data() + 12, runtime_->leak_recorded_ms);
    LittleEndian::put_le64(output->data() + 20, runtime_->leak_above_threshold_ms);

    uint8_t *record = output->data() + METRIC_CHECKPOINT_HEADER_BYTES;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const MetricHistogram &histogram = runtime_->histograms[i];
        if (!histogram.active) continue;

        LittleEndian::put_le64(
            record + METRIC_CHECKPOINT_SUM_OFFSET,
            static_cast<uint64_t>(histogram.sum_milli));
        LittleEndian::put_le64(
            record + METRIC_CHECKPOINT_SAMPLES_OFFSET, histogram.samples);
        LittleEndian::put_le32(record + 16, histogram.minimum);
        LittleEndian::put_le32(record + 20, histogram.maximum);
        record[24] = histogram.extrema_valid ? 1 : 0;
        record[25] = sparse[i] ? 1 : 0;
        LittleEndian::put_le16(record + 26, counts[i]);
        uint8_t *bins = record + METRIC_CHECKPOINT_BINS_OFFSET;
        for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
            if (sparse[i]) {
                if (!histogram.count(bin)) continue;
                LittleEndian::put_le16(bins, bin);
                bins += 2;
            }
            LittleEndian::put_le32(bins, histogram.count(bin));
            bins += 4;
        }
        record = bins;
    }

    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportMetricAccumulator::restore(const uint8_t *data, size_t length,
                                      uint32_t additional_signals) {
    if (!runtime_ || !data || length < 7 ||
        (LittleEndian::get_le16(data) != 1 &&
         LittleEndian::get_le16(data) != 2 &&
         LittleEndian::get_le16(data) != 3 &&
         LittleEndian::get_le16(data) != METRIC_CHECKPOINT_VERSION) ||
        LittleEndian::get_le32(data + 2) != length) {
        return false;
    }

    const uint16_t version = LittleEndian::get_le16(data);
    if (length < (version == 1 ? 7 : version == 2 ? 8 : METRIC_CHECKPOINT_HEADER_BYTES)) {
        return false;
    }
    const uint16_t active_mask = version < 3 ? data[METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET]
        : LittleEndian::get_le16(data + METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET);
    const bool legacy = version == 1;
    if ((active_mask & ~METRIC_CHECKPOINT_ACTIVE_MASK) != 0 ||
        (version < 4 && checkpoint_size(active_mask, version) != length) ||
        (version >= 3 &&
         ((LittleEndian::get_le16(data + 8) & ~METRIC_CHECKPOINT_ACTIVE_MASK) != 0 ||
          data[10] > 1 ||
          LittleEndian::get_le64(data + 20) > LittleEndian::get_le64(data + 12)))) {
        return false;
    }

    const uint8_t *records[METRIC_HISTOGRAM_COUNT] = {};
    uint16_t counts[METRIC_HISTOGRAM_COUNT] = {};
    bool sparse[METRIC_HISTOGRAM_COUNT] = {};
    uint16_t first_bin[METRIC_HISTOGRAM_COUNT];
    uint16_t last_bin[METRIC_HISTOGRAM_COUNT] = {};
    std::fill_n(first_bin, METRIC_HISTOGRAM_COUNT, METRIC_HISTOGRAM_BINS);
    const size_t bins_offset = legacy ? 16 : METRIC_CHECKPOINT_BINS_OFFSET;
    size_t offset = legacy ? 7 : version == 2 ? 8 : METRIC_CHECKPOINT_HEADER_BYTES;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (!(active_mask & (1u << i))) continue;
        if (length - offset < bins_offset) return false;

        const uint8_t *record = data + offset;
        records[i] = record;
        counts[i] = version < 4 ? METRIC_HISTOGRAM_BINS
                               : LittleEndian::get_le16(record + 26);
        sparse[i] = version >= 4 && record[25] == 1;
        if (version >= 4 &&
            (record[24] > 1 || record[25] > 1 ||
             counts[i] > METRIC_HISTOGRAM_BINS ||
             (!sparse[i] && counts[i] != METRIC_HISTOGRAM_BINS))) {
            return false;
        }

        offset += bins_offset;
        const size_t bytes = counts[i] * (sparse[i] ? 6u : 4u);
        if (bytes > length - offset) return false;
        uint16_t previous = 0;
        for (size_t bin = 0; bin < counts[i]; ++bin) {
            const uint8_t *entry = data + offset + bin * (sparse[i] ? 6 : 4);
            const uint16_t index = sparse[i] ? LittleEndian::get_le16(entry) : bin;
            const uint32_t value = LittleEndian::get_le32(entry + (sparse[i] ? 2 : 0));
            if (sparse[i] && (index >= METRIC_HISTOGRAM_BINS ||
                (bin && index <= previous) || value == 0)) {
                return false;
            }
            previous = index;
            if (!value) continue;
            first_bin[i] = std::min(first_bin[i], index);
            last_bin[i] = index;
        }
        offset += bytes;
    }
    if (offset != length) return false;

    bool active[METRIC_HISTOGRAM_COUNT] = {};
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        active[i] = (active_mask & (1u << i)) != 0 ||
            (additional_signals & report_signal_bit(metric_signal(i))) != 0;
    }
    active[static_cast<size_t>(MetricHistogramId::ExpirationTime)] |=
        (additional_signals & report_signal_bit(ReportSignalId::InspiratoryDuration)) != 0;

    MetricHistogram next[METRIC_HISTOGRAM_COUNT];
    bind_histograms(next, active);
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (!(active_mask & (1u << i))) continue;

        if (first_bin[i] < METRIC_HISTOGRAM_BINS &&
            !next[i].reserve(first_bin[i], last_bin[i])) return false;

        const uint8_t *record = records[i];
        next[i].sum_milli = static_cast<int64_t>(LittleEndian::get_le64(
            record + METRIC_CHECKPOINT_SUM_OFFSET));
        next[i].samples = LittleEndian::get_le64(
            record + METRIC_CHECKPOINT_SAMPLES_OFFSET);
        next[i].extrema_valid = legacy ? next[i].samples == 0 : record[24] != 0;
        if (!legacy) {
            next[i].minimum = static_cast<int32_t>(LittleEndian::get_le32(record + 16));
            next[i].maximum = static_cast<int32_t>(LittleEndian::get_le32(record + 20));
        }
        const uint8_t *bins = record + bins_offset;
        for (size_t bin = 0; bin < counts[i]; ++bin) {
            const uint16_t index = sparse[i] ? LittleEndian::get_le16(bins) : bin;
            if (sparse[i]) bins += 2;
            const uint32_t count = LittleEndian::get_le32(bins);
            if (count) {
                next[i].data()[index - next[i].first_bin] = count;
            }
            bins += 4;
        }
    }

    runtime_->clear();
    runtime_->incomplete_mask = version >= 3 ? LittleEndian::get_le16(data + 8)
        : static_cast<uint16_t>(METRIC_CHECKPOINT_ACTIVE_MASK & ~0xffu);
    if (legacy || (version == 2 && data[7] != 0)) runtime_->incomplete_mask |= 1u << 7;
    runtime_->leak_duration_incomplete = version < 3 || data[10] != 0;
    if (version >= 3) {
        runtime_->leak_recorded_ms = LittleEndian::get_le64(data + 12);
        runtime_->leak_above_threshold_ms = LittleEndian::get_le64(data + 20);
    }
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        runtime_->histograms[i] = std::move(next[i]);
    }
    return true;
}

bool ReportMetricAccumulator::merge(const ReportMetricAccumulator &other) {
    if (!runtime_ || !other.runtime_) return false;

    // Reserve the union before changing any counts or completeness flags.
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const auto &source = other.runtime_->histograms[i];
        if (source.bin_count && !runtime_->histograms[i].reserve(
                source.first_bin, source.first_bin + source.bin_count - 1)) {
            return false;
        }
    }

    runtime_->incomplete_mask |= other.runtime_->incomplete_mask;
    runtime_->leak_duration_incomplete |= other.runtime_->leak_duration_incomplete;
    runtime_->leak_recorded_ms = saturating_add_uint64(
        runtime_->leak_recorded_ms, other.runtime_->leak_recorded_ms);
    runtime_->leak_above_threshold_ms = saturating_add_uint64(
        runtime_->leak_above_threshold_ms, other.runtime_->leak_above_threshold_ms);

    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const MetricHistogram &previous = other.runtime_->histograms[i];
        if (previous.active) add_histogram(runtime_->histograms[i], previous);
    }
    return true;
}

void ReportMetricAccumulator::clear() {
    if (runtime_) runtime_->clear();
}

bool ReportMetricAccumulator::histogram(size_t index, Histogram &out) const {
    out = {};
    if (!runtime_ || index >= METRIC_HISTOGRAM_COUNT ||
        (runtime_->incomplete_mask & (1u << index))) return false;

    const auto &h = runtime_->histograms[index];
    if (!h.active || !h.bins || !h.samples) return false;
    out = {h.data(), h.bin_count, h.first_bin, h.sum_milli, h.samples,
           h.minimum, h.maximum, h.extrema_valid};
    return true;
}

ReportSignalId ReportMetricAccumulator::metric_signal(size_t index) {
    return index < METRIC_HISTOGRAM_COUNT
        ? METRIC_HISTOGRAM_CONFIGS[index].signal : ReportSignalId::Invalid;
}

int32_t ReportMetricAccumulator::bin_value(size_t index, size_t bin) {
    const auto &config = METRIC_HISTOGRAM_CONFIGS[index];
    const int64_t span = config.maximum_milli - config.minimum_milli;
    const int64_t numerator = config.quadratic_bins
        ? static_cast<int64_t>(bin) * bin : bin;
    const int64_t divisor = config.quadratic_bins
        ? (METRIC_HISTOGRAM_BINS - 1) * (METRIC_HISTOGRAM_BINS - 1)
        : METRIC_HISTOGRAM_BINS - 1;
    return config.minimum_milli + (numerator * span + divisor / 2) / divisor;
}

void report_night_count_event(ReportEventCounts &counts,
                              const ReportEventRecord &event) {
    switch (static_cast<ReportEventCode>(event.code)) {
        case ReportEventCode::Hypopnea:
            ++counts.hypopnea;
            break;
        case ReportEventCode::CentralApnea:
            ++counts.central_apnea;
            break;
        case ReportEventCode::ObstructiveApnea:
            ++counts.obstructive_apnea;
            break;
        case ReportEventCode::UnclassifiedApnea:
            ++counts.unknown_apnea;
            break;
        case ReportEventCode::Arousal:
            ++counts.arousal;
            break;
        case ReportEventCode::Csr:
            ++counts.csr;
            break;
    }
}

void report_night_metrics_from_catalog(const NightCatalogMetrics &source,
                                       ReportNightMetrics &target) {
    target.valid_mask = source.valid_mask;
    target.str_mask = source.str_mask;
    target.summary_mask = source.summary_mask;
    target.ahi_milli = to_milli(source.ahi);
    target.obstructive_apnea_index_milli =
        to_milli(source.obstructive_apnea_index);
    target.central_apnea_index_milli =
        to_milli(source.central_apnea_index);
    target.unknown_apnea_index_milli =
        to_milli(source.unknown_apnea_index);
    target.hypopnea_index_milli = to_milli(source.hypopnea_index);
    target.arousal_index_milli = to_milli(source.arousal_index);
    target.mask_pressure_50_milli =
        to_milli(source.mask_pressure_50_cm_h2o);
    target.leak_50_milli = to_milli(source.leak_50_l_min);
    target.duration_minutes = source.duration_min;
    target.mask_pressure_95_milli =
        to_milli(source.mask_pressure_95_cm_h2o);
    target.leak_95_milli = to_milli(source.leak_95_l_min);
    target.minute_ventilation_50_milli =
        to_milli(source.minute_ventilation_50_l_min);
    target.minute_ventilation_95_milli =
        to_milli(source.minute_ventilation_95_l_min);
    target.respiratory_rate_50_milli =
        to_milli(source.respiratory_rate_50_bpm);
    target.respiratory_rate_95_milli =
        to_milli(source.respiratory_rate_95_bpm);
    target.tidal_volume_50_milli =
        to_milli(source.tidal_volume_50_l);
    target.tidal_volume_95_milli =
        to_milli(source.tidal_volume_95_l);
    target.spo2_median_milli = to_milli(source.spo2_median_percent);
    target.spo2_threshold_minutes = source.spo2_threshold_minutes;
    target.csr_minutes = source.csr_minutes;
}

void report_night_complete_metrics(ReportNightMetrics &metrics,
                                   const ReportEventCounts &events,
                                   const ReportCalculatedMetrics &calculated,
                                   uint8_t requested_event_mask,
                                   uint8_t missing_event_mask,
                                   uint64_t csr_duration_ms) {
    const bool scored_events_complete =
        (requested_event_mask & REPORT_EVENT_SCORED) != 0 &&
        (missing_event_mask & REPORT_EVENT_SCORED) == 0;
    const bool csr_events_complete =
        (requested_event_mask & REPORT_EVENT_CSR) != 0 &&
        (missing_event_mask & REPORT_EVENT_CSR) == 0;
    const uint32_t apnea_count = events.hypopnea + events.central_apnea +
        events.obstructive_apnea + events.unknown_apnea;

    const struct {
        NightCatalogMetric metric;
        uint32_t count;
        int32_t *value;
    } indexes[] = {
        {NightCatalogMetric::Ahi, apnea_count, &metrics.ahi_milli},
        {NightCatalogMetric::ObstructiveApneaIndex,
         events.obstructive_apnea,
         &metrics.obstructive_apnea_index_milli},
        {NightCatalogMetric::CentralApneaIndex,
         events.central_apnea,
         &metrics.central_apnea_index_milli},
        {NightCatalogMetric::UnknownApneaIndex,
         events.unknown_apnea,
         &metrics.unknown_apnea_index_milli},
        {NightCatalogMetric::HypopneaIndex,
         events.hypopnea,
         &metrics.hypopnea_index_milli},
        {NightCatalogMetric::ArousalIndex,
         events.arousal,
         &metrics.arousal_index_milli},
    };
    if (scored_events_complete) {
        for (const auto &index : indexes) {
            const uint32_t bit = metric_bit(index.metric);
            if ((metrics.valid_mask & bit) != 0) continue;
            *index.value = index_milli(index.count, metrics.duration_minutes);
            metrics.valid_mask |= bit;
        }
    }

    const struct {
        NightCatalogMetric p50_metric;
        NightCatalogMetric p95_metric;
        const ReportMetricStatistics *source;
        int32_t *p50;
        int32_t *p95;
    } percentiles[] = {
        {NightCatalogMetric::MaskPressure50,
         NightCatalogMetric::MaskPressure95,
         &calculated.pressure,
         &metrics.mask_pressure_50_milli,
         &metrics.mask_pressure_95_milli},
        {NightCatalogMetric::Leak50,
         NightCatalogMetric::Leak95,
         &calculated.leak,
         &metrics.leak_50_milli,
         &metrics.leak_95_milli},
        {NightCatalogMetric::MinuteVentilation50,
         NightCatalogMetric::MinuteVentilation95,
         &calculated.minute_ventilation,
         &metrics.minute_ventilation_50_milli,
         &metrics.minute_ventilation_95_milli},
        {NightCatalogMetric::RespiratoryRate50,
         NightCatalogMetric::RespiratoryRate95,
         &calculated.respiratory_rate,
         &metrics.respiratory_rate_50_milli,
         &metrics.respiratory_rate_95_milli},
        {NightCatalogMetric::TidalVolume50,
         NightCatalogMetric::TidalVolume95,
         &calculated.tidal_volume,
         &metrics.tidal_volume_50_milli,
         &metrics.tidal_volume_95_milli},
    };
    for (const auto &percentile : percentiles) {
        if (!percentile.source->valid) continue;

        const uint32_t p50_bit = metric_bit(percentile.p50_metric);
        if ((metrics.valid_mask & p50_bit) == 0) {
            *percentile.p50 = percentile.source->p50_milli;
            metrics.valid_mask |= p50_bit;
        }

        const uint32_t p95_bit = metric_bit(percentile.p95_metric);
        if ((metrics.valid_mask & p95_bit) == 0) {
            *percentile.p95 = percentile.source->p95_milli;
            metrics.valid_mask |= p95_bit;
        }
    }

    if (calculated.ipap.valid) {
        metrics.ipap_mean_milli = calculated.ipap.mean_milli;
        metrics.ipap_50_milli = calculated.ipap.p50_milli;
        metrics.ipap_95_milli = calculated.ipap.p95_milli;
        metrics.valid_mask |= REPORT_NIGHT_METRIC_IPAP_MEAN |
            REPORT_NIGHT_METRIC_IPAP_50 | REPORT_NIGHT_METRIC_IPAP_95;
    }

    if (calculated.leak.valid) {
        metrics.leak_mean_milli = calculated.leak.mean_milli;
        metrics.valid_mask |= REPORT_NIGHT_METRIC_LEAK_MEAN;
    }

    const uint32_t spo2_bit = metric_bit(NightCatalogMetric::Spo2Median);
    if ((metrics.valid_mask & spo2_bit) == 0 && calculated.spo2.valid) {
        metrics.spo2_median_milli = calculated.spo2.p50_milli;
        metrics.valid_mask |= spo2_bit;
    }

    const uint32_t csr_bit = metric_bit(NightCatalogMetric::CsrMinutes);
    if ((metrics.valid_mask & csr_bit) == 0 && csr_events_complete) {
        const uint64_t minutes = (csr_duration_ms + 30000ULL) / 60000ULL;
        metrics.csr_minutes = minutes > UINT32_MAX
            ? UINT32_MAX
            : static_cast<uint32_t>(minutes);
        metrics.valid_mask |= csr_bit;
    }
}

}  // namespace aircannect
