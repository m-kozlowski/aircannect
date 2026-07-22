#include "report_plot_accumulator.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "board_report.h"
#include "report_plot_encoder.h"
#include "report_records.h"
#include "report_sources.h"

#ifdef ARDUINO
#include "memory_manager.h"
#endif

namespace aircannect {
namespace {

constexpr size_t SIGNAL_COUNT = static_cast<size_t>(ReportSignalId::Count);
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

template <typename T, typename... Args>
T *create_large(Args &&...args) {
#ifdef ARDUINO
    void *memory = Memory::alloc_large(sizeof(T), false);
    return memory ? new (memory) T(std::forward<Args>(args)...) : nullptr;
#else
    return new (std::nothrow) T(std::forward<Args>(args)...);
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

int32_t scaled_plot_value(const ReportSeriesDescriptor &series,
                          int32_t value_milli) {
    const int32_t multiplier =
        (series.signal == ReportSignalId::Flow &&
         series.source == ReportSourceId::RespiratoryFlow6p25Hz) ||
        (series.signal == ReportSignalId::Leak &&
         series.source == ReportSourceId::Leak0p5Hz)
            ? 60
            : 1;
    const int64_t scaled = static_cast<int64_t>(value_milli) * multiplier;
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
        if (delta < INT32_MIN || delta > INT32_MAX ||
            !bin_put_i32(plot, static_cast<int32_t>(delta)) ||
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

bool append_series(ReportSpoolBuffer &plot, const SeriesState *series) {
    size_t definition_count = 0;
    const ReportSignalDef *definitions = report_signal_defs(definition_count);
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

struct ReportPlotAccumulator::Runtime {
    const ReportReadPlan *plan = nullptr;
    SeriesState series[SIGNAL_COUNT];
    EnvelopeCell *cell_storage = nullptr;
    ReportSpoolBuffer event_slots;
    int64_t plot_start_ms = 0;
    int64_t plot_end_ms = 0;
    int64_t pressure_sum_milli = 0;
    int64_t leak_sum_milli = 0;
    uint64_t pressure_samples = 0;
    uint64_t leak_samples = 0;
    bool active = false;

    void clear() {
        free_large(cell_storage);
        cell_storage = nullptr;
        event_slots.clear();
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

ReportPlotAccumulator::ReportPlotAccumulator() :
    runtime_(create_large<Runtime>()) {}

ReportPlotAccumulator::~ReportPlotAccumulator() {
    if (runtime_) runtime_->clear();
    destroy_large(runtime_);
}

bool ReportPlotAccumulator::begin(const ReportReadPlan &plan,
                                  int64_t start_ms,
                                  int64_t end_ms,
                                  size_t bucket_budget) {
    failure_reason_ = nullptr;
    if (!runtime_) {
        failure_reason_ = "report_plot_runtime_unavailable";
        return false;
    }
    if (end_ms <= start_ms || bucket_budget == 0) {
        failure_reason_ = "report_plot_window_invalid";
        return false;
    }

    runtime_->clear();
    runtime_->plan = &plan;
    runtime_->plot_start_ms = start_ms;
    runtime_->plot_end_ms = end_ms;

    for (size_t i = 0; i < plan.mapping_count(); ++i) {
        const ReportReadMapping *mapping = plan.mapping(i);
        if (!mapping) {
            failure_reason_ = "report_plot_mapping_invalid";
            runtime_->clear();
            return false;
        }

        const size_t signal = static_cast<size_t>(mapping->series.signal);
        if (signal >= SIGNAL_COUNT) {
            failure_reason_ = "report_plot_signal_invalid";
            runtime_->clear();
            return false;
        }

        SeriesState &state = runtime_->series[signal];
        state.active = true;
        state.max_interval_ms = std::max(
            state.max_interval_ms, mapping->series.sample_interval_ms);
    }

    const uint64_t span = static_cast<uint64_t>(end_ms - start_ms);
    const int64_t base_bucket_ms = std::max<int64_t>(
        1,
        static_cast<int64_t>((span + bucket_budget - 1) / bucket_budget));
    size_t total_cells = 0;
    for (SeriesState &state : runtime_->series) {
        if (!state.active) continue;

        state.bucket_ms = std::max<int64_t>(base_bucket_ms,
                                            state.max_interval_ms);
        state.cell_count = static_cast<size_t>(
            (span + static_cast<uint64_t>(state.bucket_ms) - 1) /
            static_cast<uint64_t>(state.bucket_ms));
        if (state.cell_count == 0 || state.cell_count > bucket_budget ||
            total_cells > std::numeric_limits<size_t>::max() -
                              state.cell_count) {
            failure_reason_ = "report_plot_bucket_layout_invalid";
            runtime_->clear();
            return false;
        }
        total_cells += state.cell_count;
    }

    if (total_cells > 0) {
        runtime_->cell_storage = static_cast<EnvelopeCell *>(
            allocate_large(total_cells, sizeof(EnvelopeCell)));
        if (!runtime_->cell_storage) {
            failure_reason_ = "report_plot_cells_allocation_failed";
            runtime_->clear();
            return false;
        }

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

bool ReportPlotAccumulator::accept_series(
    uint16_t session_index,
    const ReportSeriesDescriptor &series,
    const ReportSeriesSample &sample) {
    if (!runtime_ || !runtime_->active || !runtime_->plan) {
        failure_reason_ = "report_plot_series_context_invalid";
        return false;
    }
    if (session_index >= runtime_->plan->session_count()) {
        failure_reason_ = "report_plot_series_session_invalid";
        return false;
    }

    const size_t signal = static_cast<size_t>(series.signal);
    if (signal >= SIGNAL_COUNT) {
        failure_reason_ = "report_plot_series_signal_invalid";
        return false;
    }

    SeriesState &state = runtime_->series[signal];
    if (!state.active || !state.cells || state.bucket_ms <= 0) {
        failure_reason_ = "report_plot_series_not_initialized";
        return false;
    }
    if (sample.timestamp_ms < runtime_->plot_start_ms ||
        sample.timestamp_ms >= runtime_->plot_end_ms) {
        failure_reason_ = "report_plot_sample_outside_plot";
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    if (!session) {
        failure_reason_ = "report_plot_session_missing";
        return false;
    }
    if (sample.timestamp_ms < session->output_window.start_ms ||
        sample.timestamp_ms >= session->output_window.end_ms) {
        failure_reason_ = "report_plot_sample_outside_session";
        return false;
    }

    const uint64_t delta = static_cast<uint64_t>(
        sample.timestamp_ms - runtime_->plot_start_ms);
    const size_t bucket = static_cast<size_t>(
        delta / static_cast<uint64_t>(state.bucket_ms));
    if (bucket >= state.cell_count) {
        failure_reason_ = "report_plot_bucket_out_of_range";
        return false;
    }

    const int32_t value = scaled_plot_value(series, sample.value_milli);
    EnvelopeCell &cell = state.cells[bucket];
    if (!cell.present) {
        cell.minimum = value;
        cell.maximum = value;
        cell.present = true;
    } else {
        cell.minimum = std::min(cell.minimum, value);
        cell.maximum = std::max(cell.maximum, value);
    }

    if (series.signal == ReportSignalId::MaskPressure) {
        runtime_->pressure_sum_milli += value;
        ++runtime_->pressure_samples;
    } else if (series.signal == ReportSignalId::Leak) {
        runtime_->leak_sum_milli += value;
        ++runtime_->leak_samples;
    }
    return true;
}

bool ReportPlotAccumulator::accept_event(
    uint16_t session_index,
    const ReportEventRecord &event) {
    if (!runtime_ || !runtime_->active || !runtime_->plan) {
        failure_reason_ = "report_plot_event_context_invalid";
        return false;
    }
    if (session_index >= runtime_->plan->session_count()) {
        failure_reason_ = "report_plot_event_session_invalid";
        return false;
    }

    const ReportReadSession *session = runtime_->plan->session(session_index);
    if (!session) {
        failure_reason_ = "report_plot_event_session_missing";
        return false;
    }
    if (!report_event_overlaps_window(
            event,
            session->output_window.start_ms,
            session->output_window.end_ms) ||
        !report_event_overlaps_window(
            event, runtime_->plot_start_ms, runtime_->plot_end_ms)) {
        failure_reason_ = "report_plot_event_outside_window";
        return false;
    }

    EventSlot slot;
    slot.event = event;
    if (!runtime_->event_slots.append(
            reinterpret_cast<const uint8_t *>(&slot), sizeof(slot))) {
        failure_reason_ = "report_plot_event_allocation_failed";
        return false;
    }
    return true;
}

std::shared_ptr<const LargeByteBuffer> ReportPlotAccumulator::finish(
    ReportPlotAccumulatorSummary &summary) {
    summary = {};
    if (!runtime_ || !runtime_->active || !runtime_->plan ||
        (runtime_->event_slots.size() % sizeof(EventSlot)) != 0) {
        failure_reason_ = "report_plot_finish_state_invalid";
        return {};
    }

    const size_t event_count = runtime_->event_slots.size() /
        sizeof(EventSlot);
    EventSlot *events = reinterpret_cast<EventSlot *>(
        runtime_->event_slots.mutable_data());

    ReportSpoolBuffer plot;
    plot.set_max_size(AC_REPORT_PLOT_MAX_BYTES);
    if (!plot.reserve_capacity(AC_REPORT_PLOT_INITIAL_RESERVE) ||
        !bin_put_u32(plot, PLOT_BIN_MAGIC) ||
        !bin_put_u16(plot, PLOT_BIN_VERSION) ||
        !bin_put_u16(plot, 0) ||
        !bin_put_i64(plot, runtime_->plot_start_ms)) {
        failure_reason_ = "report_plot_output_allocation_failed";
        return {};
    }

    size_t event_count_offset = 0;
    uint8_t *event_count_bytes =
        plot.append_uninitialized(sizeof(uint32_t), event_count_offset);
    if (!event_count_bytes) {
        failure_reason_ = "report_plot_output_allocation_failed";
        return {};
    }
    memset(event_count_bytes, 0, sizeof(uint32_t));

    uint32_t unique_events = 0;
    if (!append_events(plot,
                       events,
                       event_count,
                       runtime_->plot_start_ms,
                       summary.events,
                       unique_events) ||
        !append_series(plot, runtime_->series)) {
        failure_reason_ = "report_plot_encode_failed";
        return {};
    }

    uint8_t *plot_bytes = plot.mutable_data();
    plot_bytes[event_count_offset] = static_cast<uint8_t>(unique_events);
    plot_bytes[event_count_offset + 1] =
        static_cast<uint8_t>(unique_events >> 8);
    plot_bytes[event_count_offset + 2] =
        static_cast<uint8_t>(unique_events >> 16);
    plot_bytes[event_count_offset + 3] =
        static_cast<uint8_t>(unique_events >> 24);

    summary.pressure_sum_milli = runtime_->pressure_sum_milli;
    summary.leak_sum_milli = runtime_->leak_sum_milli;
    summary.pressure_samples = runtime_->pressure_samples;
    summary.leak_samples = runtime_->leak_samples;
    std::shared_ptr<const LargeByteBuffer> frozen = freeze_copy(plot);
    if (!frozen) failure_reason_ = "report_plot_output_allocation_failed";
    return frozen;
}

void ReportPlotAccumulator::clear() {
    if (runtime_) runtime_->clear();
}

}  // namespace aircannect
