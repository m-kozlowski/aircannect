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

constexpr size_t METRIC_HISTOGRAM_BINS = 2048;

enum class MetricHistogramId : uint8_t {
    Pressure,
    Leak,
    MinuteVentilation,
    RespiratoryRate,
    TidalVolume,
    Spo2,
    Ipap,
    Count,
};

constexpr size_t METRIC_HISTOGRAM_COUNT =
    static_cast<size_t>(MetricHistogramId::Count);
constexpr uint16_t METRIC_CHECKPOINT_VERSION = 1;
constexpr size_t METRIC_CHECKPOINT_HEADER_BYTES = 7;
constexpr size_t METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET = 6;
constexpr size_t METRIC_CHECKPOINT_SUM_OFFSET = 0;
constexpr size_t METRIC_CHECKPOINT_SAMPLES_OFFSET = 8;
constexpr size_t METRIC_CHECKPOINT_BINS_OFFSET = 16;
constexpr size_t METRIC_CHECKPOINT_HISTOGRAM_BYTES =
    METRIC_CHECKPOINT_BINS_OFFSET + METRIC_HISTOGRAM_BINS * sizeof(uint32_t);
constexpr uint8_t METRIC_CHECKPOINT_ACTIVE_MASK =
    static_cast<uint8_t>((1u << METRIC_HISTOGRAM_COUNT) - 1u);

static_assert(METRIC_CHECKPOINT_HEADER_BYTES +
                  METRIC_HISTOGRAM_COUNT * METRIC_CHECKPOINT_HISTOGRAM_BYTES <=
              UINT32_MAX,
              "metric checkpoint length must fit its wire field");

struct MetricHistogramConfig {
    ReportSignalId signal;
    int32_t minimum_milli;
    int32_t maximum_milli;
};

constexpr MetricHistogramConfig METRIC_HISTOGRAM_CONFIGS[] = {
    {ReportSignalId::MaskPressure, 0, 40000},
    {ReportSignalId::Leak, 0, 120000},
    {ReportSignalId::MinuteVentilation, 0, 30000},
    {ReportSignalId::RespiratoryRate, 0, 90000},
    {ReportSignalId::TidalVolume, 0, 4000},
    {ReportSignalId::SpO2, 0, 100000},
    {ReportSignalId::InspiratoryPressure, 0, 40000},
};
static_assert(sizeof(METRIC_HISTOGRAM_CONFIGS) /
                  sizeof(METRIC_HISTOGRAM_CONFIGS[0]) ==
              METRIC_HISTOGRAM_COUNT,
              "metric histogram config must cover every metric");

struct MetricHistogram {
    uint32_t *bins = nullptr;
    int64_t sum_milli = 0;
    uint64_t samples = 0;
    bool active = false;
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

void copy_histogram(MetricHistogram &target,
                    const MetricHistogram &source) {
    target.sum_milli = source.sum_milli;
    target.samples = source.samples;
    memcpy(target.bins,
           source.bins,
           METRIC_HISTOGRAM_BINS * sizeof(uint32_t));
}

void add_histogram(MetricHistogram &target,
                   const MetricHistogram &source) {
    for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
        target.bins[bin] = saturating_add_uint32(
            target.bins[bin], source.bins[bin]);
    }
    target.sum_milli = saturating_add_int64(
        target.sum_milli, source.sum_milli);
    target.samples = saturating_add_uint64(
        target.samples, source.samples);
}

bool allocate_histogram_bins(const bool active[METRIC_HISTOGRAM_COUNT],
                             uint32_t *&bins) {
    size_t active_count = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (active[i]) ++active_count;
    }

    if (active_count == 0) {
        bins = nullptr;
        return true;
    }

    bins = static_cast<uint32_t *>(Memory::calloc_large(
        active_count * METRIC_HISTOGRAM_BINS, sizeof(uint32_t), false));
    return bins != nullptr;
}

void bind_histograms(MetricHistogram histograms[METRIC_HISTOGRAM_COUNT],
                     const bool active[METRIC_HISTOGRAM_COUNT],
                     uint32_t *bins) {
    size_t offset = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        histograms[i] = {};
        if (!active[i]) continue;

        histograms[i].active = true;
        histograms[i].bins = bins + offset;
        offset += METRIC_HISTOGRAM_BINS;
    }
}

