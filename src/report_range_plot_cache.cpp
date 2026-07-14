#include "report_range_plot_cache.h"

#include <stdio.h>
#include <string.h>

namespace aircannect {

ReportRangePlotRead ReportRangePlotCache::read_or_request(
    size_t index,
    uint64_t night_start_ms,
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    std::shared_ptr<ReportSpoolBuffer> &out) {
    if (!etag || !etag[0]) return ReportRangePlotRead::Error;

    for (Entry &entry : entries_) {
        if (!entry.valid || entry.night_start_ms != night_start_ms ||
            strcmp(entry.etag, etag) != 0 || entry.from_ms != from_ms ||
            entry.to_ms != to_ms) {
            continue;
        }
        if (!entry.bytes) {
            clear_entry(entry);
            continue;
        }

        entry.last_used = ++tick_;
        out = entry.bytes;
        return entry.empty ? ReportRangePlotRead::Empty
                           : ReportRangePlotRead::Ready;
    }

    if (!request_matches(index,
                         night_start_ms,
                         etag,
                         from_ms,
                         to_ms)) {
        begin_request(index, night_start_ms, etag, from_ms, to_ms);
    }

    return ReportRangePlotRead::Building;
}

bool ReportRangePlotCache::request_snapshot(ReportRangePlotRequest &out) const {
    out = request_;
    return out.active;
}

bool ReportRangePlotCache::finish_request(
    size_t index,
    uint64_t night_start_ms,
    const char *etag,
    int64_t from_ms,
    int64_t to_ms,
    const std::shared_ptr<ReportSpoolBuffer> &plot,
    bool empty) {
    if (!plot || !request_matches(index,
                                  night_start_ms,
                                  etag,
                                  from_ms,
                                  to_ms)) {
        return false;
    }

    if (plot->size() > AC_REPORT_RANGE_CACHE_MAX_BYTES) {
        clear_request();
        return false;
    }

    size_t pick = 0;
    bool have_pick = false;
    for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
        Entry &entry = entries_[i];
        if (entry.valid && entry.night_start_ms == night_start_ms &&
            strcmp(entry.etag, etag) == 0 && entry.from_ms == from_ms &&
            entry.to_ms == to_ms) {
            pick = i;
            have_pick = true;
            break;
        }
    }

    if (!have_pick) {
        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            if (entries_[i].valid) continue;

            pick = i;
            have_pick = true;
            break;
        }
    }

    if (!have_pick) {
        for (size_t i = 1; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            if (entries_[i].last_used < entries_[pick].last_used) pick = i;
        }
    }

    clear_entry(entries_[pick]);
    trim_for(plot->size(), pick);

    Entry &entry = entries_[pick];
    entry.valid = true;
    entry.empty = empty;
    entry.night_start_ms = night_start_ms;
    snprintf(entry.etag, sizeof(entry.etag), "%s", etag);
    entry.from_ms = from_ms;
    entry.to_ms = to_ms;
    entry.last_used = ++tick_;
    entry.bytes = plot;
    clear_request();
    return true;
}

void ReportRangePlotCache::fail_request(size_t index,
                                        uint64_t night_start_ms,
                                        const char *etag,
                                        int64_t from_ms,
                                        int64_t to_ms) {
    if (request_matches(index,
                        night_start_ms,
                        etag,
                        from_ms,
                        to_ms)) {
        clear_request();
    }
}

void ReportRangePlotCache::reset(bool clear_ready) {
    clear_request();
    if (!clear_ready) return;

    for (Entry &entry : entries_) clear_entry(entry);
}

void ReportRangePlotCache::invalidate(uint64_t night_start_ms, bool all) {
    if (request_.active &&
        (all || request_.night_start_ms == night_start_ms)) {
        clear_request();
    }

    for (Entry &entry : entries_) {
        if (entry.valid && (all || entry.night_start_ms == night_start_ms)) {
            clear_entry(entry);
        }
    }
}

bool ReportRangePlotCache::request_matches(size_t index,
                                           uint64_t night_start_ms,
                                           const char *etag,
                                           int64_t from_ms,
                                           int64_t to_ms) const {
    return request_.active && request_.index == index &&
           request_.night_start_ms == night_start_ms && etag &&
           strcmp(request_.etag, etag) == 0 && request_.from_ms == from_ms &&
           request_.to_ms == to_ms;
}

void ReportRangePlotCache::begin_request(size_t index,
                                         uint64_t night_start_ms,
                                         const char *etag,
                                         int64_t from_ms,
                                         int64_t to_ms) {
    request_.active = true;
    request_.index = index;
    request_.night_start_ms = night_start_ms;
    snprintf(request_.etag, sizeof(request_.etag), "%s", etag);
    request_.from_ms = from_ms;
    request_.to_ms = to_ms;
}

void ReportRangePlotCache::clear_request() {
    request_ = {};
}

void ReportRangePlotCache::clear_entry(Entry &entry) {
    entry = {};
}

void ReportRangePlotCache::trim_for(size_t incoming_bytes,
                                    size_t protected_index) {
    size_t retained_bytes = 0;
    for (const Entry &entry : entries_) {
        if (entry.valid && entry.bytes) retained_bytes += entry.bytes->size();
    }

    while (retained_bytes > AC_REPORT_RANGE_CACHE_MAX_BYTES ||
           incoming_bytes > AC_REPORT_RANGE_CACHE_MAX_BYTES - retained_bytes) {
        size_t victim = AC_REPORT_RANGE_CACHE_SLOT_MAX;
        for (size_t i = 0; i < AC_REPORT_RANGE_CACHE_SLOT_MAX; ++i) {
            const Entry &entry = entries_[i];
            if (i == protected_index || !entry.valid) continue;
            if (victim == AC_REPORT_RANGE_CACHE_SLOT_MAX ||
                entry.last_used < entries_[victim].last_used) {
                victim = i;
            }
        }
        if (victim == AC_REPORT_RANGE_CACHE_SLOT_MAX) break;

        if (entries_[victim].bytes) {
            retained_bytes -= entries_[victim].bytes->size();
        }
        clear_entry(entries_[victim]);
    }
}

}  // namespace aircannect
