#pragma once

#include <stddef.h>
#include <stdint.h>
#include <cstring>
#include <string_view>

#include "operation_outcome.h"
#include "oximetry_archive.h"
#include "sleep_day_id.h"

namespace aircannect {

// An empty source denotes the archive; an empty directory day denotes all
// recordings of the source (a clock observation can affect old ring files).
struct ReportOximetryChange {
    bool pending = false, ring = false;
    char source[33]{}, directory_day[9]{};
    bool file_published = false, csv_published = false, refresh_originals = true;

    static ReportOximetryChange published(bool ring, std::string_view source = {},
                                          std::string_view day = {}) {
        ReportOximetryChange out{true, ring};
        source.copy(out.source, sizeof(out.source)-1);
        day.copy(out.directory_day, sizeof(out.directory_day)-1);
        return out;
    }

    static ReportOximetryChange file(OximetryArchiveFile kind, std::string_view source,
                                     std::string_view day) {
        auto out = published(kind == OximetryArchiveFile::RingBin, source, day);
        out.file_published = true;
        out.csv_published = !out.ring;
        out.refresh_originals = kind != OximetryArchiveFile::RingCsv;
        return out;
    }

    void merge(const ReportOximetryChange &other) {
        if (!other.pending) return;
        if (!pending) { *this = other; return; }

        ring |= other.ring;
        file_published |= other.file_published;
        csv_published |= other.csv_published;
        refresh_originals |= other.refresh_originals;
        if (strcmp(source, other.source)) source[0] = 0;
        if (!source[0] || strcmp(directory_day, other.directory_day)) directory_day[0] = 0;
    }
};

struct RemovedStorageFiles {
    enum Owner : uint8_t {
        NightSources = 1, NightStore = 2, SummaryJournal = 4, Firmware = 8
    };
    uint8_t owners = 0;
    SleepDayId first_source, last_source;

    void merge(const RemovedStorageFiles &other) {
        owners |= other.owners;
        if (!other.first_source.valid()) return;
        if (!first_source.valid() || other.first_source < first_source) first_source = other.first_source;
        if (!last_source.valid() || last_source < other.last_source) last_source = other.last_source;
    }
};

// ReportTask stamps the revision of a durable source publication on admission;
// producer generations are not comparable across producers.
struct ReportSourceChange {
    SleepDayId first_day;
    SleepDayId last_day;
    uint32_t revision = 0;
    ReportOximetryChange oximetry{};
    RemovedStorageFiles removals{};

    bool valid() const {
        return first_day.valid() && last_day.valid() &&
               !(last_day < first_day);
    }

    bool overlaps_or_touches(const ReportSourceChange &other) const {
        if (!valid() || !other.valid()) return false;
        const int64_t left_end = static_cast<int64_t>(last_day.epoch_days());
        const int64_t right_start =
            static_cast<int64_t>(other.first_day.epoch_days());
        const int64_t other_end =
            static_cast<int64_t>(other.last_day.epoch_days());
        const int64_t this_start =
            static_cast<int64_t>(first_day.epoch_days());
        return left_end + 1 >= right_start && other_end + 1 >= this_start;
    }

    void merge(const ReportSourceChange &other) {
        if (!other.valid()) return;
        if (!valid()) {
            *this = other;
            return;
        }
        if (other.first_day < first_day) first_day = other.first_day;
        if (last_day < other.last_day) last_day = other.last_day;
        if (revision < other.revision) revision = other.revision;
    }
};

static constexpr size_t AC_REPORT_SOURCE_CHANGE_CAPACITY = 8;

inline void merge_report_source_change(ReportSourceChange *entries,
                                       size_t &count,
                                       const ReportSourceChange &change) {
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].overlaps_or_touches(change)) continue;

        entries[i].merge(change);
        return;
    }

    if (count < AC_REPORT_SOURCE_CHANGE_CAPACITY) {
        entries[count++] = change;
        return;
    }

    ReportSourceChange merged = change;
    for (size_t i = 0; i < count; ++i) merged.merge(entries[i]);
    entries[0] = merged;
    count = 1;
}

using ReportSourceChangeCallback = OperationAdmission (*) (
    void *context, const ReportSourceChange &change);

}  // namespace aircannect
