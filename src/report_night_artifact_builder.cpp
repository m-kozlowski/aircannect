#include "report_night_artifact_builder.h"

#include <algorithm>
#include <limits.h>
#include <limits>
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "board_report.h"
#include "crc32.h"
#include "report_plot_encoder.h"
#include "report_records.h"
#include "report_sources.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr size_t SIGNAL_COUNT =
    static_cast<size_t>(ReportSignalId::Count);
constexpr size_t EVENT_BUFFER_MAX_BYTES = 256 * 1024;

struct EnvelopeCell {
    int32_t minimum = 0;
    int32_t maximum = 0;
    bool present = false;
};

struct SeriesState {
    EnvelopeCell *cells = nullptr;
    size_t cell_count = 0;
    int64_t bucket_ms = 0;
    uint32_t max_interval_ms = 0;
    bool active = false;
};

struct EventSlot {
    ReportEventRecord event;
};

void *allocate_large(size_t count, size_t width) {
    if (count == 0 || width == 0 ||
        count > std::numeric_limits<size_t>::max() / width) {
        return nullptr;
    }
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

uint16_t metric_bit(NightCatalogMetric metric) {
    return static_cast<uint16_t>(
        1u << static_cast<uint8_t>(metric));
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

int32_t scaled_plot_value(const EdfReportSignalLayout &layout,
                          int32_t value_milli) {
    const int32_t multiplier =
        (layout.signal == ReportSignalId::Flow &&
         layout.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (layout.signal == ReportSignalId::Leak &&
         layout.source == ReportSourceId::Leak0p5Hz)
            ? 60
            : 1;
    const int64_t scaled =
        static_cast<int64_t>(value_milli) * multiplier;
    if (scaled > INT32_MAX) return INT32_MAX;
    if (scaled < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(scaled);
}

bool event_less(const EventSlot &lhs, const EventSlot &rhs) {
    if (lhs.event.start_ms != rhs.event.start_ms) {
        return lhs.event.start_ms < rhs.event.start_ms;
    }
    if (lhs.event.duration_ms != rhs.event.duration_ms) {
        return lhs.event.duration_ms < rhs.event.duration_ms;
    }
    if (lhs.event.code != rhs.event.code) {
        return lhs.event.code < rhs.event.code;
    }
    return lhs.event.flags < rhs.event.flags;
}

bool same_event(const EventSlot &lhs, const EventSlot &rhs) {
    return lhs.event.start_ms == rhs.event.start_ms &&
           lhs.event.duration_ms == rhs.event.duration_ms &&
           lhs.event.code == rhs.event.code &&
           lhs.event.flags == rhs.event.flags;
}

std::shared_ptr<const LargeByteBuffer> freeze_copy(
    const ReportSpoolBuffer &source) {
    if (source.size() == 0) return {};
    std::unique_ptr<LargeByteBuffer> output =
        LargeByteBuffer::allocate(source.size());
    if (!output) return {};
    memcpy(output->data(), source.data(), source.size());
    return LargeByteBuffer::freeze(std::move(output));
}

}  // namespace

struct ReportNightArtifactBuilder::Runtime {
    ReportArtifactRequest request;
    const ReportReadPlan *plan = nullptr;
    SeriesState series[SIGNAL_COUNT];
    EnvelopeCell *cell_storage = nullptr;
    size_t cell_storage_count = 0;
    NightCatalogTimeRange *session_ranges = nullptr;
    size_t session_count = 0;
    ReportSpoolBuffer event_slots;
    int64_t plot_start_ms = 0;
    int64_t plot_end_ms = 0;
    int64_t pressure_sum_milli = 0;
    int64_t leak_sum_milli = 0;
    uint64_t pressure_samples = 0;
    uint64_t leak_samples = 0;
    bool active = false;
    std::shared_ptr<const ReportArtifactBundle> completed;

    void clear_work() {
        free_large(cell_storage);
        cell_storage = nullptr;
        cell_storage_count = 0;
        free_large(session_ranges);
        session_ranges = nullptr;
        session_count = 0;
        event_slots.clear();
        request = {};
        plan = nullptr;
        for (SeriesState &state : series) state = {};
        plot_start_ms = 0;
        plot_end_ms = 0;
        pressure_sum_milli = 0;
        leak_sum_milli = 0;
        pressure_samples = 0;
        leak_samples = 0;
        active = false;
    }
};

ReportNightArtifactBuilder::ReportNightArtifactBuilder() :
    runtime_(new (std::nothrow) Runtime()) {}

ReportNightArtifactBuilder::~ReportNightArtifactBuilder() {
    if (runtime_) runtime_->clear_work();
    delete runtime_;
}

bool ReportNightArtifactBuilder::begin_build(
    const ReportArtifactRequest &request,
    const ReportReadPlan &plan) {
    if (!runtime_) return false;

    runtime_->clear_work();
    runtime_->completed.reset();
    if (!request.ticket.valid() ||
        request.artifact.kind != ReportArtifactKind::Result ||
        request.artifact != plan.key()) {
        return false;
    }

    runtime_->request = request;
    runtime_->plan = &plan;

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
        runtime_->plot_end_ms =
            runtime_->session_ranges[runtime_->session_count - 1].end_ms;
    } else {
        runtime_->plot_start_ms = plan.night().day_start_ms;
        runtime_->plot_end_ms = plan.night().day_end_ms;
    }
    if (runtime_->plot_end_ms <= runtime_->plot_start_ms) {
        runtime_->clear_work();
        return false;
    }

    for (size_t i = 0; i < plan.mapping_count(); ++i) {
        const ReportReadMapping *mapping = plan.mapping(i);
        if (!mapping) {
            runtime_->clear_work();
            return false;
        }
        const size_t signal = static_cast<size_t>(mapping->layout.signal);
        if (signal >= SIGNAL_COUNT) {
            runtime_->clear_work();
            return false;
        }
        SeriesState &state = runtime_->series[signal];
        state.active = true;
        state.max_interval_ms = std::max(
            state.max_interval_ms, mapping->layout.sample_interval_ms);
    }

    const uint64_t span = static_cast<uint64_t>(
        runtime_->plot_end_ms - runtime_->plot_start_ms);
    const int64_t base_bucket_ms = std::max<int64_t>(
        1,
        static_cast<int64_t>((span + AC_REPORT_PLOT_BUCKETS - 1) /
                             AC_REPORT_PLOT_BUCKETS));
    size_t total_cells = 0;
    for (SeriesState &state : runtime_->series) {
        if (!state.active) continue;
        state.bucket_ms = std::max<int64_t>(base_bucket_ms,
                                            state.max_interval_ms);
        state.cell_count = static_cast<size_t>(
            (span + static_cast<uint64_t>(state.bucket_ms) - 1) /
            static_cast<uint64_t>(state.bucket_ms));
        if (state.cell_count == 0 ||
            state.cell_count > AC_REPORT_PLOT_BUCKETS ||
            total_cells > std::numeric_limits<size_t>::max() -
                              state.cell_count) {
            runtime_->clear_work();
            return false;
        }
        total_cells += state.cell_count;
    }

    if (total_cells > 0) {
        runtime_->cell_storage = static_cast<EnvelopeCell *>(
            allocate_large(total_cells, sizeof(EnvelopeCell)));
        if (!runtime_->cell_storage) {
            runtime_->clear_work();
            return false;
        }
        runtime_->cell_storage_count = total_cells;

        size_t offset = 0;
        for (SeriesState &state : runtime_->series) {
            if (!state.active) continue;
            state.cells = runtime_->cell_storage + offset;
            offset += state.cell_count;
        }
    }

    runtime_->event_slots.set_max_size(EVENT_BUFFER_MAX_BYTES);
    runtime_->active = true;
    return true;
}

bool ReportNightArtifactBuilder::accept_series(
    uint16_t session_index,
    const EdfReportSignalLayout &layout,
    const ReportSeriesSample &sample) {
    if (!runtime_ || !runtime_->active || !runtime_->plan ||
        session_index >= runtime_->plan->session_count()) {
        return false;
    }

    const size_t signal = static_cast<size_t>(layout.signal);
    if (signal >= SIGNAL_COUNT) return false;
    SeriesState &state = runtime_->series[signal];
    if (!state.active || !state.cells || state.bucket_ms <= 0 ||
        sample.timestamp_ms < runtime_->plot_start_ms ||
        sample.timestamp_ms >= runtime_->plot_end_ms) {
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    if (!session || sample.timestamp_ms < session->output_window.start_ms ||
        sample.timestamp_ms >= session->output_window.end_ms) {
        return false;
    }

    const uint64_t delta = static_cast<uint64_t>(
        sample.timestamp_ms - runtime_->plot_start_ms);
    const size_t bucket = static_cast<size_t>(
        delta / static_cast<uint64_t>(state.bucket_ms));
    if (bucket >= state.cell_count) return false;

    const int32_t value = scaled_plot_value(layout, sample.value_milli);
    EnvelopeCell &cell = state.cells[bucket];
    if (!cell.present) {
        cell.minimum = value;
        cell.maximum = value;
        cell.present = true;
    } else {
        cell.minimum = std::min(cell.minimum, value);
        cell.maximum = std::max(cell.maximum, value);
    }

    if (layout.signal == ReportSignalId::MaskPressure) {
        runtime_->pressure_sum_milli += value;
        ++runtime_->pressure_samples;
    } else if (layout.signal == ReportSignalId::Leak) {
        runtime_->leak_sum_milli += value;
        ++runtime_->leak_samples;
    }
    return true;
}

bool ReportNightArtifactBuilder::accept_event(
    uint16_t session_index,
    const ReportEventRecord &event) {
    if (!runtime_ || !runtime_->active || !runtime_->plan ||
        session_index >= runtime_->plan->session_count()) {
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    if (!session || !report_event_overlaps_window(
                        event,
                        session->output_window.start_ms,
                        session->output_window.end_ms)) {
        return false;
    }

    EventSlot slot;
    slot.event = event;
    return runtime_->event_slots.append(
        reinterpret_cast<const uint8_t *>(&slot), sizeof(slot));
}

namespace {

void add_event_count(ReportArtifactEventCounts &counts,
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
    target.hypopnea_index_milli =
        to_milli(source.hypopnea_index);
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
                      int64_t pressure_sum,
                      uint64_t pressure_samples,
                      int64_t leak_sum,
                      uint64_t leak_samples) {
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
        *fallback.value = index_milli(fallback.count,
                                      result.duration_min);
        metrics.valid_mask |= bit;
    }

    const uint16_t pressure_bit =
        metric_bit(NightCatalogMetric::MaskPressure50);
    if ((metrics.valid_mask & pressure_bit) == 0 && pressure_samples > 0) {
        metrics.mask_pressure_50_milli =
            divide_round(pressure_sum, pressure_samples);
        metrics.valid_mask |= pressure_bit;
    }

    const uint16_t leak_bit = metric_bit(NightCatalogMetric::Leak50);
    if ((metrics.valid_mask & leak_bit) == 0 && leak_samples > 0) {
        metrics.leak_50_milli = divide_round(leak_sum, leak_samples);
        metrics.valid_mask |= leak_bit;
    }
}

bool append_events(ReportSpoolBuffer &plot,
                   EventSlot *events,
                   size_t event_count,
                   int64_t base_ms,
                   ReportArtifactEventCounts &counts,
                   uint32_t &unique_count) {
    if (event_count == 0) {
        unique_count = 0;
        return true;
    }
    if (!events) return false;

    std::sort(events, events + event_count, event_less);
    unique_count = 0;
    for (size_t i = 0; i < event_count; ++i) {
        if (i > 0 && same_event(events[i - 1], events[i])) continue;

        const int64_t delta = events[i].event.start_ms - base_ms;
        if (delta < INT32_MIN || delta > INT32_MAX) return false;
        if (!bin_put_i32(plot, static_cast<int32_t>(delta)) ||
            !bin_put_i32(plot, events[i].event.duration_ms) ||
            !bin_put_i32(plot, events[i].event.code) ||
            !bin_put_i32(plot, events[i].event.flags)) {
            return false;
        }
        add_event_count(counts, events[i].event);
        ++unique_count;
    }
    return true;
}

bool append_series(ReportSpoolBuffer &plot,
                   const SeriesState *series) {
    size_t definition_count = 0;
    const ReportSignalDef *definitions =
        report_signal_defs(definition_count);
    if (!definitions) return false;

    for (size_t signal_index = 0; signal_index < SIGNAL_COUNT;
         ++signal_index) {
        const SeriesState &state = series[signal_index];
        if (!state.active || !state.cells) continue;

        const char *name = nullptr;
        for (size_t i = 0; i < definition_count; ++i) {
            if (static_cast<size_t>(definitions[i].id) == signal_index) {
                name = definitions[i].store_name;
                break;
            }
        }
        if (!name) return false;

        ReportSpoolBuffer raw;
        const size_t raw_limit = state.cell_count * 12;
        raw.set_max_size(raw_limit ? raw_limit : 12);
        if (raw_limit && !raw.reserve_capacity(raw_limit)) return false;

        for (size_t bucket = 0; bucket < state.cell_count; ++bucket) {
            const EnvelopeCell &cell = state.cells[bucket];
            if (!cell.present) continue;
            if (!bin_put_u32(raw, static_cast<uint32_t>(bucket)) ||
                !bin_put_i32(raw, cell.minimum) ||
                !bin_put_i32(raw, cell.maximum)) {
                return false;
            }
        }

        bool ok = true;
        if (!append_plot_series_envelope_runs(
                plot, name, raw, state.bucket_ms, ok) || !ok) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool ReportNightArtifactBuilder::finish_build() {
    if (!runtime_ || !runtime_->active || !runtime_->plan) return false;
    if ((runtime_->event_slots.size() % sizeof(EventSlot)) != 0) return false;

    const size_t event_count =
        runtime_->event_slots.size() / sizeof(EventSlot);
    EventSlot *events = reinterpret_cast<EventSlot *>(
        runtime_->event_slots.mutable_data());

    ReportSpoolBuffer plot;
    plot.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    if (!plot.reserve_capacity(AC_REPORT_PLOT_INITIAL_RESERVE) ||
        !bin_put_u32(plot, PLOT_BIN_MAGIC) ||
        !bin_put_u16(plot, PLOT_BIN_VERSION) ||
        !bin_put_u16(plot, 0) ||
        !bin_put_i64(plot, runtime_->plot_start_ms)) {
        return false;
    }

    size_t event_count_offset = 0;
    uint8_t *event_count_bytes =
        plot.append_uninitialized(sizeof(uint32_t), event_count_offset);
    if (!event_count_bytes) return false;
    memset(event_count_bytes, 0, sizeof(uint32_t));

    ReportArtifactEventCounts event_counts;
    uint32_t unique_events = 0;
    if (!append_events(plot,
                       events,
                       event_count,
                       runtime_->plot_start_ms,
                       event_counts,
                       unique_events) ||
        !append_series(plot, runtime_->series)) {
        return false;
    }
    uint8_t *plot_bytes = plot.mutable_data();
    plot_bytes[event_count_offset] = static_cast<uint8_t>(unique_events);
    plot_bytes[event_count_offset + 1] =
        static_cast<uint8_t>(unique_events >> 8);
    plot_bytes[event_count_offset + 2] =
        static_cast<uint8_t>(unique_events >> 16);
    plot_bytes[event_count_offset + 3] =
        static_cast<uint8_t>(unique_events >> 24);

    std::shared_ptr<const LargeByteBuffer> overview = freeze_copy(plot);
    if (!overview) return false;

    ReportResultArtifactData result;
    result.key = runtime_->request.artifact;
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
    result.requested_event_mask = runtime_->plan->requested_event_mask();
    result.missing_event_mask = runtime_->plan->missing_event_mask();
    result.source_flags = runtime_->plan->night().source_flags;
    result.events = event_counts;
    result.sessions = runtime_->session_ranges;
    result.session_count = runtime_->session_count;

    const NightCatalogMetrics &catalog_metrics =
        runtime_->plan->night().metrics;
    copy_catalog_metrics(catalog_metrics, result.metrics);
    result.duration_min = catalog_metrics.has(
        NightCatalogMetric::DurationMinutes)
        ? catalog_metrics.duration_min
        : session_duration_minutes(*runtime_->plan);
    complete_metrics(result,
                     runtime_->pressure_sum_milli,
                     runtime_->pressure_samples,
                     runtime_->leak_sum_milli,
                     runtime_->leak_samples);

    if (result.missing_required_signal_mask == 0) {
        result.flags |= REPORT_RESULT_COMPLETE;
    }
    if ((result.requested_event_mask & result.missing_event_mask) == 0) {
        result.flags |= REPORT_RESULT_EVENTS_AVAILABLE;
    }

    std::shared_ptr<const LargeByteBuffer> result_bytes =
        ReportResultArtifactCodec::encode(result);
    if (!result_bytes) return false;

    std::shared_ptr<ReportArtifactBundle> bundle =
        std::make_shared<ReportArtifactBundle>();
    bundle->key = result.key;
    bundle->result = std::move(result_bytes);
    bundle->overview = std::move(overview);
    bundle->result_crc32 = crc32_ieee(
        bundle->result->data(), bundle->result->size());
    bundle->overview_crc32 = crc32_ieee(
        bundle->overview->data(), bundle->overview->size());
    bundle->manifest = ReportArtifactManifestCodec::encode(*bundle);
    if (!bundle->manifest) return false;

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
    std::shared_ptr<const ReportArtifactBundle> output =
        std::move(runtime_->completed);
    return output;
}

}  // namespace aircannect
