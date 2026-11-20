#include "report_oximetry.h"
#include "report_oximetry_signals.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include <functional>
#include "calendar_utils.h"
#include "little_endian.h"
#include "large_json_allocator.h"
#include "edf_time.h"
#include "night_catalog.h"
#include "oximetry_archive.h"
#include "utc_time.h"

namespace aircannect {
namespace {

// All paths in a snapshot come from the storage owner. Only accept the
// importer's final layout: temporary files and unrelated USB files are not inputs.
bool archive_name(const char *path, bool clock, std::string_view &source, std::string_view &id) {
    std::string_view suffix;
    if (!oximetry_archive_path(path, source, suffix)) return false;
    if (clock) return suffix.size() > 16 && suffix.substr(0, 6) == "clock/" &&
                      suffix.substr(suffix.size() - 5) == ".json";
    const auto kind = oximetry_recording_file(source, suffix, id);
    return kind == OximetryArchiveFile::LiveCsv || kind == OximetryArchiveFile::RingBin;
}

using LittleEndian::get_le32;
constexpr uint8_t SHeader[] = {1, 3, 0, 0, 0, 0, 0, 0, 4, 0};
static_assert(sizeof(SHeader) == RingBinLayout::SHeader);

bool ring_start(time_t wall, std::string_view id, ReportOximetryRecording &out) {
    struct tm local{};
    char stamp[16];
    if (!gmtime_r(&wall, &local) || local.tm_year < 100 || local.tm_year > 199 ||
        strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &local) != 14 || id != stamp)
        return false;

    local.tm_isdst = -1;
    out.wall_s = wall;
    out.local_start_s = mktime(&local);
    out.start_ms = out.local_start_s * 1000;
    return out.local_start_s >= 0;
}

bool decode_original_header(const uint8_t *p, size_t length, uint64_t size, std::string_view id,
                            ReportOximetryRecording &out) {
    const auto header = out.bin.offset(0);
    if (length != header || p[0] != 3 || p[1] != 0 || size <= header ||
        size != get_le32(p + 9) || (size - header) % out.bin.stride()) return false;

    const uint32_t samples = (size - header) / out.bin.stride();
    const auto duration = get_le32(p + 13);
    if (duration != samples * 2ULL && duration != samples * 4ULL) return false;

    const int year = p[2] + 256 * p[3];
    if (year < 2000 || year > 2099 || p[4] < 1 || p[4] > 12 || p[5] < 1 ||
        p[5] > calendar_days_in_month(year, p[4]) || p[6] > 23 || p[7] > 59 || p[8] > 59)
        return false;

    out.samples = samples;
    out.interval_s = duration / samples;
    const auto wall = calendar_days_from_civil(year, p[4], p[5]) * 86400 +
                      p[6] * 3600 + p[7] * 60 + p[8];
    return ring_start(wall, id, out);
}

// The writer puts the first UTC millisecond in the name. Only the last
// complete row is needed to locate the other end, even for a full night.
bool live_bounds(std::string_view id, const uint8_t *tail, size_t length,
                 ReportOximetryRecording &out) {
    std::string_view row(reinterpret_cast<const char *>(tail), length);
    if (length < 2 || row.substr(length - 2) != "\r\n") return false;
    row.remove_suffix(2);
    const auto newline = row.rfind('\n');
    if (newline != row.npos) row.remove_prefix(newline + 1);
    const auto pi = row.rfind(',');
    if (pi == row.npos) return false;
    row = row.substr(0, pi);
    const auto utc = row.rfind(',');
    if (utc == row.npos || row.size() - utc - 1 != 24) return false;

    char timestamp[25]{};
    memcpy(timestamp, row.data() + utc + 1, 24);
    int64_t last;
    if (!parse_utc_iso8601_ms(timestamp, last) ||
        std::from_chars(id.data() + 14, id.data() + 27, out.start_ms).ec != std::errc{} ||
        last < out.start_ms || last - out.start_ms > 26LL * 3600000) return false;
    out.csv_end_ms = last + 1; // Exclusive bound, not a fabricated sample duration.
    return true;
}

bool night_window(time_t sample, ReportOximetryRecording &out) {
    struct tm local{};
    if (!localtime_r(&sample, &local)) return false;
    if (local.tm_hour < 12) --local.tm_mday;
    local.tm_hour = 12;
    local.tm_min = local.tm_sec = 0;
    local.tm_isdst = -1;
    out.noon_s = mktime(&local);
    if (!strftime(out.day, sizeof(out.day), "%Y%m%d", &local)) return false;
    ++local.tm_mday;
    local.tm_isdst = -1;
    out.next_noon_s = mktime(&local);
    return out.noon_s <= sample && sample < out.next_noon_s;
}

int32_t ring_offset(int64_t start_ms, const NightCatalog *nights) {
    const int64_t utc_day = start_ms / 86400000;
    for (int delta = -2; nights && delta <= 1; ++delta) {
        SleepDayId day;
        if (!SleepDayId::from_epoch_days(utc_day + delta, day)) continue;
        const auto *record = nights->find(day);
        if (record && record->timezone_offset_valid && record->day_start_ms <= start_ms &&
            start_ms < record->day_end_ms) return record->timezone_offset_minutes;
    }
    int32_t offset = 0;
    edf_configured_timezone_offset_minutes(start_ms, offset);
    return offset;
}

}  // namespace

