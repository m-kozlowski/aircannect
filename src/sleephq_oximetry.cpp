#include "sleephq_oximetry.h"

#include <algorithm>
#include <cstring>

#include "oximetry_archive.h"

namespace aircannect {
namespace {

std::string_view directory_day(const ReportOximetryRecording &r) {
    return {r.csv_bytes ? r.id + 5 : r.id, 8};
}

bool overlaps(const ReportOximetryRecording &a, const ReportOximetryRecording &b) {
    return a.start_ms < b.end_ms() && b.start_ms < a.end_ms();
}

}  // namespace

void SleepHqOximetryPlan::begin(Catalog catalog, std::string_view preference, std::string_view day) {
    catalog_ = std::move(catalog);
    decltype(files_)().swap(files_);
    cursor_ = 0;
    collecting_ = true;
    scoped_ = !day.empty();
    inventory_day_[0] = 0;
    deadline_ = 0;
    if (!catalog_ || preference.empty()) return;

    // Older configurations may name an archive directory or a serial.
    // Resolve that preference once, so it includes both kinds of recording.
    if (preference != "auto") {
        std::string_view selected;
        bool ambiguous = false;
        for (const auto &r : *catalog_) {
            if (preference == r.device) {
                selected = r.device;
                ambiguous = false;
                break;
            }
            if (preference != r.source && preference != r.serial) continue;
            ambiguous |= !selected.empty() && selected != r.device;
            selected = r.device;
        }
        if (ambiguous || selected.empty()) return;
        preference = selected;
    }

    for (const auto &r : *catalog_) {
        if (r.start_ms < r.noon_s * 1000 || (r.csv_bytes && !r.export_clock_known)) continue;
        if (preference != "auto" && preference != r.device) continue;
        files_.push_back({&r});
    }

    if (scoped_) {
        // The ring's directory date can differ from live's, even for the
        // same interval. Read its receipts too before choosing a recording.
        decltype(files_) changed;
        for (const auto &f : files_) {
            if (directory_day(*f.record) == day) changed.push_back(f);
        }
        files_.erase(std::remove_if(files_.begin(), files_.end(), [&](const File &f) {
            if (directory_day(*f.record) == day) return false;
            return std::none_of(changed.begin(), changed.end(), [&](const File &c) {
                return !strcmp(c.record->device, f.record->device) && overlaps(*c.record, *f.record);
            });
        }), files_.end());
    }
}

const char *SleepHqOximetryPlan::next_source() {
    while (cursor_ < files_.size()) {
        const char *source = files_[cursor_].record->source;
        const auto day = directory_day(*files_[cursor_].record);
        if (scoped_) day.copy(inventory_day_, 8);
        bool needed = collecting_;
        do {
            const auto &f = files_[cursor_++];
            needed |= f.published && !f.confirmed;
        } while (cursor_ < files_.size() && !strcmp(source, files_[cursor_].record->source) &&
                 (!scoped_ || day == directory_day(*files_[cursor_].record)));
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
    const bool live = r.csv_bytes != 0;
    bool counterpart_ready = false;

    for (const auto &other : files_) {
        const auto &s = *other.record;
        if (&other == file || !other.published || strcmp(r.device, s.device) || !overlaps(r, s)) continue;
        if (other.confirmed) return false;
        if (live == (s.csv_bytes != 0)) continue;

        // Ring live samples are nominally 1 Hz; CSV timestamps have no fixed interval in the catalog.
        counterpart_ready = true;
        const auto &memory = live ? s : r;
        const bool prefer_live = memory.interval_s > 1;
        if (live != prefer_live) return false;
    }

    if (!counterpart_ready && now < r.next_noon_s &&
        (r.peer[0] ? downloads && downloads(r.peer) : !live)) {
        if (!deadline_ || r.next_noon_s < deadline_) deadline_ = r.next_noon_s;
        return false;
    }
    return true;
}

}  // namespace aircannect
