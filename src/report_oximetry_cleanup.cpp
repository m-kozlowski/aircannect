#include "report_oximetry_cleanup.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include "report_signal_store.h"
#include "storage_space_policy.h"

namespace aircannect {
namespace {
using Visit = StorageScanCommand::Visit;
using View = std::string_view;

Visit cache_directory(size_t depth, const char *name, bool directory) {
    if (!directory) return Visit::Skip;
    const View s(name);
    if (depth <= 3)
        return s.size() == (depth == 1 ? 4u : 2u) && s.find_first_not_of("0123456789") == s.npos
            ? Visit::Descend : Visit::Skip;
    if (depth == 4) return s == "ox" ? Visit::Descend : Visit::Skip;
    if (depth < 7) return Visit::Descend;
    return depth == 7 ? Visit::Collect : Visit::Skip;
}

// Match the five decimal components emitted by report_oximetry_signal_path.
bool version_size(View name, uint64_t &size) {
    const char *p = name.data(), *end = p + name.size();
    for (int i = 0; i < 5; ++i) {
        uint64_t n = 0;
        const auto result = std::from_chars(p, end, n);
        if (result.ec != std::errc{}) return false;
        if (i == 1) size = n;
        p = result.ptr;
        if (i < 4 && (p == end || *p++ != '-')) return false;
    }
    return p == end;
}

bool known_plane(View name) {
    if (name == "manifest" || name == "index" || name == "raw" || name == "histograms") return true;
    if (name.size() > 8 && name.substr(name.size()-8) == ".deflate") name.remove_suffix(8);
    for (const auto *channel : ReportOximetryChannels) if (name == channel) return true;
    return false;
}

// Appending a new recording does not retire an earlier version.
bool retires_versions(const ReportOximetryCatalog &previous, const ReportOximetryCatalog &next) {
    auto it = next.begin();
    for (const auto &r : previous) {
        while (it != next.end() && it->key() < r.key()) ++it;
        if (it == next.end() || it->key() != r.key() || it->start_ms != r.start_ms ||
            it->modified != r.modified || it->size() != r.size() || it->noon_s != r.noon_s ||
            it->next_noon_s != r.next_noon_s) return true;
        ++it;
    }
    return false;
}
}  // namespace

void ReportOximetryCleanup::begin(ReportOximetryArchive &archive, StorageScanPort &scans,
                                  StorageReadPort &reads, StorageRangeWritePort &ranges,
                                  StorageAtomicWritePort &writes, StorageDeletePort &deletes,
                                  StoragePathPort &paths) {
    archive_ = &archive;
    scans_ = &scans;
    deletes_ = &deletes;
    paths_ = &paths;
    verifier_.begin(reads, ranges, writes);
    manifest_reader_.begin(reads);
}

void ReportOximetryCleanup::next() {
    verifier_.reset();
    manifest_reader_.reset();
    directory_.clear();
    contents_.reset();
    original_ = {};
    current_ = manifest_ = false;
    phase_ = Phase::Select;
    ++cursor_;
}

bool ReportOximetryCleanup::cancel() {
    if (remove_directory_.valid()) {
        StoragePathCompletion done;
        if (!paths_->take_completion(remove_directory_, done)) return false;
        remove_directory_ = {};
    }
    if (delete_id_) {
        deletes_->cancel(delete_id_);
        StorageDeleteStatus s;
        if (!deletes_->status(s, 0) || (s.id == delete_id_ && s.state == StorageDeleteState::Deleting))
            return false;
        delete_id_ = 0;
    }
    if (scan_.valid() && !scans_->abandon(scan_)) return false;
    scan_ = {};
    entries_.reset();
    next();
    cursor_ = 0;
    return true;
}

void ReportOximetryCleanup::forget_media() {
    originals_.reset();
    copies_only_ = pressure_ = due_ = false;
}

bool ReportOximetryCleanup::poll(bool allowed, bool pressure) try {
    if (!archive_) return false;
    const auto originals = archive_->originals();
    if (!allowed || !originals || originals != originals_ || pressure != pressure_) {
        if (active()) {
            due_ = true;
            if (!cancel()) return false;
        }
        if (pressure != pressure_ || (pressure && originals != originals_)) {
            copies_only_ = pressure;
            due_ = true;
        }
        pressure_ = pressure;
        if (!allowed || !originals) return false;
        if (originals != originals_) {
            due_ |= !originals_ || retires_versions(*originals_, *originals);
            originals_ = originals;
        }
    }
    if (originals->empty() || (!due_ && !active())) return false;

    if (delete_id_) {
        StorageDeleteStatus status;
        if (!deletes_->status(status, 0) ||
            (status.id == delete_id_ && status.state == StorageDeleteState::Deleting)) return false;
        const bool done = status.id == delete_id_ && status.state == StorageDeleteState::Done;
        delete_id_ = 0;
        if (done && !copies_only_) phase_ = Phase::EmptyDirectory;
        else next();
        return true;
    }

    if (!entries_ || phase_ == Phase::Contents) {
        const bool inventory = !entries_;
        if (!scan_.valid()) {
            const StorageScanRoot root{inventory ? REPORT_SIGNAL_STORE_ROOT : directory_.c_str(), inventory};
            const auto s = scans_->request_scan({&root, 1, true, 1, inventory ? cache_directory : nullptr});
            if (s.admission == OperationAdmission::Busy) return false;
            if (!s.accepted()) {
                if (inventory) due_ = false;
                else next();
            } else scan_ = s.ticket;
            return true;
        }
        StorageScanCompletion done;
        if (!scans_->take_completion(scan_, done)) return false;
        scan_ = {};
        if (done.outcome.disposition != OperationDisposition::Succeeded || !done.snapshot) {
            if (inventory) due_ = false;
            else next();
            return true;
        }
        if (inventory) {
            entries_ = std::move(done.snapshot);
            cursor_ = 0;
            due_ = false;
        } else {
            for (size_t i = 0; i < done.snapshot->size(); ++i) {
                StorageScanEntryView e;
                if (!done.snapshot->entry(i, e)) { next(); return true; }
                if (directory_ == e.path) continue;
                const View name(storage_basename_from_path(e.path));
                if (e.directory || !known_plane(name)) { next(); return true; }
                manifest_ |= name == "manifest";
            }
            contents_ = std::move(done.snapshot);
            if (copies_only_) {
                if (!manifest_ || manifest_reader_.start((directory_ + "/manifest").c_str(),
                    ReportOximetryPlanes::PublicationBytes, 1, StorageReadLane::Maintenance) !=
                    OperationAdmission::Accepted) next();
                else phase_ = Phase::Manifest;
            } else phase_ = Phase::Verify;
        }
        return true;
    }

    if (phase_ == Phase::Select) {
        if (cursor_ == entries_->size()) {
            if (copies_only_) {
                copies_only_ = false;
                cursor_ = 0;
            } else entries_.reset();
            return true;
        }
        StorageScanEntryView e;
        if (!entries_->entry(cursor_, e) || !e.directory) { next(); return true; }
        const View path(e.path);
        const auto version = path.rfind('/');
        uint64_t bytes = 0;
        if (version == path.npos || !version_size(path.substr(version + 1), bytes)) { next(); return true; }

        const auto id = path.rfind('/', version - 1);
        const auto source = path.rfind('/', id - 1);
        if (source < 3 || source == path.npos || path.substr(source - 3, 3) != "/ox") { next(); return true; }
        const auto key = std::make_pair(path.substr(source + 1, id - source - 1),
                                        path.substr(id + 1, version - id - 1));
        auto it = std::lower_bound(originals->begin(), originals->end(), key,
            [](const auto &r, const auto &k) { return r.key() < k; });
        if (it == originals->end() || it->key() != key || it->size() != bytes) { next(); return true; }

        directory_ = path;
        original_ = *it;
        for (; it != originals->end() && it->key() == key; ++it) {
            if (report_oximetry_signal_path(*it, "") != directory_ + "/") continue;
            original_ = *it;
            current_ = true;
            break;
        }
        if (current_ && !pressure) next();
        else phase_ = Phase::Contents;
        return true;
    }

    if (phase_ == Phase::Manifest) {
        if (manifest_reader_.status().active()) return manifest_reader_.poll();
        const auto bytes = manifest_reader_.take_completed();
        if (!bytes || !planes_.decode(bytes->data(), bytes->size())) next();
        else phase_ = Phase::Delete;
        return true;
    }

    if (phase_ == Phase::Verify) {
        const bool worked = verifier_.step(original_, ReportOximetrySignalBuilder::Mode::VerifyOriginal);
        if (!verifier_.finished()) return worked;
        if (!verifier_.verified()) { next(); return true; }
        verifier_.reset();
        if (current_) archive_->withdraw(original_);
        phase_ = Phase::Delete;
        return true;
    }

    if (phase_ == Phase::EmptyDirectory) {
        if (!remove_directory_.valid()) {
            const auto s = paths_->request({StoragePathOperation::RemoveEmptyDirectory, directory_, {}, 1});
            if (s.admission == OperationAdmission::Busy) return false;
            if (s.accepted()) remove_directory_ = s.ticket;
            else next();
        } else {
            StoragePathCompletion done;
            if (!paths_->take_completion(remove_directory_, done)) return false;
            remove_directory_ = {};
            next();
        }
        return true;
    }

    // Delete only observed files. Removing the manifest first makes a
    // partially deleted version unavailable even if power fails.
    const char *names[12];
    size_t count = 0;
    if (manifest_ && !copies_only_) names[count++] = "manifest";
    unsigned plain = 0;
    const char *copies[4]{};
    for (size_t i = 0; i < contents_->size(); ++i) {
        StorageScanEntryView e;
        if (!contents_->entry(i, e)) { next(); return true; }
        if (e.directory) continue;
        const char *name = storage_basename_from_path(e.path);
        if (copies_only_) {
            const View filename(name);
            for (size_t c = 0; c < 4; ++c) {
                const View channel(ReportOximetryChannels[c]);
                if (filename == channel && e.size && e.size == planes_.plane_bytes[c]) plain |= 1u << c;
                if (filename.size() == channel.size() + 8 && filename.substr(0, channel.size()) == channel &&
                    filename.substr(channel.size()) == ".deflate" && e.size) copies[c] = name;
            }
            continue;
        }
        if (!strcmp(name, "manifest")) continue;
        if (count == sizeof(names)/sizeof(*names)) { next(); return true; }
        names[count++] = name;
    }
    if (copies_only_) {
        for (size_t c = 0; c < 4; ++c) {
            if ((plain & (1u << c)) && copies[c]) names[count++] = copies[c];
        }
        if (!count) { next(); return true; }
    } else if (!count) { phase_ = Phase::EmptyDirectory; return true; }
    char error[AC_STORAGE_ERROR_MAX]{};
    if (!deletes_->start_selected(directory_.c_str(), names, count, &delete_id_, error, sizeof(error),
                                  copies_only_ ? StorageSpacePolicy::ReclaimTarget : 0)) {
        if (!*error || strstr(error, "busy") || strstr(error, "unavailable")) return false;
        next();
    }
    return true;
} catch (const std::bad_alloc &) {
    if (cancel()) due_ = false;
    return false;
}

}  // namespace aircannect