std::string ReportOximetryRecording::path() const {
    return std::string(OximetryArchiveRoot) + "/" + source + "/" +
           std::string(csv_bytes ? id + 5 : id, 8) + "/" + id + (csv_bytes ? ".csv" : ".bin");
}

void ReportOximetryArchive::begin(StorageScanPort &scan, StorageReadPort &read) {
    scan_ = &scan;
    read_ = &read;
}

void ReportOximetryArchive::release() {
    if (scan_ticket_.valid()) scan_->abandon(scan_ticket_);
    if (read_ticket_.valid()) read_->abandon(read_ticket_);
    if (prepared_.valid()) read_->release_prepared(prepared_);
    scan_ticket_ = {};
    read_ticket_ = {};
    prepared_ = {};
    files_.reset();
    building_.reset();
    index_ = 0;
    part_ = FilePart::Prefix;
    clocks_ = false;
}

void ReportOximetryArchive::reset() {
    release();
    if (signals_) signals_->reset();
    preparing_.reset();
    ring_sources_.reset();
    originals_.reset();
    prepare_pending_ = offsets_dirty_ = false;
    signal_retry_at_ = 0;
    published_.reset();
    retry_at_ = 0;
    scanning_ = {};
    pending_ = {true, true};
}

bool ReportOximetryArchive::poll(uint32_t now_ms, const NightCatalog *nights) try {
    return step(now_ms, nights);
} catch (const std::bad_alloc &) {
    release();
    retry_at_ = now_ms + 60000;
    return false;
}

