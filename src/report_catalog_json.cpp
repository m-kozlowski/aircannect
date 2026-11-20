#include "report_catalog_json.h"
#include "report_oximetry_signals.h"

#include <algorithm>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "large_allocator.h"
#include "board_oximetry.h"
#include "report_night_summary.h"

namespace aircannect {
namespace {

uint64_t catalog_identity(const NightCatalog &catalog,
                          const ReportSignalStoreCatalog *store,
                          const ReportOximetryCatalog *oximetry, uint64_t &oximetry_identity) {
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
        mix(stored ? stored->metadata_size : 0);
        if (stored) {
            mix(stored->flags & REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED);
            mix(stored->trend_valid_mask);
            for (size_t metric = 0; metric < 5; ++metric) {
                if (stored->trend_valid_mask & (1u << metric)) {
                    mix(static_cast<uint32_t>(stored->trend_values[metric]));
                }
            }
        }
    }
    const auto night_hash = hash;
    hash = 1469598103934665603ULL;
    mix(oximetry ? oximetry->size() : 0);
    if (oximetry) for (const auto &r : *oximetry) {
        for (const char *c = r.source; *c; ++c) mix(*c);
        for (const char *c = r.id; *c; ++c) mix(*c);
        for (const char *c = r.peer; *c; ++c) mix(*c);
        for (const char *c = r.device; *c; ++c) mix(*c);
        for (const char *c = r.serial; *c; ++c) mix(*c);
        mix(r.start_ms); mix(r.report_end_ms()); mix(r.size()); mix(r.interval_s); mix(r.modified);
        mix(r.noon_s); mix(r.next_noon_s);
        mix(r.samples); mix(r.csv_bytes);
        if (r.planes) {
            mix(r.planes->index_bytes); mix(r.planes->histogram_bytes); mix(r.planes->rows);
            for (const auto size : r.planes->plane_bytes) mix(size);
        }
    }
    oximetry_identity = hash;
    mix(night_hash);
    return hash;
}

bool format_catalog_etag(const NightCatalog &catalog,
                         const ReportSignalStoreCatalog *store,
                         const ReportOximetryCatalog *oximetry,
                         uint32_t generation,
                         char *out,
                         size_t out_size, uint64_t &oximetry_identity) {
    const int written = snprintf(
        out,
        out_size,
        "\"catalog-v4-%08lx-%016llx\"",
        static_cast<unsigned long>(generation),
        static_cast<unsigned long long>(catalog_identity(catalog, store, oximetry, oximetry_identity)));
    return written > 0 && static_cast<size_t>(written) < out_size;
}

}  // namespace

ReportCatalogJsonReader::ReportCatalogJsonReader(
    std::shared_ptr<const ReportCatalogJson> publication)
    : publication_(std::move(publication)) {
    failed_ = !publication_ || !publication_->catalog;
}

bool ReportCatalogJsonReader::format_fragment(const char *format, ...) {
    if (!format || failed_) return false;

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(fragment_, sizeof(fragment_), format, args);
    va_end(args);

    if (written < 0 || static_cast<size_t>(written) >= sizeof(fragment_)) {
        failed_ = true;
        return false;
    }

    fragment_length_ = static_cast<size_t>(written);
    fragment_offset_ = 0;
    return true;
}

