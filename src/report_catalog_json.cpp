#include "report_catalog_json.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {
namespace {

void append_session_json(LargeTextBuffer &json,
                         const NightCatalogTimeRange &session) {
    char number[32] = {};

    json += "{\"start\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(session.start_ms));
    json += number;
    json += ",\"end\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(session.end_ms));
    json += number;
    json += ",\"duration_min\":";
    snprintf(number,
             sizeof(number),
             "%lld",
             static_cast<long long>(
                 (session.end_ms - session.start_ms) / 60000));
    json += number;
    json += '}';
}

uint64_t catalog_identity(const NightCatalog &catalog,
                          const ReportSignalStoreCatalog *store) {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t value) {
        for (size_t byte = 0; byte < sizeof(value); ++byte) {
            hash ^= static_cast<uint8_t>(value >> (byte * 8));
            hash *= 1099511628211ULL;
        }
    };

    mix(catalog.size());
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *night = catalog.record(i);
        if (!night) continue;

        mix(static_cast<uint32_t>(night->sleep_day.epoch_days()));
        mix(night->source_revision.value());
        const ReportSignalStoreCatalogRecord *stored = store
            ? store->find(night->sleep_day) : nullptr;
        mix(stored ? stored->generation : 0);
        mix(stored ? stored->source_revision.value() : 0);
        mix(stored && stored->metadata ? stored->metadata->size() : 0);
    }
    return hash;
}

bool format_catalog_etag(const NightCatalog &catalog,
                         const ReportSignalStoreCatalog *store,
                         uint32_t generation,
                         char *out,
                         size_t out_size) {
    const int written = snprintf(
        out,
        out_size,
        "\"catalog-%08lx-%016llx\"",
        static_cast<unsigned long>(generation),
        static_cast<unsigned long long>(catalog_identity(catalog, store)));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace

std::shared_ptr<const ReportCatalogJson> build_report_catalog_json(
    const NightCatalog &catalog,
    const ReportSignalStoreCatalog *store,
    uint32_t generation,
    std::shared_ptr<const ReportCatalogJson> previous) {
    char etag[sizeof(ReportCatalogJson::etag)] = {};
    if (!format_catalog_etag(catalog, store, generation, etag, sizeof(etag))) {
        return {};
    }
    if (previous && strcmp(previous->etag, etag) == 0) return previous;

    auto next = std::make_shared<ReportCatalogJson>();
    if (!next || !next->body.reserve(256 + catalog.size() * 288)) return {};

    next->generation = generation;
    memcpy(next->etag, etag, sizeof(etag));
    LargeTextBuffer &json = next->body;

    char number[32] = {};
    json = "{\"state\":\"ready\",\"generation\":";
    snprintf(number,
             sizeof(number),
             "%lu",
             static_cast<unsigned long>(generation));
    json += number;
    json += ",\"nights\":[";
    bool first = true;
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *night = catalog.record(i);
        if (!night || night->unresolved_history) continue;

        if (!first) json += ',';
        first = false;
        char day[9] = {};
        night->sleep_day.format_yyyymmdd(day, sizeof(day));
        const ReportSignalStoreCatalogRecord *stored = store
            ? store->find(night->sleep_day) : nullptr;
        if (stored && (stored->view.night.flags & REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED)) {
            stored = nullptr;
        }
        const bool materialized = stored &&
            stored->source_revision == night->source_revision;

        json += "{\"id\":\"";
        json += day;
        json += "\",\"start\":";
        snprintf(number,
                 sizeof(number),
                 "%lld",
                 static_cast<long long>(night->day_start_ms));
        json += number;
        json += ",\"end\":";
        snprintf(number,
                 sizeof(number),
                 "%lld",
                 static_cast<long long>(night->day_end_ms));
        json += number;
        json += ",\"duration_min\":";
        snprintf(number,
                 sizeof(number),
                 "%lu",
                 static_cast<unsigned long>(
                     night_catalog_duration_minutes(catalog, *night)));
        json += number;
        json += ",\"materialized\":";
        json += materialized ? "true" : "false";
        json += ",\"time_conflict\":";
        json += (night->source_flags & NIGHT_CATALOG_SOURCE_TIME_CONFLICT) ? "true" : "false";
        json += ",\"active\":";
        json += (night->source_flags & NIGHT_CATALOG_SOURCE_ACTIVE_CAPTURE)
            ? "true" : "false";
        json += ",\"report_generation\":";
        snprintf(number,
                 sizeof(number),
                 "%lu",
                 static_cast<unsigned long>(
                     materialized ? stored->generation : 0));
        json += number;
        json += ",\"sessions\":[";

        size_t session_count = 0;
        const NightCatalogTimeRange *sessions =
            catalog.sessions(*night, session_count);
        for (size_t session = 0; sessions && session < session_count;
             ++session) {
            if (session) json += ',';
            append_session_json(json, sessions[session]);
        }
        json += ']';

        // Published night metadata supplies trends; no historical builds or I/O.
        json += ",\"means\":[";
        const auto *report = stored ? &stored->view.night : nullptr;
        ReportNightMetrics metrics;
        if (report) metrics = report->metrics;
        else report_night_metrics_from_catalog(night->metrics, metrics);

        const int32_t values[] = {metrics.ahi_milli, metrics.ipap_mean_milli,
            metrics.leak_mean_milli,
            report ? report->statistics.tidal_volume.mean_milli : 0,
            report ? report->statistics.minute_ventilation.mean_milli : 0};
        const bool present[] = {
            (metrics.valid_mask & 1u) != 0,
            (metrics.valid_mask & REPORT_NIGHT_METRIC_IPAP_MEAN) != 0,
            (metrics.valid_mask & REPORT_NIGHT_METRIC_LEAK_MEAN) != 0,
            report && report->statistics.tidal_volume.valid,
            report && report->statistics.minute_ventilation.valid};
        for (size_t metric = 0; metric < 5; ++metric) {
            if (metric) json += ',';
            if (!present[metric]) { json += "null"; continue; }

            snprintf(number, sizeof(number), "%.3f", values[metric] / 1000.0);
            json += number;
        }
        json += "]}";
    }
    json += "]}";

    return json.overflowed() ? nullptr : next;
}

}  // namespace aircannect