bool ReportOximetryArchive::step(uint32_t now_ms, const NightCatalog *nights) {
    if (offsets_dirty_ && ring_sources_) {
        std::shared_ptr<ReportOximetryCatalog> revised;
        for (size_t i = 0; i < ring_sources_->size(); ++i) {
            const auto &r = (*ring_sources_)[i];
            if (r.csv_bytes) continue;
            const auto offset = ring_offset(r.start_ms, nights);
            if (r.offset_minutes == offset) continue;
            if (!revised) revised = std::allocate_shared<ReportOximetryCatalog>(
                LargeAllocator<ReportOximetryCatalog>(), *ring_sources_);
            (*revised)[i].offset_minutes = offset;
        }
        if (revised) ring_sources_ = std::move(revised);
    }
    offsets_dirty_ = false;
    if (!files_) {
        if (!scan_ticket_.valid()) {
            if (!scanning_.pending && pending_.pending) {
                scanning_ = pending_;
                pending_ = {};
                if (++generation_ == 0) ++generation_;
            }
            if (!scanning_.pending || (retry_at_ && int32_t(now_ms - retry_at_) < 0))
                return false;
            std::string dir = OximetryArchiveRoot;
            if (scanning_.source[0]) dir += std::string("/") + scanning_.source;
            if (scanning_.directory_day[0]) dir += std::string("/") + scanning_.directory_day;
            const StorageScanRoot root{dir.c_str(), true};
            const auto submitted = scan_->request_scan({&root, 1, false, generation_});
            scan_ticket_ = submitted.ticket;
            return submitted.accepted();
        }
        StorageScanCompletion result;
        if (!scan_->take_completion(scan_ticket_, result)) return false;
        scan_ticket_ = {};
        if (!result.snapshot || result.outcome.disposition != OperationDisposition::Succeeded) {
            retry_at_ = now_ms + 60000;
            return false;
        }
        files_ = std::move(result.snapshot);
        ring_changed_ = scanning_.ring;
        building_ = std::allocate_shared<ReportOximetryCatalog>(LargeAllocator<ReportOximetryCatalog>());
        if (scanning_.source[0] && published_) {
            for (const auto &r : *published_) {
                const char *directory_day = r.csv_bytes ? r.id + 5 : r.id;
                const bool in_scope = !strcmp(scanning_.source, r.source) &&
                    (!scanning_.directory_day[0] || !strncmp(scanning_.directory_day, directory_day, 8));
                if (in_scope || r.start_ms < r.noon_s * 1000) continue;
                building_->push_back(r);
            }
        }
        return true;
    }

    StorageScanEntryView entry;
    if (!files_->entry(index_, entry)) {
        if (!clocks_ && ring_changed_ && !building_->empty()) {
            clocks_ = true;
            index_ = 0;
        } else {
            const size_t originals = building_->size();
            for (size_t i = 0; i < originals; ++i) {
                auto slice = (*building_)[i];
                if (!night_window(slice.start_ms / 1000, slice)) continue;
                if (slice.noon_s != (*building_)[i].noon_s ||
                    slice.next_noon_s != (*building_)[i].next_noon_s) slice.planes.reset();
                if (!slice.csv_bytes) slice.offset_minutes = ring_offset(slice.start_ms, nights);
                (*building_)[i] = slice;
                while (slice.next_noon_s * 1000 < slice.report_end_ms() && night_window(slice.next_noon_s, slice)) {
                    slice.planes.reset();
                    building_->push_back(slice);
                }
            }
            std::sort(building_->begin(), building_->end(), [](const auto &a, const auto &b) {
                return a.key() == b.key() ? a.noon_s < b.noon_s : a.key() < b.key();
            });
            // A scoped scan may reread ring clocks. Keep ready tracks when the
            // immutable source and its night window still identify the same files.
            if (published_) {
                auto old = published_->begin();
                for (auto &r : *building_) {
                    r.invalid_tracks = false;
                    r.last_track_warning = 0;
                    while (old != published_->end() && (old->key() < r.key() ||
                           (old->key() == r.key() && old->noon_s < r.noon_s))) ++old;
                    if (old != published_->end() && old->key() == r.key() && old->noon_s == r.noon_s &&
                        old->next_noon_s == r.next_noon_s && old->start_ms == r.start_ms &&
                        old->size() == r.size() && old->modified == r.modified) {
                        r.planes = old->planes;
                        r.invalid_tracks = old->invalid_tracks;
                        r.last_track_warning = old->last_track_warning;
                    }
                }
            }
            if (signals_) signals_->reset();
            preparing_.reset();
            prepare_pending_ = std::any_of(building_->begin(), building_->end(),
                std::mem_fn(&ReportOximetryRecording::needs_tracks));
            manifest_only_ = true;
            signal_retry_at_ = 0;
            resolve_oximetry_devices(*building_);
            if (ring_changed_) ring_sources_ = building_;
            originals_ = building_;
            published_ = std::move(building_);
            scanning_ = {};
            release();
        }
        return true;
    }

    std::string_view source, id;
    if (entry.directory || !entry.size || !archive_name(entry.path, clocks_, source, id) ||
        (clocks_ && entry.size > 128 * 1024)) {
        ++index_;
        return true;
    }
    if (!prepared_.valid() && !read_ticket_.valid()) {
        // Publishing a live CSV must not reread historical files or rerun the
        // BIN converter. Reuse only immutable files still present on this SD.
        const bool live = id.substr(0, 5) == "live-";
        if (!clocks_ && part_ == FilePart::Prefix && published_) {
            const auto key = std::make_pair(source, id);
            const auto it = std::lower_bound(published_->begin(), published_->end(), key,
                [](const auto &recording, const auto &wanted) { return recording.key() < wanted; });
            if (it != published_->end() && it->key() == key && it->size() == entry.size &&
                it->modified == entry.modified) {
                if (live || !ring_changed_) {
                    building_->push_back(*it);
                    ++index_;
                    return true;
                }

                // A scoped publication may revise the clock, not the bytes
                // of an existing immutable BIN. A full remount scan rechecks it.
                if (scanning_.source[0]) {
                    metadata_ = {};
                    metadata_.bin = it->bin;
                    metadata_.samples = it->samples;
                    metadata_.interval_s = it->interval_s;
                    if (ring_start(it->wall_s, id, metadata_)) add_file(entry, source, id);
                    ++index_;
                    return true;
                }
            }
        }
        StorageReadCommand cmd;
        cmd.path = entry.path;
        cmd.length = clocks_ ? entry.size : std::min<uint64_t>(entry.size, 40);
        if (live) {
            cmd.length = std::min<uint64_t>(entry.size, part_ == FilePart::LiveTail
                ? OximetryCsv::MaxRow : OximetryCsv::MaxHeader);
            cmd.offset = part_ == FilePart::LiveTail ? entry.size - cmd.length : 0;
        } else if (part_ == FilePart::SFooter) {
            cmd.length = RingBinLayout::SFooter;
            cmd.offset = entry.size - cmd.length;
        }
        cmd.generation = generation_;
        const auto submission = read_->request_read(cmd);
        if (submission.admission == OperationAdmission::Rejected) {
            release();
            retry_at_ = now_ms + 60000;
            return false;
        }
        read_ticket_ = submission.ticket;
        return submission.accepted();
    }
    if (!prepared_.valid()) {
        StorageReadCompletion result;
        if (!read_->take_completion(read_ticket_, result)) return false;
        read_ticket_ = {};
        prepared_ = result.prepared;
        if (result.outcome.disposition != OperationDisposition::Succeeded ||
            result.file_size != entry.size || result.modified != entry.modified || !prepared_.valid()) {
            release();
            retry_at_ = now_ms + 10000;
            return false;
        }
    }
    const auto view = read_->view_prepared(prepared_);
    if (view.state == PreparedByteReadState::Retry) return false;
    bool finished = true;
    if (view.valid()) {
        if (clocks_) consume_clock(source, view.data, view.length);
        else finished = consume_file(entry, source, id, view.data, view.length);
    }
    read_->release_prepared(prepared_);
    prepared_ = {};
    if (finished) {
        part_ = FilePart::Prefix;
        ++index_;
    }
    return true;
}

