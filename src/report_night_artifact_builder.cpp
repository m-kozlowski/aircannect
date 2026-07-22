#include "report_night_artifact_builder.h"

#include <limits.h>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <utility>

#include "board_report.h"
#include "crc32.h"
#include "report_plot_accumulator.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

void *allocate_large(size_t count, size_t width) {
    if (count == 0 || width == 0 || count > SIZE_MAX / width) return nullptr;
#ifdef ARDUINO
    return Memory::calloc_large(count, width, false);
#else
    return calloc(count, width);
#endif
}

void free_large(void *memory) {
#ifdef ARDUINO
    Memory::free(memory);
#else
    free(memory);
#endif
}

template <typename T>
T *create_large() {
#ifdef ARDUINO
    void *memory = Memory::alloc_large(sizeof(T), false);
    return memory ? new (memory) T() : nullptr;
#else
    return new (std::nothrow) T();
#endif
}

template <typename T>
void destroy_large(T *value) {
    if (!value) return;
#ifdef ARDUINO
    value->~T();
    Memory::free(value);
#else
    delete value;
#endif
}

uint16_t metric_bit(NightCatalogMetric metric) {
    return static_cast<uint16_t>(1u << static_cast<uint8_t>(metric));
}

int32_t to_milli(float value) {
    const double scaled = static_cast<double>(value) * 1000.0;
    if (scaled >= static_cast<double>(INT32_MAX)) return INT32_MAX;
    if (scaled <= static_cast<double>(INT32_MIN)) return INT32_MIN;
    return static_cast<int32_t>(llround(scaled));
}