uint8_t histogram_active_mask(
    const bool active[METRIC_HISTOGRAM_COUNT]) {
    uint8_t mask = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (active[i]) mask |= static_cast<uint8_t>(1u << i);
    }
    return mask;
}

size_t checkpoint_size(uint8_t active_mask) {
    size_t active_count = 0;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if ((active_mask & (1u << i)) != 0) ++active_count;
    }
    return METRIC_CHECKPOINT_HEADER_BYTES +
        active_count * METRIC_CHECKPOINT_HISTOGRAM_BYTES;
}

uint32_t metric_bit(NightCatalogMetric metric) {
    return 1u << static_cast<uint8_t>(metric);
}

int metric_histogram_index(ReportSignalId signal) {
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

void add_metric_sample(MetricHistogram &histogram,
                       const MetricHistogramConfig &config,
                       int32_t value_milli) {
    if (!histogram.active || !histogram.bins) return;

    const int32_t bounded = std::max(
        config.minimum_milli,
        std::min(config.maximum_milli, value_milli));
    const uint64_t offset = static_cast<uint64_t>(
        bounded - config.minimum_milli);
    const uint64_t span = static_cast<uint64_t>(
        config.maximum_milli - config.minimum_milli);
    const size_t bin = static_cast<size_t>(
        (offset * (METRIC_HISTOGRAM_BINS - 1) + span / 2) / span);
    if (histogram.bins[bin] != UINT32_MAX) ++histogram.bins[bin];

    histogram.sum_milli = saturating_add_int64(
        histogram.sum_milli, static_cast<int64_t>(value_milli));
    histogram.samples = saturating_add_uint64(histogram.samples, 1);
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
        cumulative += histogram.bins[bin];
        if (cumulative <= rank) continue;

        const int64_t span = static_cast<int64_t>(
            config.maximum_milli - config.minimum_milli);
        out = config.minimum_milli + static_cast<int32_t>(
            (static_cast<int64_t>(bin) * span +
             (METRIC_HISTOGRAM_BINS - 1) / 2) /
            (METRIC_HISTOGRAM_BINS - 1));
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
}

}  // namespace

struct ReportMetricAccumulator::Runtime {
    MetricHistogram histograms[METRIC_HISTOGRAM_COUNT];
    uint32_t *bins = nullptr;

    void clear() {
        Memory::free(bins);
        bins = nullptr;
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

    if (!allocate_histogram_bins(active, runtime_->bins)) {
        runtime_->clear();
        return false;
    }

    bind_histograms(runtime_->histograms, active, runtime_->bins);
    return true;
}

bool ReportMetricAccumulator::begin(const ReportReadPlan &plan) {
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
    return begin(signal_mask);
}

void ReportMetricAccumulator::accept(ReportSignalId signal,
                                     int32_t value_milli) {
    if (!runtime_) return;
    const int index = metric_histogram_index(signal);
    if (index < 0) return;
    add_metric_sample(runtime_->histograms[index],
                      METRIC_HISTOGRAM_CONFIGS[index],
                      value_milli);
}

ReportCalculatedMetrics ReportMetricAccumulator::finish() const {
    ReportCalculatedMetrics out;
    if (!runtime_) return out;

    ReportMetricStatistics *outputs[] = {
        &out.pressure,
        &out.leak,
        &out.minute_ventilation,
        &out.respiratory_rate,
        &out.tidal_volume,
        &out.spo2,
        &out.ipap,
    };
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        finish_metric(runtime_->histograms[i],
                      METRIC_HISTOGRAM_CONFIGS[i],
                      *outputs[i]);
    }
    return out;
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
        if (active[i] && !runtime_->histograms[i].bins) return {};
    }
    const uint8_t active_mask = histogram_active_mask(active);

    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(checkpoint_size(active_mask));
    if (!output) return {};

    memset(output->data(), 0, output->size());
    LittleEndian::put_le16(output->data(), METRIC_CHECKPOINT_VERSION);
    LittleEndian::put_le32(
        output->data() + 2, static_cast<uint32_t>(output->size()));
    output->data()[METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET] = active_mask;

    uint8_t *record = output->data() + METRIC_CHECKPOINT_HEADER_BYTES;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const MetricHistogram &histogram = runtime_->histograms[i];
        if (!histogram.active) continue;

        LittleEndian::put_le64(
            record + METRIC_CHECKPOINT_SUM_OFFSET,
            static_cast<uint64_t>(histogram.sum_milli));
        LittleEndian::put_le64(
            record + METRIC_CHECKPOINT_SAMPLES_OFFSET, histogram.samples);
        for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
            LittleEndian::put_le32(
                record + METRIC_CHECKPOINT_BINS_OFFSET + bin * sizeof(uint32_t),
                histogram.bins[bin]);
        }
        record += METRIC_CHECKPOINT_HISTOGRAM_BYTES;
    }

    return LargeByteBuffer::freeze(std::move(output));
}

