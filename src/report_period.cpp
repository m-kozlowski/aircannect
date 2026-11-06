#include "report_period.h"

#include <algorithm>
#include <limits.h>
#include <list>
#include "json_util.h"
#include "large_allocator.h"
#include "large_object.h"
#include "memory_manager.h"
#include "night_catalog_store_service.h"
#include "report_build_checkpoint.h"
#include "report_night_histogram.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr size_t MetricCount = ReportCalculatedMetrics::Count;
constexpr size_t Bins = ReportMetricAccumulator::HistogramBins;
constexpr const char *Names[] = {"mask_pressure", "leak", "minute_ventilation",
    "respiratory_rate", "tidal_volume", "spo2", "pressure", "epap",
    "flow_limitation", "snore", "inspiration_time", "expiration_time",
    "ie_ratio", "target_ventilation", "pulse"};
constexpr size_t DailyMeans[] = {6, 1, 4, 2};

struct PeriodNight {
    uint64_t duration_ms;
    uint64_t leak_ms;
    uint64_t leak_above_ms;
    uint32_t valid_mask;
    int32_t ahi_milli;
    int32_t arousal_index_milli;
    uint32_t csr_minutes;
    ReportEventCounts events;
    uint32_t flags;
    uint8_t available_event_mask;
    uint8_t missing_event_mask;
    uint8_t mean_mask = 0;
    int32_t means[4] = {};

    explicit PeriodNight(const ReportSignalStoreNight &night)
        : duration_ms(night.closed_therapy_duration_ms),
          leak_ms(night.statistics.leak_recorded_ms),
          leak_above_ms(night.statistics.leak_above_threshold_ms),
          valid_mask(night.metrics.valid_mask), ahi_milli(night.metrics.ahi_milli),
          arousal_index_milli(night.metrics.arousal_index_milli),
          csr_minutes(night.metrics.csr_minutes),
          events(night.events), flags(night.flags),
          available_event_mask(night.available_event_mask),
          missing_event_mask(night.missing_event_mask) {
        for (size_t i = 0; i < 4; ++i) {
            const auto &statistic = night.statistics.at(DailyMeans[i]);
            if (!statistic.valid) continue;
            mean_mask |= 1u << i;
            means[i] = statistic.mean_milli;
        }
    }
};

struct PeriodHistogram {
    uint64_t bins[Bins] = {};
    double sum = 0;
    uint64_t weight = 0;
    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    uint16_t nights = 0;
    bool extrema = true;
};

bool has(const ReportNightMetrics &metrics, NightCatalogMetric field) {
    return (metrics.valid_mask & (1u << static_cast<unsigned>(field))) != 0;
}

void append_metric(LargeTextBuffer &json, bool valid, double milli) {
    if (valid) append_json_float(json, milli / 1000.0);
    else json += "null";
}

bool metadata_matches_snapshot(
    const ReportSignalStoreCatalogRecord &snapshot,
    const ReportSignalStoreCatalogRecord &projected) {
    return projected.sleep_day == snapshot.sleep_day &&
        projected.source_revision == snapshot.source_revision &&
        projected.generation == snapshot.generation &&
        projected.rejected_source_revision == snapshot.rejected_source_revision &&
        projected.flags == snapshot.flags &&
        projected.source_flags == snapshot.source_flags &&
        projected.checkpoint_slot == snapshot.checkpoint_slot &&
        projected.compression_version == snapshot.compression_version &&
        projected.metadata_size == snapshot.metadata_size &&
        projected.trend_valid_mask == snapshot.trend_valid_mask &&
        std::equal(projected.trend_values, projected.trend_values + 5,
                   snapshot.trend_values);
}

}  // namespace

bool ReportPeriodKey::valid() const {
    return end.valid() && first().valid() &&
        (days == 7 || days == 30 || days == 90 || days == 180 || days == 365);
}

SleepDayId ReportPeriodKey::first() const {
    SleepDayId day;
    if (end.valid() && days) {
        SleepDayId::from_epoch_days(static_cast<int64_t>(end.epoch_days()) - days + 1, day);
    }
    return day;
}