void ReportOximetryArchive::pause_signals() {
    if (signals_ && preparing_ && !manifest_only_) signals_->pause();
}

bool ReportOximetryArchive::poll_signals(uint32_t now_ms, bool background_allowed) try {
    if (!signals_ || !prepare_pending_ || !published_ ||
        (!preparing_ && signal_retry_at_ && int32_t(now_ms - signal_retry_at_) < 0)) return false;
    if (!manifest_only_ && !background_allowed) {
        pause_signals();
        return false;
    }
    if (!preparing_) {
        preparing_ = std::allocate_shared<ReportOximetryCatalog>(
            LargeAllocator<ReportOximetryCatalog>(), *published_);
        signal_index_ = 0;
        signal_retry_at_ = 0;
    }

    while (signal_index_ < preparing_->size()) {
        const auto &r = (*preparing_)[signal_index_];
        if (r.needs_tracks()) break;
        ++signal_index_;
    }
    if (signal_index_ == preparing_->size()) {
        published_ = std::move(preparing_);
        prepare_pending_ = signal_retry_at_ != 0 || (manifest_only_ &&
            std::any_of(published_->begin(), published_->end(),
                std::mem_fn(&ReportOximetryRecording::needs_tracks)));
        manifest_only_ = false;
        return true;
    }

    using Mode = ReportOximetrySignalBuilder::Mode;
    const bool worked = signals_->step((*preparing_)[signal_index_], manifest_only_ ? Mode::Manifest : Mode::Build);
    if (signals_->finished()) {
        if (signals_->source_changed()) {
            const auto &r = (*preparing_)[signal_index_];
            invalidate(ReportOximetryChange::published(!r.csv_bytes, r.source));
        }
        if (signals_->retryable()) signal_retry_at_ = now_ms + 60000;
        signals_->reset();
        ++signal_index_;
    }
    return worked;
} catch (const std::bad_alloc &) {
    signals_->reset();
    preparing_.reset();
    signal_retry_at_ = now_ms + 60000;
    return false;
}

