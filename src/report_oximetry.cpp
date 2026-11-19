#include "report_oximetry.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include "calendar_utils.h"
#include "large_json_allocator.h"
#include "oximetry_archive.h"

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
    out.local_start_s = out.start_s = mktime(&local);
    out.samples = samples;
    out.interval_s = duration / samples;
    return out.start_s >= 0;
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
    return std::string(OximetryArchiveRoot) + "/" + source + "/" + std::string(id, 8) +
           "/" + id + ".bin";
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
    clocks_ = false;
}

void ReportOximetryArchive::reset() {
    release();
    published_.reset();
    observed_ = retry_at_ = 0;
}

bool ReportOximetryArchive::poll(uint32_t now_ms) try {
    return step(now_ms);
} catch (const std::bad_alloc &) {
    release();
    retry_at_ = now_ms + 60000;
    return false;
}

bool ReportOximetryArchive::step(uint32_t now_ms) {
    if (!files_) {
        if (!scan_ticket_.valid()) {
            generation_ = oximetry_archive_revision.load();
            if (generation_ == observed_ || (retry_at_ && int32_t(now_ms - retry_at_) < 0))
                return false;
            const StorageScanRoot root{OximetryArchiveRoot, true};
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
        building_ = std::allocate_shared<ReportOximetryCatalog>(LargeAllocator<ReportOximetryCatalog>());
        return true;
    }

    StorageScanEntryView entry;
    if (!files_->entry(index_, entry)) {
        if (!clocks_ && !building_->empty()) {
            clocks_ = true;
            index_ = 0;
        } else {
            const size_t originals = building_->size();
            for (size_t i = 0; i < originals; ++i) {
                auto slice = (*building_)[i];
                if (!night_window(slice.start_s, slice)) continue;
                (*building_)[i] = slice;
                const int64_t end = slice.start_s + int64_t(slice.samples) * slice.interval_s;
                while (slice.next_noon_s < end && night_window(slice.next_noon_s, slice))
                    building_->push_back(slice);
            }
            published_ = std::move(building_);
            observed_ = generation_;
            release();
        }
        return true;
    }

    std::string_view source, id;
    if (entry.directory || !archive_name(entry.path, clocks_, source, id) ||
        (clocks_ && (!entry.size || entry.size > 128 * 1024))) {
        ++index_;
        return true;
    }
    if (!prepared_.valid() && !read_ticket_.valid()) {
        StorageReadCommand cmd;
        cmd.path = entry.path;
        cmd.length = clocks_ ? entry.size : 40;
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
    if (view.valid()) consume(entry, view.data, view.length);
    read_->release_prepared(prepared_);
    prepared_ = {};
    ++index_;
    return true;
}

void ReportOximetryArchive::consume(const StorageScanEntryView &entry, const uint8_t *bytes,
                                    size_t length) {
    std::string_view source, id;
    if (!archive_name(entry.path, clocks_, source, id)) return;
    if (!clocks_) {
        ReportOximetryRecording recording;
        if (!decode_header(bytes, length, entry.size, id, recording)) return;
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
        if (source != recording.source ||
            (recording.listed && recording.observation_ms <= utc)) continue;
        const auto pos = list.find(recording.id);
        const bool listed = pos != list.npos && (!pos || list[pos-1] == ',') &&
            (pos + 14 == list.size() || list[pos+14] == ',');
        if (!listed && (recording.listed || recording.local_start_s >= utc / 1000 ||
                        recording.wall_s - delta >= utc / 1000)) continue;
        if (recording.observation_ms && recording.listed == listed &&
            recording.observation_ms <= utc) continue;
        recording.listed = listed;
        recording.observation_ms = utc;
        recording.start_s = recording.wall_s - delta;
    }
}

}  // namespace aircannect
