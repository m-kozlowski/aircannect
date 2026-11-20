#include "sleephq_oximetry.h"

#include <algorithm>
#include <cstring>
#include <strings.h>

#include "oximetry_archive.h"

namespace aircannect {
namespace {

bool same_device(const ReportOximetryRecording &a, const ReportOximetryRecording &b) {
    return !strcmp(a.source, b.source) ||
        (a.serial[0] && !strcmp(a.serial, b.serial)) ||
        (a.peer[0] && !strcasecmp(a.peer, b.peer));
}

}  // namespace

const char *SleepHqOximetryPlan::source_id(const ReportOximetryRecording &r,
                                           const ReportOximetryCatalog &catalog, char (&ble)[17]) {
    if (oximetry_ble_source(r.peer, ble)) return ble;
    if (r.serial[0]) {
        for (const auto &other : catalog) {
            if (!strcmp(r.serial, other.serial) && oximetry_ble_source(other.peer, ble)) return ble;
        }
    }
    return r.source;
}

void SleepHqOximetryPlan::begin(Catalog catalog, std::string_view preference) {
    catalog_ = std::move(catalog);
    decltype(files_)().swap(files_);
    cursor_ = 0;
    collecting_ = true;
    deadline_ = 0;
    if (!catalog_ || preference.empty()) return;

    for (const auto &r : *catalog_) {
        if (r.start_ms < r.noon_s * 1000 || (r.csv_bytes && !r.export_clock_known)) continue;
        char ble[17];
        if (preference != "auto" && preference != r.source && preference != r.serial &&
            preference != source_id(r, *catalog_, ble)) continue;
        files_.push_back({&r});
    }
}

const char *SleepHqOximetryPlan::next_source() {
    while (cursor_ < files_.size()) {
        const char *source = files_[cursor_].record->source;
        bool needed = collecting_;
        do {
            const auto &f = files_[cursor_++];
            needed |= f.published && !f.confirmed;
        } while (cursor_ < files_.size() && !strcmp(source, files_[cursor_].record->source));
        if (needed) return source;
    }
    return nullptr;
}

void SleepHqOximetryPlan::start_uploads() {
    collecting_ = false;
    cursor_ = 0;
}

SleepHqOximetryPlan::File *SleepHqOximetryPlan::find(const char *path) {
    std::string_view source, suffix, id;
    if (!oximetry_archive_path(path, source, suffix)) return nullptr;
    const auto kind = oximetry_recording_file(source, suffix, id);
    if (kind != OximetryArchiveFile::LiveCsv && kind != OximetryArchiveFile::RingCsv) return nullptr;

    const auto key = std::make_pair(source, id);
    auto it = std::lower_bound(files_.begin(), files_.end(), key,
        [](const File &f, const auto &k) { return f.record->key() < k; });
    return it != files_.end() && it->record->key() == key ? &*it : nullptr;
}

bool SleepHqOximetryPlan::inspect(const char *path, bool confirmed) {
    auto *file = find(path);
    if (!file) return false;
    file->published = true;
    file->confirmed |= confirmed;
    return true;
}

const ReportOximetryRecording *SleepHqOximetryPlan::ring_csv(const char *path) {
    auto *file = find(path);
    return file && !file->record->csv_bytes ? file->record : nullptr;
}

bool SleepHqOximetryPlan::eligible(const char *path, int64_t now, MemoryEnabled downloads) {
    auto *file = find(path);
    if (!file || !file->published || file->confirmed) return false;
    const auto &r = *file->record;

    for (const auto &other : files_) {
        const auto &s = *other.record;
        if (&other == file || !other.published ||
            r.start_ms >= s.end_ms() || s.start_ms >= r.end_ms()) continue;

        if (same_device(r, s) && (other.confirmed || (r.csv_bytes && !s.csv_bytes))) return false;
    }

    if (r.csv_bytes && now < r.next_noon_s && r.peer[0] &&
        downloads && downloads(r.peer)) {
        if (!deadline_ || r.next_noon_s < deadline_) deadline_ = r.next_noon_s;
        return false;
    }
    return true;
}

}  // namespace aircannect