void ReportOximetryArchive::withdraw(const ReportOximetryRecording &recording) {
    auto revised = std::allocate_shared<ReportOximetryCatalog>(
        LargeAllocator<ReportOximetryCatalog>(), *published_);
    for (auto &r : *revised) {
        if (r.key() == recording.key() && r.noon_s == recording.noon_s) r.planes.reset();
    }

    if (signals_) signals_->reset();
    preparing_.reset();
    published_ = std::move(revised);
    manifest_only_ = false;
    prepare_pending_ = true;
}

bool ReportOximetryArchive::consume_file(const StorageScanEntryView &entry, std::string_view source,
                                         std::string_view id, const uint8_t *bytes, size_t length) {
    auto &r = metadata_;
    if (part_ == FilePart::Prefix) {
        r = {};
        if (id.substr(0, 5) == "live-") {
            r.export_clock_known = OximetryCsv::parse_header(
                {reinterpret_cast<const char *>(bytes), length}, source, id, r);
            part_ = FilePart::LiveTail;
            return false;
        }

        if (length >= sizeof(SHeader) && !memcmp(bytes, SHeader, sizeof(SHeader))) {
            r.bin.format = RingBinLayout::O2RingS;
            if (entry.size < r.bin.offset(1) + r.bin.footer()) return true;

            part_ = FilePart::SFooter;
            return false;
        }
        if (!decode_original_header(bytes, length, entry.size, id, r)) return true;
    } else if (part_ == FilePart::LiveTail) {
        if (!live_bounds(id, bytes, length, r)) return true;
        r.csv_bytes = entry.size;
    } else {
        if (!RingBinLayout::s_footer_matches(bytes, length, entry.size) ||
            !ring_start(get_le32(bytes + 8), id, r)) return true;

        r.samples = get_le32(bytes + 12);
        r.interval_s = 1;
    }

    add_file(entry, source, id);
    return true;
}

void ReportOximetryArchive::add_file(const StorageScanEntryView &entry, std::string_view source,
                                     std::string_view id) {
    auto &r = metadata_;
    memcpy(r.source, source.data(), source.size());
    if (!r.csv_bytes) {
        // The importer uses SN, or the peer's twelve hex digits without an SN.
        // Only an explicit clock record can disambiguate a twelve-hex SN.
        if (source.size() != 12 || source.find_first_not_of("0123456789abcdefABCDEF") != source.npos)
            strcpy(r.serial, r.source);
        ring_changed_ = true;
    }

    memcpy(r.id, id.data(), id.size());
    r.modified = entry.modified;
    building_->push_back(r);
}

void ReportOximetryArchive::consume_clock(std::string_view source, const uint8_t *bytes, size_t length) {
    LargeJsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (deserializeJson(doc, bytes, length)) return;
    const JsonString names = doc["FileList"].as<JsonString>();
    if (names.isNull() || !doc["utc_ms"].is<int64_t>() ||
        !doc["ring_minus_utc_s"].is<int64_t>()) return;
    const int64_t utc = doc["utc_ms"];
    const int64_t delta = doc["ring_minus_utc_s"];
    if (utc < 1704067200000LL || utc > 4102444800000LL ||
        delta < -3155760000LL || delta > 3155760000LL) return;
    const std::string_view list(names.c_str(), names.size());

    // Membership is stronger evidence than comparing wall times across a
    // backward step. A later sync must never reinterpret an already listed file.
    for (auto &recording : *building_) {
        if (recording.csv_bytes || source != recording.source) continue;
        const char *peer = doc["ble"] | "";
        const char *serial = doc["serial"] | "";
        if (strlen(peer) == 17 && strspn(peer, "0123456789abcdefABCDEF:") == 17)
            strcpy(recording.peer, peer);
        if (oximetry_source_valid(serial) && source == serial) strcpy(recording.serial, serial);
        if (recording.listed && recording.observation_ms <= utc) continue;
        const auto pos = list.find(recording.id);
        const bool listed = pos != list.npos && (!pos || list[pos-1] == ',') &&
            (pos + 14 == list.size() || list[pos+14] == ',');
        if (!listed && (recording.listed || recording.local_start_s >= utc / 1000 ||
                        recording.wall_s - delta >= utc / 1000)) continue;
        if (recording.observation_ms && recording.listed == listed &&
            recording.observation_ms <= utc) continue;
        recording.listed = listed;
        recording.observation_ms = utc;
        recording.start_ms = (recording.wall_s - delta) * 1000;
    }
}

}  // namespace aircannect