uint64_t report_period_revision(ReportPeriodKey key, const NightCatalog &catalog,
                               const ReportSignalStoreCatalog *store,
                               const NightCatalogLayoutIssues *layout_issues) {
    // The cache identity contains only source/publication versions in this range.
    uint64_t revision = 1469598103934665603ULL;
    for (int32_t d = key.first().epoch_days(); d <= key.end.epoch_days(); ++d) {
        SleepDayId day;
        SleepDayId::from_epoch_days(d, day);
        const auto *source = catalog.find(day);
        const auto *saved = store ? store->find(day) : nullptr;
        const uint64_t values[] = {static_cast<uint64_t>(d),
            source ? source->source_revision.value() : 0,
            saved ? saved->source_revision.value() : 0,
            saved ? saved->generation : 0,
            saved ? saved->rejected_source_revision.value() : 0};
        for (uint64_t value : values) {
            revision = (revision ^ value) * 1099511628211ULL;
        }
        if (source && layout_issues && layout_issues->blocked(day)) {
            revision = (revision ^ 0x424c4f434b45444eULL) * 1099511628211ULL;
            revision = (revision ^ static_cast<uint64_t>(d)) * 1099511628211ULL;
        }
    }
    return revision;
}

struct ReportPeriodService::Cache {
    struct Histogram {
        std::shared_ptr<const LargeByteBuffer> bytes;
        ReportNightHistogramView view;

        size_t storage_bytes() const {
            return bytes->size() + sizeof(Histogram);
        }
    };

    struct Entry {
        ReportSignalStoreCatalogRecord snapshot;
        PeriodNight night;
        std::unique_ptr<Histogram, void (*)(Histogram *)> histogram{
            nullptr, LargeObject::destroy<Histogram>};
    };

    static constexpr size_t EntryBytes = sizeof(Entry) + 2 * sizeof(void *);
    std::list<Entry, LargeAllocator<Entry>> entries;
    size_t bytes = 0;

    Entry *find(const ReportSignalStoreCatalogRecord &snapshot) {
        for (auto &entry : entries) {
            if (!metadata_matches_snapshot(snapshot, entry.snapshot)) continue;
            return &entry;
        }
        return nullptr;
    }

    bool make_room(size_t cost, SleepDayId day, bool summary) {
        const size_t budget = Memory::buffer_budget(NightCacheBytes, 64 * 1024);
        if (cost > budget) return false;
        if (!summary && entries.size() * EntryBytes + cost > budget) return false;

        // Histogram eviction leaves the smaller, more useful summary intact.
        while (bytes + cost > budget) {
            Entry *oldest = nullptr;
            for (auto &entry : entries) {
                if (entry.histogram && (summary || entry.snapshot.sleep_day < day) &&
                    (!oldest || entry.snapshot.sleep_day < oldest->snapshot.sleep_day)) {
                    oldest = &entry;
                }
            }
            if (!oldest) break;
            bytes -= oldest->histogram->storage_bytes();
            oldest->histogram.reset();
        }

        if (summary) {
            while (bytes + cost > budget) {
                auto oldest = entries.end();
                for (auto it = entries.begin(); it != entries.end(); ++it) {
                    if (it->snapshot.sleep_day < day && (oldest == entries.end() ||
                        it->snapshot.sleep_day < oldest->snapshot.sleep_day)) {
                        oldest = it;
                    }
                }
                if (oldest == entries.end()) break;
                bytes -= EntryBytes;
                entries.erase(oldest);
            }
        }
        return bytes + cost <= budget;
    }

    void remember_summary(const ReportSignalStoreCatalogRecord &snapshot,
                          const ReportSignalStoreNight &night) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (it->snapshot.sleep_day != snapshot.sleep_day) { ++it; continue; }
            bytes -= EntryBytes + (it->histogram ? it->histogram->storage_bytes() : 0);
            it = entries.erase(it);
        }

        if (!make_room(EntryBytes, snapshot.sleep_day, true)) return;
        try {
            entries.push_back({snapshot, PeriodNight(night)});
            bytes += EntryBytes;
        } catch (const std::bad_alloc &) {
            // Retention is optional; the current calculation owns its data.
        }
    }

    void remember_histogram(const ReportSignalStoreCatalogRecord &snapshot,
                            std::shared_ptr<const LargeByteBuffer> data,
                            const ReportNightHistogramView &view) {
        Entry *entry = find(snapshot);
        if (!entry) return;

        if (entry->histogram) {
            bytes -= entry->histogram->storage_bytes();
            entry->histogram.reset();
        }
        const size_t cost = data->size() + sizeof(Histogram);
        if (!make_room(cost, snapshot.sleep_day, false)) return;

        entry->histogram.reset(LargeObject::create<Histogram>(
            Histogram{std::move(data), view}));
        if (entry->histogram) bytes += cost;
    }
};

