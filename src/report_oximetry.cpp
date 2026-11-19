#include "report_oximetry.h"
#include "report_oximetry_signals.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <ctime>
#include "calendar_utils.h"
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
    const std::string_view full(path);
    constexpr size_t prefix = sizeof(OximetryArchiveRoot);
    if (full.size() <= prefix || full.substr(0, prefix - 1) != OximetryArchiveRoot ||
        full[prefix - 1] != '/') return false;
    const auto rest = full.substr(prefix);
    const auto slash = rest.find('/');
    if (slash == rest.npos || !oximetry_source_valid(source = rest.substr(0, slash))) return false;
    const auto suffix = rest.substr(slash + 1);
    if (clock) return suffix.size() > 16 && suffix.substr(0, 6) == "clock/" &&
                      suffix.substr(suffix.size() - 5) == ".json";
    if (suffix.size() == 49 && suffix.substr(8, 6) == "/live-" &&
        suffix.substr(45) == ".csv") {
        id = suffix.substr(9, 36);
        return (source == "udp" || (source.size() == 16 && source.substr(0, 4) == "ble-" &&
                source.substr(4).find_first_not_of("0123456789abcdef") == source.npos)) &&
            id.substr(5, 8) == suffix.substr(0, 8) && id[13] == '-' && id[27] == '-' &&
            id.substr(5, 8).find_first_not_of("0123456789") == id.npos &&
            id.substr(14, 13).find_first_not_of("0123456789") == id.npos &&
            id.substr(28).find_first_not_of("0123456789abcdef") == id.npos;
    }
    if (suffix.size() != 27 || suffix[8] != '/' || suffix.substr(23) != ".bin") return false;
    id = suffix.substr(9, 14);
    return id.substr(0, 8) == suffix.substr(0, 8) && id.find_first_not_of("0123456789") == id.npos;
}

uint32_t little32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

bool decode_header(const uint8_t *p, size_t length, uint64_t size, std::string_view id,
                   ReportOximetryRecording &out) {
    if (length != 40 || p[0] != 3 || p[1] != 0 || size <= 40 ||
        size != little32(p + 9) || (size - 40) % 5) return false;
    const auto samples = static_cast<uint32_t>((size - 40) / 5);
    const auto duration = little32(p + 13);
    if (duration != samples * 2ULL && duration != samples * 4ULL) return false;

    const int year = p[2] + 256 * p[3];
    char stamp[16];
    if (year < 2000 || year > 2099 || p[4] < 1 || p[4] > 12 || p[5] < 1 ||
        p[5] > calendar_days_in_month(year, p[4]) || p[6] > 23 || p[7] > 59 || p[8] > 59 ||
        snprintf(stamp, sizeof(stamp), "%04d%02u%02u%02u%02u%02u",
                 year,p[4],p[5],p[6],p[7],p[8]) != 14 || id != stamp) return false;

    out.wall_s = calendar_days_from_civil(year, p[4], p[5]) * 86400 +
                 p[6] * 3600 + p[7] * 60 + p[8];
    struct tm local{};
    local.tm_year = year - 1900; local.tm_mon = p[4] - 1; local.tm_mday = p[5];
    local.tm_hour = p[6]; local.tm_min = p[7]; local.tm_sec = p[8]; local.tm_isdst = -1;
    out.local_start_s = mktime(&local);
    out.start_ms = out.local_start_s * 1000;
    out.samples = samples;
    out.interval_s = duration / samples;
    return out.local_start_s >= 0;
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
    if (signals_) signals_->reset();
    preparing_signals_ = false;
    if (scan_ticket_.valid()) scan_->abandon(scan_ticket_);
    if (read_ticket_.valid()) read_->abandon(read_ticket_);
    if (prepared_.valid()) read_->release_prepared(prepared_);
    scan_ticket_ = {};
    read_ticket_ = {};
    prepared_ = {};
    files_.reset();
    building_.reset();
    index_ = 0;
    clocks_ = false;
}