bool ReportCatalogJsonReader::append_format(const char *format, ...) {
    if (!format || failed_ || fragment_length_ >= sizeof(fragment_)) {
        failed_ = true;
        return false;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(fragment_ + fragment_length_,
                                  sizeof(fragment_) - fragment_length_,
                                  format,
                                  args);
    va_end(args);

    if (written < 0 ||
        static_cast<size_t>(written) >= sizeof(fragment_) - fragment_length_) {
        failed_ = true;
        return false;
    }

    fragment_length_ += static_cast<size_t>(written);
    return true;
}

bool ReportCatalogJsonReader::append_text(const char *text) {
    return text && append_format("%s", text);
}

bool ReportCatalogJsonReader::prepare_fragment() {
    if (failed_) return false;

    const NightCatalog &catalog = *publication_->catalog;
    while (true) {
        switch (phase_) {
            case Phase::Prefix:
                phase_ = Phase::Nights;
                return format_fragment(
                    "{\"state\":\"ready\",\"generation\":%lu,\"nights\":[",
                    static_cast<unsigned long>(publication_->generation));

            case Phase::Nights:
                while (night_index_ < catalog.size()) {
                    const size_t index = night_index_++;
                    const NightCatalogRecord *night = catalog.record(index);
                    if (night->unresolved_history) continue;

                    current_night_ = night;
                    current_stored_ = publication_->store
                        ? publication_->store->find(night->sleep_day) : nullptr;
                    if (current_stored_ &&
                        (current_stored_->flags &
                         REPORT_SIGNAL_STORE_NIGHT_TIME_UNRESOLVED)) {
                        current_stored_ = nullptr;
                    }

                    // Summary includes unused calendar days. Keep the source
                    // record, but do not offer it as a night with therapy.
                    constexpr uint8_t summary_flags = NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK |
                        NIGHT_CATALOG_SOURCE_SUMMARY_EXPIRED;
                    const uint8_t source_flags = night->source_flags |
                        (current_stored_ ? current_stored_->source_flags : 0);
                    if ((night->source_flags & NIGHT_CATALOG_SOURCE_SUMMARY_FALLBACK) &&
                        !(source_flags & ~summary_flags) &&
                        !night->session_count && !night->metrics.valid_mask &&
                        (!current_stored_ ||
                         current_stored_->source_revision == night->source_revision)) {
                        continue;
                    }

                    phase_ = Phase::NightPrefix;
                    break;
                }
                if (phase_ == Phase::NightPrefix) continue;

                if (publication_->oximetry && !publication_->oximetry->empty()) {
                    phase_ = Phase::Recordings;
                    return format_fragment("],\"oximetry_live_stale_ms\":%u,\"oximetry\":[",
                        unsigned(AC_OXIMETRY_SAMPLE_STALE_MS));
                }
                phase_ = Phase::Done;
                return format_fragment("]}");

            case Phase::NightPrefix: {
                const NightCatalogRecord &night = *current_night_;
                const ReportSignalStoreCatalogRecord *stored = current_stored_;

                const bool materialized = stored &&
                    stored->source_revision == night.source_revision;
                char day[9] = {};
                if (!night.sleep_day.format_yyyymmdd(day, sizeof(day))) {
                    failed_ = true;
                    return false;
                }

                const bool formatted = format_fragment(
                    "%s{\"id\":\"%s\",\"start\":%lld,\"end\":%lld,"
                    "\"duration_min\":%lu,\"materialized\":%s,"
                    "\"time_conflict\":%s,\"active\":%s,"
                    "\"report_generation\":%lu,\"sessions\":[",
                    first_night_ ? "" : ",",
                    day,
                    static_cast<long long>(night.day_start_ms),
                    static_cast<long long>(night.day_end_ms),
                    static_cast<unsigned long>(
                        night_catalog_duration_minutes(catalog, night)),
                    materialized ? "true" : "false",
                    (night.source_flags & NIGHT_CATALOG_SOURCE_TIME_CONFLICT)
                        ? "true" : "false",
                    (night.source_flags & NIGHT_CATALOG_SOURCE_ACTIVE_CAPTURE)
                        ? "true" : "false",
                    static_cast<unsigned long>(
                        materialized ? stored->generation : 0));
                if (!formatted) return false;

                first_night_ = false;
                session_index_ = 0;
                phase_ = Phase::Sessions;
                return true;
            }

            case Phase::Sessions: {
                size_t session_count = 0;
                const NightCatalogTimeRange *sessions =
                    catalog.sessions(*current_night_, session_count);
                if (sessions && session_index_ < session_count) {
                    const NightCatalogTimeRange &session =
                        sessions[session_index_];
                    const bool formatted = format_fragment(
                        "%s{\"start\":%lld,\"end\":%lld,"
                        "\"duration_min\":%lld}",
                        session_index_ ? "," : "",
                        static_cast<long long>(session.start_ms),
                        static_cast<long long>(session.end_ms),
                        static_cast<long long>(
                            (session.end_ms - session.start_ms) / 60000));
                    if (!formatted) return false;

                    ++session_index_;
                    return true;
                }

                phase_ = Phase::Means;
                return format_fragment("],\"means\":[");
            }

            case Phase::Means: {
                int32_t values[5] = {};
                uint8_t valid_mask = 0;
                if (current_stored_) {
                    memcpy(values, current_stored_->trend_values, sizeof(values));
                    valid_mask = current_stored_->trend_valid_mask;
                } else {
                    ReportNightMetrics metrics;
                    report_night_metrics_from_catalog(current_night_->metrics,
                                                      metrics);
                    values[0] = metrics.ahi_milli;
                    values[1] = metrics.ipap_mean_milli;
                    values[2] = metrics.leak_mean_milli;
                    if (metrics.valid_mask & 1u) valid_mask |= 1u << 0;
                    if (metrics.valid_mask & REPORT_NIGHT_METRIC_IPAP_MEAN) {
                        valid_mask |= 1u << 1;
                    }
                    if (metrics.valid_mask & REPORT_NIGHT_METRIC_LEAK_MEAN) {
                        valid_mask |= 1u << 2;
                    }
                }

                if (!format_fragment("")) return false;
                for (size_t metric = 0; metric < 5; ++metric) {
                    if (metric && !append_text(",")) return false;
                    if (!(valid_mask & (1u << metric))) {
                        if (!append_text("null")) return false;
                        continue;
                    }

                    if (!append_format("%.3f",
                                       static_cast<double>(values[metric]) /
                                           1000.0)) {
                        return false;
                    }
                }

                phase_ = Phase::NightEnd;
                return true;
            }

            case Phase::NightEnd:
                phase_ = Phase::Nights;
                current_night_ = nullptr;
                current_stored_ = nullptr;
                return format_fragment("]}");

            case Phase::Recordings:
                if (recording_index_ < publication_->oximetry->size()) {
                    const auto &r = (*publication_->oximetry)[recording_index_++];
                    char ble[17]{};
                    const bool known_peer = oximetry_ble_source(r.peer, ble);
                    return format_fragment("%s{\"source\":\"%s\",\"device\":\"%s\",\"peer\":\"%s\",\"serial\":\"%s\",\"id\":\"%s\",\"start\":%lld,"
                        "\"end\":%lld,\"size\":%llu,\"csv_bytes\":%llu,\"step\":%u,\"count\":%u,\"modified\":%llu,\"day\":\"%s\","
                        "\"noon\":%lld,\"next_noon\":%lld,\"channels\":%u}",
                        recording_index_ == 1 ? "" : ",", r.source, r.device, known_peer ? ble : "", r.serial, r.id,
                        static_cast<long long>(r.start_ms), static_cast<long long>(r.report_end_ms()),
                        static_cast<unsigned long long>(r.size()),
                        static_cast<unsigned long long>(r.csv_bytes), r.interval_s * 1000, r.samples,
                        static_cast<unsigned long long>(r.modified), r.day,
                        static_cast<long long>(r.noon_s * 1000),
                        static_cast<long long>(r.next_noon_s * 1000),
                        r.planes ? unsigned(bool(r.planes->plane_bytes[0])) |
                            (unsigned(bool(r.planes->plane_bytes[1])) << 1) |
                            (unsigned(bool(r.planes->plane_bytes[2])) << 2) |
                            (unsigned(bool(r.planes->plane_bytes[3])) << 3) : 0);
                }
                phase_ = Phase::Done;
                return format_fragment("]}");

            case Phase::Done:
                return false;
        }
    }
}

size_t ReportCatalogJsonReader::read(uint8_t *buffer,
                                    size_t capacity,
                                    size_t offset) {
    if (failed_ || !buffer || capacity == 0 || offset != offset_) return 0;

    if (publication_->length() > 0) {
        if (offset_ >= publication_->length()) return 0;
        capacity = std::min(capacity, publication_->length() - offset_);
    }

    size_t copied = 0;
    while (copied < capacity) {
        if (fragment_offset_ >= fragment_length_) {
            fragment_length_ = 0;
            fragment_offset_ = 0;
            if (phase_ == Phase::Done || !prepare_fragment()) break;
        }

        const size_t available = fragment_length_ - fragment_offset_;
        if (available == 0) continue;

        const size_t count = std::min(available, capacity - copied);
        memcpy(buffer + copied, fragment_ + fragment_offset_, count);
        fragment_offset_ += count;
        copied += count;
    }

    offset_ += copied;
    return copied;
}

bool ReportCatalogJsonReader::complete() const {
    return !failed_ && phase_ == Phase::Done &&
        fragment_offset_ >= fragment_length_;
}

std::shared_ptr<const ReportCatalogJson> build_report_catalog_json(
    std::shared_ptr<const NightCatalog> catalog,
    std::shared_ptr<const ReportSignalStoreCatalog> store,
    uint32_t generation,
    std::shared_ptr<const ReportCatalogJson> previous,
    std::shared_ptr<const ReportOximetryCatalog> oximetry) try {
    if (!catalog) return {};

    char etag[64] = {};
    uint64_t oximetry_identity;
    if (!format_catalog_etag(*catalog, store.get(), oximetry.get(), generation, etag,
                             sizeof(etag), oximetry_identity)) {
        return {};
    }
    const bool same_etag = previous && strcmp(previous->etag, etag) == 0;
    if (same_etag && previous->catalog.get() == catalog.get() &&
        previous->store.get() == store.get() && previous->oximetry == oximetry) {
        return previous;
    }

    auto next = std::allocate_shared<ReportCatalogJson>(
        LargeAllocator<ReportCatalogJson>());
    next->catalog = std::move(catalog);
    next->store = std::move(store);
    next->oximetry = std::move(oximetry);
    next->oximetry_identity = oximetry_identity;
    memcpy(next->etag, etag, sizeof(etag));
    next->generation = generation;

    std::shared_ptr<const ReportCatalogJson> publication = next;
    if (same_etag) {
        next->content_length = previous->length();
        return publication;
    }

    ReportCatalogJsonReader counter(publication);
    uint8_t scratch[256] = {};
    size_t offset = 0;
    while (true) {
        const size_t copied = counter.read(scratch, sizeof(scratch), offset);
        if (copied == 0) break;
        if (copied > SIZE_MAX - offset) return {};
        offset += copied;
    }
    if (!counter.valid() || !counter.complete()) return {};

    next->content_length = offset;
    return publication;
} catch (const std::bad_alloc &) {
    return {};
}

}  // namespace aircannect
