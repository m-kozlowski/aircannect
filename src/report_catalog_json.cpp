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
        mix(stored && stored->source_revision == night->source_revision
                ? stored->generation : 0);
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
    for (size_t i = 0; i < catalog.size(); ++i) {
        const NightCatalogRecord *night = catalog.record(i);
        if (!night) continue;

        if (i) json += ',';
        char day[9] = {};
        night->sleep_day.format_yyyymmdd(day, sizeof(day));
        const ReportSignalStoreCatalogRecord *stored = store
            ? store->find(night->sleep_day) : nullptr;
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
        json += "]}";
    }
    json += "]}";

    return json.overflowed() ? nullptr : next;
}

}  // namespace aircannect