void ReportOximetryArchive::reset() {
    release();
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
    if (preparing_signals_) {
        if (signals_ && index_ < building_->size()) {
            auto &recording = (*building_)[index_];
            if (recording.planes) { ++index_; return true; }
            const bool worked = signals_->step(recording);
            if (signals_->finished()) {
                if (signals_->retryable()) {
                    pending_.merge(ReportOximetryChange::published(!recording.csv_bytes,
                        recording.source, recording.csv_bytes ? std::string_view(recording.id+5, 8) :
                        std::string_view{}));
                    retry_at_ = now_ms + 60000;
                }
                signals_->reset();
                ++index_;
            }
            return worked;
        }
        published_ = std::move(building_);
        scanning_ = {};
        release();
        return true;
    }
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
                if (!slice.csv_bytes) {
                    edf_configured_timezone_offset_minutes(slice.start_ms, slice.offset_minutes);
                    // The EDF day may differ from the ESP sleep day near noon.
                    const int64_t utc_day = slice.start_ms / 86400000;
                    for (int delta = -2; nights && delta <= 1; ++delta) {
                        SleepDayId day;
                        if (!SleepDayId::from_epoch_days(utc_day + delta, day)) continue;
                        const auto *record = nights->find(day);
                        if (record && record->timezone_offset_valid &&
                            record->day_start_ms <= slice.start_ms && slice.start_ms < record->day_end_ms) {
                            slice.offset_minutes = record->timezone_offset_minutes;
                            break;
                        }
                    }
                }
                (*building_)[i] = slice;
                while (slice.next_noon_s * 1000 < slice.end_ms() && night_window(slice.next_noon_s, slice)) {
                    slice.planes.reset();
                    building_->push_back(slice);
                }
            }
            std::sort(building_->begin(), building_->end(), [](const auto &a, const auto &b) {
                return a.key() == b.key() ? a.noon_s < b.noon_s : a.key() < b.key();
            });
            preparing_signals_ = true;
            index_ = 0;
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
        if (!clocks_ && published_ && (live || !ring_changed_)) {
            const auto key = std::make_pair(source, id);
            const auto it = std::lower_bound(published_->begin(), published_->end(), key,
                [](const auto &recording, const auto &wanted) { return recording.key() < wanted; });
            if (it != published_->end() && it->key() == key && it->size() == entry.size &&
                it->modified == entry.modified) {
                building_->push_back(*it);
                ++index_;
                return true;
            }
        }
        StorageReadCommand cmd;
        cmd.path = entry.path;
        cmd.length = clocks_ ? entry.size : 40;
        if (live) {
            cmd.length = std::min<uint64_t>(entry.size, OximetryCsv::MaxRow);
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
            result.file_size != entry.size || !prepared_.valid()) {
            release();
            retry_at_ = now_ms + 10000;
            return false;
        }
    }
    const auto view = read_->view_prepared(prepared_);
    if (view.state == PreparedByteReadState::Retry) return false;
    if (view.valid()) consume(entry, source, id, view.data, view.length);
    read_->release_prepared(prepared_);
    prepared_ = {};
    ++index_;
    return true;
}

void ReportOximetryArchive::consume(const StorageScanEntryView &entry, std::string_view source,
                                    std::string_view id, const uint8_t *bytes, size_t length) {
    if (!clocks_) {
        ReportOximetryRecording recording;
        if (id.substr(0, 5) == "live-") {
            if (!live_bounds(id, bytes, length, recording)) return;
            recording.csv_bytes = entry.size;
        } else {
            if (!decode_header(bytes, length, entry.size, id, recording)) return;
            // Legacy imports used SN, or twelve MAC digits when INFO had no SN.
            if (source.size() != 12 || source.find_first_not_of("0123456789abcdefABCDEF") != source.npos)
                memcpy(recording.serial, source.data(), source.size());
            ring_changed_ = true;
        }
        memcpy(recording.source, source.data(), source.size());
        memcpy(recording.id, id.data(), id.size());
        recording.modified = entry.modified;
        building_->push_back(recording);
        return;
    }

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