int32_t divide_round(int64_t total, uint64_t count) {
    if (count == 0) return 0;

    const int64_t half = static_cast<int64_t>(count / 2);
    const int64_t value = total >= 0
        ? (total + half) / static_cast<int64_t>(count)
        : -((-total + half) / static_cast<int64_t>(count));
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(value);
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

void copy_catalog_metrics(const NightCatalogMetrics &source,
                          ReportArtifactMetrics &target) {
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
}

uint32_t session_duration_minutes(const ReportReadPlan &plan) {
    uint64_t duration_ms = 0;
    for (size_t i = 0; i < plan.session_count(); ++i) {
        const ReportReadSession *session = plan.session(i);
        if (!session || !session->output_window.valid()) continue;

        const uint64_t span = static_cast<uint64_t>(
            session->output_window.end_ms -
            session->output_window.start_ms);
        if (duration_ms > UINT64_MAX - span) return UINT32_MAX;
        duration_ms += span;
    }

    const uint64_t minutes = (duration_ms + 30000ULL) / 60000ULL;
    return minutes > UINT32_MAX ? UINT32_MAX
                                : static_cast<uint32_t>(minutes);
}

void complete_metrics(ReportResultArtifactData &result,
                      const ReportPlotAccumulatorSummary &plot) {
    ReportArtifactMetrics &metrics = result.metrics;
    const ReportArtifactEventCounts &events = result.events;
    const uint32_t apnea_count =
        events.hypopnea + events.central_apnea +
        events.obstructive_apnea + events.unknown_apnea;

    const struct {
        NightCatalogMetric metric;
        uint32_t count;
        int32_t *value;
    } index_fallbacks[] = {
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

    for (const auto &fallback : index_fallbacks) {
        const uint16_t bit = metric_bit(fallback.metric);
        if ((metrics.valid_mask & bit) != 0) continue;

        *fallback.value = index_milli(fallback.count, result.duration_min);
        metrics.valid_mask |= bit;
    }

    const uint16_t pressure_bit =
        metric_bit(NightCatalogMetric::MaskPressure50);
    if ((metrics.valid_mask & pressure_bit) == 0 &&
        plot.pressure_samples > 0) {
        metrics.mask_pressure_50_milli = divide_round(
            plot.pressure_sum_milli, plot.pressure_samples);
        metrics.valid_mask |= pressure_bit;
    }

    const uint16_t leak_bit = metric_bit(NightCatalogMetric::Leak50);
    if ((metrics.valid_mask & leak_bit) == 0 && plot.leak_samples > 0) {
        metrics.leak_50_milli =
            divide_round(plot.leak_sum_milli, plot.leak_samples);
        metrics.valid_mask |= leak_bit;
    }
}

bool valid_tile_key(const ReportArtifactKey &key) {
    return key.kind == ReportArtifactKind::RangeTile && key.valid();
}

}  // namespace

struct ReportNightArtifactBuilder::Runtime {
    ReportArtifactRequest request;
    const ReportReadPlan *plan = nullptr;
    NightCatalogTimeRange *session_ranges = nullptr;
    size_t session_count = 0;
    int64_t plot_start_ms = 0;
    int64_t plot_end_ms = 0;
    ReportPlotAccumulator plot;
    bool active = false;
    std::shared_ptr<const ReportArtifactBundle> completed;

    void clear_work() {
        plot.clear();
        free_large(session_ranges);
        session_ranges = nullptr;
        session_count = 0;
        request = {};
        plan = nullptr;
        plot_start_ms = 0;
        plot_end_ms = 0;
        active = false;
    }
};

ReportNightArtifactBuilder::ReportNightArtifactBuilder() :
    runtime_(create_large<Runtime>()) {}

ReportNightArtifactBuilder::~ReportNightArtifactBuilder() {
    if (runtime_) runtime_->clear_work();
    destroy_large(runtime_);
}

bool ReportNightArtifactBuilder::begin_build(
    const ReportArtifactRequest &request,
    const ReportReadPlan &plan) {
    if (!runtime_) return false;

    runtime_->clear_work();
    runtime_->completed.reset();
    if (!request.ticket.valid() || request.artifact != plan.key() ||
        (request.artifact.kind != ReportArtifactKind::Result &&
         !valid_tile_key(request.artifact))) {
        return false;
    }

    runtime_->request = request;
    runtime_->plan = &plan;

    if (request.artifact.kind == ReportArtifactKind::Result) {
        if (plan.session_count() > 0) {
            runtime_->session_ranges = static_cast<NightCatalogTimeRange *>(
                allocate_large(plan.session_count(),
                               sizeof(NightCatalogTimeRange)));
            if (!runtime_->session_ranges) {
                runtime_->clear_work();
                return false;
            }
            runtime_->session_count = plan.session_count();

            for (size_t i = 0; i < plan.session_count(); ++i) {
                const ReportReadSession *session = plan.session(i);
                if (!session || !session->output_window.valid()) {
                    runtime_->clear_work();
                    return false;
                }
                runtime_->session_ranges[i] = session->output_window;
            }

            runtime_->plot_start_ms = runtime_->session_ranges[0].start_ms;
            runtime_->plot_end_ms = runtime_->session_ranges[
                runtime_->session_count - 1].end_ms;
        } else {
            runtime_->plot_start_ms = plan.night().day_start_ms;
            runtime_->plot_end_ms = plan.night().day_end_ms;
        }
    } else {
        runtime_->plot_start_ms = request.artifact.range_start_ms;
        runtime_->plot_end_ms = request.artifact.range_end_ms;
    }

    const size_t bucket_budget =
        request.artifact.kind == ReportArtifactKind::Result
            ? AC_REPORT_PLOT_BUCKETS
            : AC_REPORT_RANGE_PLOT_BUCKETS;
    if (!runtime_->plot.begin(plan,
                              runtime_->plot_start_ms,
                              runtime_->plot_end_ms,
                              bucket_budget)) {
        runtime_->clear_work();
        return false;
    }

    runtime_->active = true;
    return true;
}

bool ReportNightArtifactBuilder::accept_series(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    const ReportSeriesSample &sample) {
    return runtime_ && runtime_->active &&
           runtime_->plot.accept_series(session_index, series, sample);
}

bool ReportNightArtifactBuilder::accept_event(
    uint16_t session_index,
    const ReportEventRecord &event) {
    return runtime_ && runtime_->active &&
           runtime_->plot.accept_event(session_index, event);
}

bool ReportNightArtifactBuilder::finish_build() {
    if (!runtime_ || !runtime_->active || !runtime_->plan) return false;

    ReportPlotAccumulatorSummary plot_summary;
    std::shared_ptr<const LargeByteBuffer> plot =
        runtime_->plot.finish(plot_summary);
    if (!plot) return false;

    std::shared_ptr<ReportArtifactBundle> bundle =
        std::make_shared<ReportArtifactBundle>();
    bundle->key = runtime_->request.artifact;

    if (bundle->key.kind == ReportArtifactKind::RangeTile) {
        bundle->range_tile = std::move(plot);
        bundle->range_tile_crc32 = crc32_ieee(
            bundle->range_tile->data(), bundle->range_tile->size());
    } else {
        ReportResultArtifactData result;
        result.key = bundle->key;
        result.day_start_ms = runtime_->plan->night().day_start_ms;
        result.day_end_ms = runtime_->plan->night().day_end_ms;
        result.therapy_start_ms = runtime_->plan->session_count() > 0
            ? runtime_->plot_start_ms
            : 0;
        result.therapy_end_ms = runtime_->plan->session_count() > 0
            ? runtime_->plot_end_ms
            : 0;
        result.requested_signal_mask =
            runtime_->plan->requested_signal_mask();
        result.missing_required_signal_mask =
            runtime_->plan->missing_required_signal_mask();
        result.missing_optional_signal_mask =
            runtime_->plan->missing_optional_signal_mask();
        result.available_signal_mask = result.requested_signal_mask &
            ~(result.missing_required_signal_mask |
              result.missing_optional_signal_mask);
        result.requested_event_mask =
            runtime_->plan->requested_event_mask();
        result.missing_event_mask = runtime_->plan->missing_event_mask();
        result.source_flags = runtime_->plan->night().source_flags;
        result.events = plot_summary.events;
        result.sessions = runtime_->session_ranges;
        result.session_count = runtime_->session_count;

        const NightCatalogMetrics &catalog_metrics =
            runtime_->plan->night().metrics;
        copy_catalog_metrics(catalog_metrics, result.metrics);
        result.duration_min = catalog_metrics.has(
            NightCatalogMetric::DurationMinutes)
            ? catalog_metrics.duration_min
            : session_duration_minutes(*runtime_->plan);
        complete_metrics(result, plot_summary);

        if (result.missing_required_signal_mask == 0) {
            result.flags |= REPORT_RESULT_COMPLETE;
        }
        if ((result.requested_event_mask & result.missing_event_mask) == 0) {
            result.flags |= REPORT_RESULT_EVENTS_AVAILABLE;
        }

        bundle->result = ReportResultArtifactCodec::encode(result);
        bundle->overview = std::move(plot);
        if (!bundle->result || !bundle->overview) return false;

        bundle->result_crc32 = crc32_ieee(
            bundle->result->data(), bundle->result->size());
        bundle->overview_crc32 = crc32_ieee(
            bundle->overview->data(), bundle->overview->size());
        bundle->manifest = ReportArtifactManifestCodec::encode(*bundle);
        if (!bundle->manifest) return false;
    }

    if (!bundle->valid()) return false;
    runtime_->completed = std::move(bundle);
    runtime_->clear_work();
    return true;
}

void ReportNightArtifactBuilder::discard_build() {
    if (!runtime_) return;
    runtime_->clear_work();
    runtime_->completed.reset();
}

std::shared_ptr<const ReportArtifactBundle>
ReportNightArtifactBuilder::completed() const {
    return runtime_ ? runtime_->completed : nullptr;
}

std::shared_ptr<const ReportArtifactBundle>
ReportNightArtifactBuilder::take_completed() {
    if (!runtime_) return {};
    return std::move(runtime_->completed);
}

}  // namespace aircannect