bool ReportMetricAccumulator::restore(const uint8_t *data, size_t length) {
    if (!runtime_ || !data || length < METRIC_CHECKPOINT_HEADER_BYTES ||
        LittleEndian::get_le16(data) != METRIC_CHECKPOINT_VERSION ||
        LittleEndian::get_le32(data + 2) != length) {
        return false;
    }

    const uint8_t active_mask = data[METRIC_CHECKPOINT_ACTIVE_MASK_OFFSET];
    if ((active_mask & ~METRIC_CHECKPOINT_ACTIVE_MASK) != 0 ||
        checkpoint_size(active_mask) != length) {
        return false;
    }

    bool active[METRIC_HISTOGRAM_COUNT] = {};
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        active[i] = (active_mask & (1u << i)) != 0;
    }

    uint32_t *next_bins = nullptr;
    if (!allocate_histogram_bins(active, next_bins)) return false;

    MetricHistogram next[METRIC_HISTOGRAM_COUNT];
    bind_histograms(next, active, next_bins);
    const uint8_t *record = data + METRIC_CHECKPOINT_HEADER_BYTES;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        if (!active[i]) continue;

        next[i].sum_milli = static_cast<int64_t>(LittleEndian::get_le64(
            record + METRIC_CHECKPOINT_SUM_OFFSET));
        next[i].samples = LittleEndian::get_le64(
            record + METRIC_CHECKPOINT_SAMPLES_OFFSET);
        for (size_t bin = 0; bin < METRIC_HISTOGRAM_BINS; ++bin) {
            next[i].bins[bin] = LittleEndian::get_le32(
                record + METRIC_CHECKPOINT_BINS_OFFSET +
                bin * sizeof(uint32_t));
        }
        record += METRIC_CHECKPOINT_HISTOGRAM_BYTES;
    }

    runtime_->clear();
    runtime_->bins = next_bins;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        runtime_->histograms[i] = next[i];
    }
    return true;
}

bool ReportMetricAccumulator::merge(const ReportMetricAccumulator &other) {
    if (!runtime_ || !other.runtime_) return false;

    bool added_active = false;
    bool merged_active[METRIC_HISTOGRAM_COUNT] = {};
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const MetricHistogram &current = runtime_->histograms[i];
        const MetricHistogram &previous = other.runtime_->histograms[i];
        if (current.active && !current.bins) return false;
        if (previous.active && !previous.bins) return false;

        merged_active[i] = current.active || previous.active;
        added_active |= previous.active && !current.active;
    }

    if (!added_active) {
        for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
            if (!other.runtime_->histograms[i].active) continue;

            MetricHistogram &current = runtime_->histograms[i];
            const MetricHistogram &previous = other.runtime_->histograms[i];
            add_histogram(current, previous);
        }
        return true;
    }

    uint32_t *next_bins = nullptr;
    if (!allocate_histogram_bins(merged_active, next_bins)) return false;

    MetricHistogram next[METRIC_HISTOGRAM_COUNT];
    bind_histograms(next, merged_active, next_bins);
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        const MetricHistogram &current = runtime_->histograms[i];
        const MetricHistogram &previous = other.runtime_->histograms[i];
        if (!merged_active[i]) continue;

        if (!current.active) {
            copy_histogram(next[i], previous);
            continue;
        }
        if (!previous.active) {
            copy_histogram(next[i], current);
            continue;
        }

        copy_histogram(next[i], current);
        add_histogram(next[i], previous);
    }

    runtime_->clear();
    runtime_->bins = next_bins;
    for (size_t i = 0; i < METRIC_HISTOGRAM_COUNT; ++i) {
        runtime_->histograms[i] = next[i];
    }
    return true;
}

void ReportMetricAccumulator::clear() {
    if (runtime_) runtime_->clear();
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