struct ReportPeriodService::Runtime {
    std::shared_ptr<const NightCatalog> catalog;
    std::shared_ptr<const ReportSignalStoreCatalog> store;
    std::shared_ptr<const NightCatalogLayoutIssues> layout_issues;
    std::shared_ptr<ReportPeriodResult> result;
    PeriodHistogram histograms[MetricCount];
    ReportMetricAccumulator night_metrics;
    ReportMetricAccumulator tail_metrics;
    std::shared_ptr<const LargeByteBuffer> bytes;
    ReportBuildCheckpointView checkpoint;
    std::shared_ptr<const LargeByteBuffer> histogram_bytes;
    ReportNightHistogramView histogram;
    const ReportSignalStoreCatalogRecord *saved = nullptr;
    ReportSignalStoreMetadata metadata;
    LargeTextBuffer daily;
    uint16_t cursor = 0;
    uint16_t nights = 0;
    uint16_t active_nights = 0;
    uint16_t outdated_nights = 0;
    uint16_t retained_nights = 0;
    uint16_t checkpoint_missing = 0;
    uint16_t layout_blocked = 0;
    uint16_t event_nights = 0;
    uint16_t ahi_nights = 0;
    uint16_t csr_nights = 0;
    size_t tail = 0;
    size_t metric = 0;
    double minutes = 0;
    double ahi_sum = 0;
    double ahi_minutes = 0;
    double rdi_sum = 0;
    double rdi_minutes = 0;
    uint64_t events[5] = {};
    uint64_t csr_minutes = 0;
    uint64_t leak_ms = 0;
    uint64_t leak_above_ms = 0;
    enum class Phase { Night, ReadingMetadata, ReadingHistogram,
                       ReadCheckpoint, Reading, Tails, WriteHistogram,
                       Metrics, Finish, Done } phase = Phase::Night;
    Phase after_metadata = Phase::Night;

    void fail(const char *error) {
        copy_cstr(result->error, sizeof(result->error), error);
        checkpoint = {};
        histogram = {};
        bytes.reset();
        histogram_bytes.reset();
        drop_metadata();
        saved = nullptr;
        phase = Phase::Done;
    }

    void drop_metadata() {
        metadata.view = {};
        metadata.metadata.reset();
    }

    void finish_night() {
        checkpoint = {};
        bytes.reset();
        night_metrics.clear();
        tail_metrics.clear();
        histogram = {};
        histogram_bytes.reset();
        drop_metadata();
        saved = nullptr;
        ++cursor;
        phase = Phase::Night;
    }

    void accept_metadata(SleepDayId day, const PeriodNight *night) {
        const auto *source = catalog->find(day);
        if (source && source->unresolved_history) source = nullptr;
        ReportNightMetrics m;
        if (night) {
            m.valid_mask = night->valid_mask;
            m.ahi_milli = night->ahi_milli;
            m.arousal_index_milli = night->arousal_index_milli;
            m.csr_minutes = night->csr_minutes;
        }
        else if (source) report_night_metrics_from_catalog(source->metrics, m);

        const bool open = (night && (night->flags & REPORT_SIGNAL_STORE_NIGHT_OPEN_SESSION)) ||
            (source && (source->source_flags & NIGHT_CATALOG_SOURCE_ACTIVE_CAPTURE));
        if (open) ++active_nights;
        const double duration = night ? night->duration_ms / 60000.0
            : source ? night_catalog_duration_minutes(*catalog, *source) : 0;
        const bool available = !open && duration > 0;
        const bool changed = saved && source && saved->source_revision != source->source_revision;
        const bool retained = changed && saved->rejected_source_revision == source->source_revision;
        if (changed) {
            if (retained) ++retained_nights;
            else ++outdated_nights;
        }

        char id[9];
        day.format_yyyymmdd(id, sizeof(id));
        if (cursor) daily += ',';
        daily += '{';
        json_add_string(daily, "id", id, false);
        if (retained) json_add_bool(daily, "retained", true);
        daily += ",\"duration_min\":";
        if (available) append_json_float(daily, duration);
        else daily += "null";
        daily += ",\"means\":[";
        append_metric(daily, available && has(m, NightCatalogMetric::Ahi), m.ahi_milli);
        for (size_t index = 0; index < 4; ++index) {
            daily += ',';
            append_metric(daily, available && night && (night->mean_mask & (1u << index)),
                          night ? night->means[index] : 0);
        }
        daily += "]}";
        if (!available) { saved = nullptr; return; }

        ++nights;
        minutes += duration;
        if (has(m, NightCatalogMetric::Ahi)) {
            ahi_sum += m.ahi_milli * duration;
            ahi_minutes += duration;
            ++ahi_nights;
            if (has(m, NightCatalogMetric::ArousalIndex)) {
                rdi_sum += (static_cast<double>(m.ahi_milli) + m.arousal_index_milli) * duration;
                rdi_minutes += duration;
            }
        }
        if (has(m, NightCatalogMetric::CsrMinutes)) {
            csr_minutes += m.csr_minutes;
            ++csr_nights;
        }
        if (!night) return;

        leak_ms += night->leak_ms;
        leak_above_ms += night->leak_above_ms;
        if ((night->available_event_mask & REPORT_EVENT_SCORED) &&
            !(night->missing_event_mask & REPORT_EVENT_SCORED)) {
            ++event_nights;
            events[0] += night->events.central_apnea;
            events[1] += night->events.obstructive_apnea;
            events[2] += night->events.unknown_apnea;
            events[3] += night->events.hypopnea;
            events[4] += night->events.arousal;
        }
    }

    void start_night_work(Cache &cache, StorageBoundedFileLoader &loader,
                          SleepDayId day, const Cache::Entry *cached = nullptr) {
        const PeriodNight night = cached ? cached->night : PeriodNight(metadata.view.night);
        if (saved && !cached && metadata.metadata) {
            cache.remember_summary(*saved, metadata.view.night);
        }
        accept_metadata(day, cached || metadata.metadata ? &night : nullptr);

        if (!saved) {
            finish_night();
            return;
        }

        if (cached && cached->histogram) {
            histogram_bytes = cached->histogram->bytes;
            histogram = cached->histogram->view;
            metric = 0;
            phase = Phase::Metrics;
            return;
        }

        char path[AC_STORAGE_PATH_MAX];
        if (!saved->checkpoint_slot || !ReportNightHistogramCodec::path(
                saved->sleep_day, saved->generation, saved->checkpoint_slot,
                path, sizeof(path))) {
            ++checkpoint_missing;
            finish_night();
            return;
        }

        if (loader.start(path, ReportNightHistogramCodec::MaxBytes, 1,
                         StorageReadLane::Report, true) != OperationAdmission::Accepted) {
            fail("period_read_rejected");
        } else {
            phase = Phase::ReadingHistogram;
        }
    }

    bool read_metadata(ReportSignalStoreMetadataLoader &loader, Phase resume) {
        const OperationAdmission admitted = loader.start(
            saved->sleep_day, 1, StorageReadLane::Report, true);
        if (admitted == OperationAdmission::Busy) return false;

        if (admitted != OperationAdmission::Accepted) {
            fail("report_store_metadata_read_rejected");
        } else {
            after_metadata = resume;
            phase = Phase::ReadingMetadata;
        }
        return true;
    }

    void accept_histogram(size_t index) {
        const auto &h = histogram.metrics[index];
        if (!h.samples) return;
        const uint32_t interval = h.interval_ms;

        auto &out = histograms[index];
        for (size_t i = 0; i < h.count; ++i) {
            uint16_t bin;
            uint32_t samples;
            h.bin(i, bin, samples);
            out.bins[bin] += static_cast<uint64_t>(samples) * interval;
        }
        out.sum += static_cast<double>(h.sum_milli) * interval;
        out.weight += h.samples * interval;
        out.minimum = std::min(out.minimum, h.minimum);
        out.maximum = std::max(out.maximum, h.maximum);
        out.extrema &= h.extrema_valid;
        ++out.nights;
    }

    void finish() {
        auto &json = result->body;
        json = "{";
        char day[9];
        result->key.first().format_yyyymmdd(day, sizeof(day));
        json_add_string(json, "first", day, false);
        result->key.end.format_yyyymmdd(day, sizeof(day));
        json_add_string(json, "end", day);
        json_add_int(json, "days", result->key.days);
        json_add_int(json, "nights", nights);
        json_add_int(json, "active_nights", active_nights);
        json_add_int(json, "outdated_nights", outdated_nights);
        json_add_int(json, "retained_nights", retained_nights);
        json_add_int(json, "checkpoint_missing", checkpoint_missing);
        json_add_int(json, "layout_blocked", layout_blocked);
        json_add_float(json, "duration_min", minutes);
        json += ",\"ahi\":";
        append_metric(json, ahi_minutes > 0, ahi_minutes ? ahi_sum / ahi_minutes : 0);
        json_add_int(json, "ahi_nights", ahi_nights);
        json += ",\"rdi\":";
        append_metric(json, rdi_minutes > 0, rdi_minutes ? rdi_sum / rdi_minutes : 0);
        json += ",\"csr_minutes\":";
        if (csr_nights) append_json_float(json, csr_minutes);
        else json += "null";
        json_add_int(json, "csr_nights", csr_nights);
        json += ",\"leak_duration\":{";
        json_add_uint64(json, "recorded_ms", leak_ms, false);
        json_add_uint64(json, "above_ms", leak_above_ms);
        json_add_int(json, "threshold", 24);
        json += "},\"events\":[";
        for (size_t i = 0; i < 5; ++i) {
            if (i) json += ',';
            if (event_nights) append_json_float(json, events[i]);
            else json += "null";
        }
        json += ']';
        json_add_int(json, "event_nights", event_nights);
        json += ",\"statistics\":{";
        for (size_t index = 0; index < MetricCount; ++index) {
            if (index) json += ',';
            json += '"'; json += Names[index]; json += "\":{";
            const auto &h = histograms[index];
            json_add_int(json, "nights", h.nights, false);
            json_add_uint64(json, "recorded_ms", h.weight);
            json += ",\"min\":";
            append_metric(json, h.weight && h.extrema, h.minimum);
            json += ",\"mean\":";
            append_metric(json, h.weight, h.weight ? h.sum / h.weight : 0);
            for (unsigned percentile : {50u, 95u}) {
                json += percentile == 50 ? ",\"median\":" : ",\"p95\":";
                uint64_t cumulative = 0;
                size_t bin = 0;
                for (; bin + 1 < Bins; ++bin) {
                    cumulative += h.bins[bin];
                    if (cumulative && cumulative >= (h.weight * percentile + 99) / 100) break;
                }
                append_metric(json, h.weight, ReportMetricAccumulator::bin_value(index, bin));
            }
            json += ",\"max\":";
            append_metric(json, h.weight && h.extrema, h.maximum);
            json += '}';
        }
        json += "},\"nights_daily\":[";
        json.append(daily.c_str(), daily.length());
        json += "]}";
        if (json.overflowed() || daily.overflowed()) {
            copy_cstr(result->error, sizeof(result->error), "period_alloc");
        }
        drop_metadata();
        phase = Phase::Done;
        catalog.reset();
        store.reset();
    }
};

ReportPeriodService::ReportPeriodService() = default;
ReportPeriodService::~ReportPeriodService() { cancel(); }

void ReportPeriodService::begin(StorageReadPort &port, StorageAtomicWritePort &writes) {
    cancel();
    clear_cache();
    metadata_loader_.begin(port);
    loader_.begin(port);
    write_port_ = &writes;
}

void ReportPeriodService::clear_cache() { cache_.reset(); }

bool ReportPeriodService::start(ReportPeriodKey key,
    std::shared_ptr<const NightCatalog> catalog,
    std::shared_ptr<const ReportSignalStoreCatalog> store, uint64_t revision,
    std::shared_ptr<const NightCatalogLayoutIssues> layout_issues) {
    cancel();
    if (!key.valid() || !catalog) return false;
    try {
        if (!cache_) cache_ = std::allocate_shared<Cache>(LargeAllocator<Cache>());
        runtime_ = std::allocate_shared<Runtime>(LargeAllocator<Runtime>());
        runtime_->result = std::make_shared<ReportPeriodResult>();
    } catch (const std::bad_alloc &) { cancel(); return false; }
    auto &r = *runtime_;
    r.result->key = key;
    r.result->revision = revision;
    r.catalog = std::move(catalog);
    r.store = std::move(store);
    r.layout_issues = std::move(layout_issues);
    if (!r.daily.reserve(key.days * 160) || !r.result->body.reserve(key.days * 160 + 8192)) {
        cancel();
        return false;
    }
    return true;
}

bool ReportPeriodService::active() const {
    return runtime_ && runtime_->phase != Runtime::Phase::Done;
}

bool ReportPeriodService::poll() {
    if (!active()) return false;
    auto &r = *runtime_;
    using Phase = Runtime::Phase;
    if (r.phase == Phase::Night) {
        if (r.cursor == r.result->key.days) { r.phase = Phase::Finish; return true; }
        SleepDayId day;
        SleepDayId::from_epoch_days(r.result->key.first().epoch_days() + r.cursor, day);

        if (r.layout_issues && r.catalog->find(day) &&
            r.layout_issues->blocked(day)) {
            ++r.layout_blocked;

            char id[9];
            day.format_yyyymmdd(id, sizeof(id));
            if (r.cursor) r.daily += ',';
            r.daily += '{';
            json_add_string(r.daily, "id", id, false);
            r.daily += ",\"duration_min\":null,\"means\":[null,null,null,null,null]}";

            ++r.cursor;
            return true;
        }

        r.saved = r.store ? r.store->find(day) : nullptr;
        r.drop_metadata();
        if (r.saved && (r.saved->flags & REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED)) {
            r.saved = nullptr;
        }

        if (r.saved) {
            if (const auto *cached = cache_->find(*r.saved)) {
                r.start_night_work(*cache_, loader_, day, cached);
                return true;
            }

            const ReportSignalStoreMetadata *cached = r.store->cached(day);
            if (cached) {
                r.metadata = *cached;
            } else {
                return r.read_metadata(metadata_loader_, Phase::Night);
            }
        }

        r.start_night_work(*cache_, loader_, day);
        return true;
    }
    if (r.phase == Phase::ReadingMetadata) {
        const bool worked = metadata_loader_.poll();
        const StorageBoundedFileLoadStatus status = metadata_loader_.status();
        if (status.active()) return worked;

        if (status.state != StorageBoundedFileLoadState::Ready) {
            metadata_loader_.reset();
            if (status.state == StorageBoundedFileLoadState::Missing) {
                r.fail("report_store_metadata_missing");
            } else {
                r.fail(status.error[0] ? status.error
                                      : "report_store_metadata_read_failed");
            }
            return true;
        }

        r.metadata = metadata_loader_.take_completed();
        metadata_loader_.reset();
        if (!r.saved || !metadata_matches_snapshot(*r.saved,
                ReportSignalStoreCatalogBuilder::project(r.metadata))) {
            r.fail("report_store_metadata_changed");
            return true;
        }

        if (r.after_metadata == Phase::ReadCheckpoint) {
            r.phase = Phase::ReadCheckpoint;
            return true;
        }

        SleepDayId day;
        SleepDayId::from_epoch_days(
            r.result->key.first().epoch_days() + r.cursor, day);
        r.start_night_work(*cache_, loader_, day);
        return true;
    }
    if (r.phase == Phase::ReadingHistogram) {
        const bool worked = loader_.poll();
        if (loader_.status().active()) return worked;
        const auto status = loader_.status();
        r.histogram_bytes = loader_.take_completed();
        loader_.reset();

        if (r.histogram_bytes && ReportNightHistogramCodec::decode(
                r.histogram_bytes->data(), r.histogram_bytes->size(), r.histogram) &&
            r.histogram.matches(r.saved->sleep_day, r.saved->source_revision,
                                r.saved->generation, r.saved->checkpoint_slot)) {
            cache_->remember_histogram(*r.saved, r.histogram_bytes, r.histogram);
            r.metric = 0;
            r.phase = Phase::Metrics;
        } else if (status.state == StorageBoundedFileLoadState::Ready ||
                   status.state == StorageBoundedFileLoadState::Missing ||
                   strcmp(status.error, "storage_file_size_invalid") == 0) {
            // Only this derived shortcut is expendable. Leave the checkpoint intact.
            r.histogram = {};
            r.histogram_bytes.reset();
            r.phase = Phase::ReadCheckpoint;
        } else {
            r.fail(status.error[0] ? status.error : "period_read_failed");
        }
        return true;
    }
    if (r.phase == Phase::ReadCheckpoint) {
        if (!r.metadata.metadata) {
            const auto *cached = r.store->cached(r.saved->sleep_day);
            if (cached) r.metadata = *cached;
            else return r.read_metadata(metadata_loader_, Phase::ReadCheckpoint);
        }

        char path[AC_STORAGE_PATH_MAX];
        const auto &night = r.metadata.view.night;
        if (!report_build_checkpoint_path(night.sleep_day, night.generation,
                night.checkpoint_slot, path, sizeof(path)) ||
            loader_.start(path, ReportBuildCheckpointCodec::MaxBytes, 1,
                StorageReadLane::Report) != OperationAdmission::Accepted) {
            r.fail("period_read_rejected");
        } else {
            r.phase = Phase::Reading;
        }
        return true;
    }
    if (r.phase == Phase::Reading) {
        const bool worked = loader_.poll();
        if (loader_.status().active()) return worked;
        r.bytes = loader_.take_completed();
        const auto status = loader_.status();
        loader_.reset();
        if (!r.bytes) {
            if (status.state != StorageBoundedFileLoadState::Missing) {
                r.fail(status.error[0] ? status.error : "period_read_failed");
            } else { ++r.checkpoint_missing; r.finish_night(); }
            return true;
        }
        const auto &night = r.metadata.view.night;
        if (!ReportBuildCheckpointCodec::decode(r.bytes->data(), r.bytes->size(), r.checkpoint) ||
            r.checkpoint.sleep_day != night.sleep_day || r.checkpoint.source_revision != night.source_revision ||
            r.checkpoint.generation != night.generation || r.checkpoint.track_count != night.track_count ||
            !r.night_metrics.restore(r.checkpoint.metrics, r.checkpoint.metrics_size)) {
            r.fail("period_checkpoint_invalid");
            return true;
        }
        r.tail = 0;
        r.phase = Phase::Tails;
        return true;
    }
    if (r.phase == Phase::Tails) {
        if (r.tail == r.checkpoint.track_count) {
            r.histogram_bytes = ReportNightHistogramCodec::encode(
                r.night_metrics, r.metadata.view, &r.histogram);
            if (!r.histogram_bytes) {
                r.fail("period_histogram_alloc");
                return true;
            }
            r.checkpoint = {};
            r.bytes.reset();
            r.night_metrics.clear();
            r.tail_metrics.clear();
            r.phase = Phase::WriteHistogram;
            return true;
        }
        ReportBuildTrackState track;
        r.checkpoint.track(r.tail++, track);
        if (track.tail_metrics_size &&
            (!r.tail_metrics.restore(track.tail_metrics, track.tail_metrics_size) ||
             !r.night_metrics.merge(r.tail_metrics))) {
            r.fail("period_tail_invalid");
        }
        return true;
    }
    if (r.phase == Phase::WriteHistogram) {
        if (!write_ticket_.valid()) {
            if (storage_low_space_) {
                cache_->remember_histogram(*r.saved, r.histogram_bytes, r.histogram);
                r.metric = 0;
                r.phase = Phase::Metrics;
                return true;
            }

            char path[AC_STORAGE_PATH_MAX];
            ReportNightHistogramCodec::path(
                r.metadata.view.night, path, sizeof(path));
            StorageAtomicWriteCommand command;
            command.path = path;
            command.bytes = r.histogram_bytes;
            command.generation = 1;
            command.lane = StorageAtomicWriteLane::Foreground;
            const auto submitted = write_port_->request_write(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) {
                r.fail("period_cache_write_rejected");
            } else {
                write_ticket_ = submitted.ticket;
            }
            return true;
        }

        StorageAtomicWriteCompletion completion;
        if (!write_port_->take_completion(write_ticket_, completion)) return false;
        write_ticket_ = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded ||
            completion.bytes_written != r.histogram_bytes->size()) {
            r.fail(completion.error[0] ? completion.error
                                       : "period_cache_write_failed");
        } else {
            cache_->remember_histogram(*r.saved, r.histogram_bytes, r.histogram);
            r.metric = 0;
            r.phase = Phase::Metrics;
        }
        return true;
    }
    if (r.phase == Phase::Metrics) {
        r.accept_histogram(r.metric++);
        if (r.metric == MetricCount) r.finish_night();
        return true;
    }
    r.finish();
    return true;
}

void ReportPeriodService::cancel() {
    metadata_loader_.reset();
    loader_.reset();
    if (write_port_ && write_ticket_.valid()) write_port_->abandon(write_ticket_);
    write_ticket_ = {};
    runtime_.reset();
}

std::shared_ptr<const ReportPeriodResult> ReportPeriodService::take_completed() {
    if (!runtime_ || active()) return {};
    auto out = std::move(runtime_->result);
    runtime_.reset();
    return out;
}

}  // namespace aircannect
