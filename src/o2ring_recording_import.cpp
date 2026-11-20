#include "o2ring_recording_import.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <ctime>
#include "board_oximetry.h"
#include "debug_log.h"
#include "large_json_allocator.h"
#include "little_endian.h"
#include "storage_space_policy.h"
#include "utc_time.h"
#include "oximetry_archive.h"

namespace aircannect {

void O2RingRecordingImport::begin(const char *peer, StoragePathPort &paths,
                                  StorageRangeWritePort &writes,
                                  StorageAtomicWritePort *clock_storage,
                                  void (*published)(const ReportOximetryChange &)) {
    end();
    paths_ = &paths;
    writes_ = &writes;
    clock_storage_ = clock_storage;
    published_ = published;
    downloads_ = true;
    source_ = peer;
    snprintf(peer_, sizeof(peer_), "%s", peer);
    source_.erase(std::remove(source_.begin(), source_.end(), ':'), source_.end());
    window_ = Window::Unknown;
}

void O2RingRecordingImport::end() {
    reset();
    serial_[0] = peer_[0] = 0;
    if (clock_ticket_.valid()) clock_storage_->abandon(clock_ticket_);
    clock_ticket_ = {};
    clock_write_ = {};
    clock_storage_ = nullptr;
}

void O2RingRecordingImport::enable_downloads(bool enabled) {
    if (enabled == downloads_) return;
    reset();
    window_ = Window::Unknown;
    downloads_ = enabled;
}

bool O2RingRecordingImport::save_clock(const uint8_t *info, size_t size,
                                       int64_t utc_ms, uint32_t rtt_ms) {
    if (!clock_storage_ || utc_ms < 1704067200000LL || clock_write_.bytes) return false;
    LargeJsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (deserializeJson(doc, info, size)) return false;
    const JsonString clock = doc["CurTIME"].as<JsonString>();
    const JsonString list = doc["FileList"].as<JsonString>();
    if (clock.size() != 19 || clock.c_str()[10] != ',' || list.isNull()) return false;
    char iso[22] = {};
    memcpy(iso, clock.c_str(), 19);
    iso[10] = 'T';
    iso[19] = 'Z';
    int64_t wall_ms = 0;
    if (!parse_utc_iso8601_ms(iso, wall_ms)) return false;

    identify(doc["SN"] | "");
    return save_clock({list.c_str(), list.size()}, wall_ms, utc_ms, rtt_ms);
}

void O2RingRecordingImport::identify(std::string_view sn) {
    if (oximetry_source_valid(sn)) {
        source_.assign(sn);
        strcpy(serial_, source_.c_str());
    }
}

bool O2RingRecordingImport::save_clock(std::string_view list, int64_t wall_ms,
                                       int64_t utc_ms, uint32_t rtt_ms) {
    if (!clock_storage_ || utc_ms < 1704067200000LL || clock_write_.bytes ||
        !oximetry_source_valid(source_)) return false;

    // Use the midpoint of INFO, keeping the observation's round-trip uncertainty.
    const int64_t observed = utc_ms - rtt_ms / 2;
    const time_t seconds = observed / 1000;
    struct tm local = {};
    char month[12] = {};
    char zone[8] = {};
    if (!localtime_r(&seconds, &local) || !strftime(month, sizeof(month), "%Y/%m", &local) ||
        !strftime(zone, sizeof(zone), "%z", &local)) return false;

    LargeJsonAllocator allocator;
    JsonDocument observation(&allocator);
    observation["utc_ms"] = observed;
    observation["ring_minus_utc_s"] = wall_ms / 1000 - seconds;
    observation["reference"] = clock_source_.load();
    observation["ble"] = peer_;
    if (serial_[0]) observation["serial"] = serial_;
    observation["local_offset"] = zone;
    observation["rtt_ms"] = rtt_ms;
    observation["FileList"] = list;
    if (observation.overflowed()) return false;
    auto bytes = LargeByteBuffer::allocate(measureJson(observation));
    if (!bytes || serializeJson(observation, bytes->data(), bytes->size()) != bytes->size())
        return false;

    clock_write_.path = std::string(OximetryArchiveRoot) + "/" + source_ + "/clock/" +
        month + "/" + std::to_string(observed) + ".json";
    clock_write_.bytes = LargeByteBuffer::freeze(std::move(bytes));
    clock_write_.replace_existing = false;
    clock_write_.generation = 1;
    return true;
}

OperationDisposition O2RingRecordingImport::poll_clock() {
    if (!clock_storage_ || !clock_write_.bytes) return OperationDisposition::Failed;
    if (!clock_ticket_.valid()) {
        const auto submission = clock_storage_->request_write(clock_write_);
        if (submission.admission == OperationAdmission::Busy) return OperationDisposition::Retry;
        if (submission.accepted()) {
            clock_ticket_ = submission.ticket;
            return OperationDisposition::Retry;
        }
        clock_write_ = {};
        return OperationDisposition::Failed;
    }

    StorageAtomicWriteCompletion completion;
    if (!clock_storage_->take_completion(clock_ticket_, completion)) return OperationDisposition::Retry;
    const bool ok = completion.outcome.disposition == OperationDisposition::Succeeded &&
                    completion.bytes_written == clock_write_.bytes->size();
    clock_write_ = {};
    clock_ticket_ = {};
    if (ok && published_) published_(ReportOximetryChange::published(true, source_));
    return ok ? OperationDisposition::Succeeded : OperationDisposition::Failed;
}

bool O2RingRecordingImport::off_finger_timeout(uint32_t now_ms) const {
    return window_ == Window::OffFinger &&
           now_ms - idle_since_ms_ >= AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS;
}

bool O2RingRecordingImport::needs_connection(uint32_t now_ms) const {
    if (!active() || now_ms - last_contact_ms_ >= AC_OXIMETRY_SENSOR_NOTIFY_TIMEOUT_MS)
        return false;

    // The hub's invalid-reading timer may have started before contact was lost.
    return now_ms - idle_since_ms_ < AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS ||
           (last_write_ms_ && now_ms - *last_write_ms_ < WriteProgressGraceMs);
}

void O2RingRecordingImport::observe_contact(bool on_finger, bool charging, uint32_t now_ms) {
    if (!downloads_) return;
    last_contact_ms_ = now_ms;
    const Window next = charging ? Window::Charging :
                        on_finger ? Window::Worn : Window::OffFinger;

    if (next != window_) {
        if (next == Window::Worn) {
            if (active()) Log::logf(CAT_OXI, LOG_DEBUG, "recording import paused: sensor worn\n");
            reset();
        } else if (window_ == Window::Unknown || window_ == Window::Worn) {
            // The newest file first appeared about ten seconds after removal on the O2Ring.
            idle_since_ms_ = now_ms;
            phase_ = Phase::WaitForSave;
            recheck_pending_ = window_ == Window::Worn;
        } else if (!active()) {
            idle_since_ms_ = now_ms;
            phase_ = Phase::Info;
        }
        window_ = next;
    }

    if (phase_ == Phase::WaitForSave && now_ms - idle_since_ms_ >= RecordingSettleMs) {
        phase_ = Phase::Info;
    }
}

void O2RingRecordingImport::reset() {
    if (ticket_.valid()) {
        if (phase_ == Phase::Write) writes_->abandon(ticket_);
        else paths_->abandon(ticket_);
    }
    ticket_ = {};
    list_.reset();
    chunk_.reset();
    recheck_pending_ = false;
    last_write_ms_.reset();
    cursor_ = 0;
    recheck_from_.reset();
    saved_ = 0;
    id_[0] = '\0';
    offset_ = size_ = 0;
    phase_ = Phase::Idle;
}

void O2RingRecordingImport::fail(const char *reason) {
    Log::logf(CAT_OXI, LOG_WARN, "recording import stopped file=%s block=%u reason=%s\n",
              id_, unsigned(block()), reason);
    reset();
}

bool O2RingRecordingImport::take_list(std::string_view list) {
    // On the second INFO, skip the already checked prefix of complete IDs.
    const size_t checked = list_ ? std::min(recheck_from_.value_or(cursor_), list_->size()) : 0;
    recheck_from_.reset();
    cursor_ = 0;
    if (list_ && list.size()) {
        const std::string_view before(reinterpret_cast<const char *>(list_->data()), list_->size());
        const auto after = list;
        const auto end = std::mismatch(after.begin(), after.end(), before.begin(), before.end()).first;
        if (end == after.end() && after.size() == before.size()) {
            cursor_ = checked;
            return true;
        }
        const auto comma = after.substr(0, end - after.begin()).rfind(',');
        if (comma != after.npos) cursor_ = std::min(checked, comma + 1);
    }

    list_.reset();
    if (list.size()) {
        list_ = LargeByteBuffer::copy_and_freeze(list.data(), list.size());
        if (!list_) return false;
    }

    return true;
}

void O2RingRecordingImport::next_file() {
    while (list_ && cursor_ < list_->size()) {
        const std::string_view rest(reinterpret_cast<const char *>(list_->data()) + cursor_,
                                    list_->size() - cursor_);
        const auto id = rest.substr(0, rest.find(','));
        cursor_ += id.size() + 1;
        if (id.empty()) continue;
        if (id.size() != 14 || id.find_first_not_of("0123456789") != id.npos) {
            Log::logf(CAT_OXI, LOG_WARN, "skipping invalid recording ID\n");
            continue;
        }

        memcpy(id_, id.data(), 14);
        final_ = std::string(OximetryArchiveRoot) + "/" + source_ + "/" + std::string(id.substr(0, 8))
               + "/" + id_ + ".bin";
        offset_ = size_ = 0;
        skip_publication_ = false;
        phase_ = Phase::Open;
        return;
    }
    if (saved_) Log::logf(CAT_OXI, LOG_INFO, "recordings saved to SD files=%u\n", saved_);
    if (recheck_pending_) {
        saved_ = 0;
        last_write_ms_.reset();
        phase_ = Phase::WaitForRecheck;
        return;
    }
    reset();
}

void O2RingRecordingImport::skip_open(bool not_finalized) {
    if (phase_ != Phase::Open) return;
    if (not_finalized && !recheck_from_) recheck_from_ = cursor_ - strlen(id_) - 1;
    Log::logf(CAT_OXI, not_finalized ? LOG_DEBUG : LOG_WARN, "recording not ready file=%s\n", id_);
    phase_ = Phase::Select;
}

O2RingRecordingImport::Command O2RingRecordingImport::next(uint32_t now_ms) {
    switch (phase_) {
        case Phase::Info: return Command::Info;
        case Phase::Select:
        case Phase::WaitForRecheck:
            // Recheck between files, leaving time for the first write before disconnect.
            if (recheck_pending_ && now_ms - idle_since_ms_ >=
                    AC_OXIMETRY_SENSOR_INVALID_DISCONNECT_MS - 5000) {
                recheck_pending_ = false;
                phase_ = Phase::Info;
                return Command::Info;
            }
            if (phase_ == Phase::Select) next_file();
            break;
        case Phase::Open: return Command::Open;
        case Phase::Read: return Command::Read;
        case Phase::Close: return Command::Close;
        case Phase::Write: step_write(now_ms); break;
        case Phase::FinalStat:
        case Phase::PartialStat:
        case Phase::Publish: step_path(); break;
        case Phase::WaitForSave:
        case Phase::Idle: break;
    }
    return Command::None;
}

void O2RingRecordingImport::reply(Command cmd, uint16_t index,
                                  const uint8_t *data, size_t len) {
    switch (cmd) {
        case Command::Info: {
            if (phase_ != Phase::Info) return;
            LargeJsonAllocator allocator;
            JsonDocument doc(&allocator);
            if (deserializeJson(doc, data, len)) return fail("invalid_recording_list");
            const auto ids = doc["FileList"].as<JsonString>();
            if (ids.isNull()) return fail("invalid_recording_list");
            identify(doc["SN"] | "");
            listed({ids.c_str(), ids.size()});
            break;
        }
        case Command::Open:
            if (phase_ != Phase::Open) return;
            if (len != 4) return fail("invalid_file_size");
            opened(LittleEndian::get_le32(data), Addressing::Blocks);
            break;
        case Command::Read:
            if (phase_ != Phase::Read) return;
            if (index != block() || len != std::min<size_t>(BlockSize, size_ - offset_))
                return fail("invalid_file_block");
            received(uint32_t(index) * BlockSize, data, len);
            break;
        case Command::Close:
            if (phase_ != Phase::Close) return;
            phase_ = skip_publication_ ? Phase::Select : Phase::Publish;
            break;
        case Command::None: break;
    }
}

void O2RingRecordingImport::listed(std::string_view ids) {
    if (phase_ != Phase::Info) return;
    if (!take_list(ids)) return fail("invalid_recording_list");
    phase_ = Phase::Select;
}

void O2RingRecordingImport::opened(uint32_t size, Addressing addressing) {
    if (phase_ != Phase::Open) return;
    if (!size || (addressing == Addressing::Blocks && size > BlockSize * 65536u))
        return fail("unsupported_file_size");
    size_ = size;
    addressing_ = addressing;
    part_ = final_ + "." + std::to_string(size_) + ".part";
    phase_ = Phase::FinalStat;
}

void O2RingRecordingImport::received(uint32_t offset, const uint8_t *data, size_t size) {
    if (phase_ != Phase::Read) return;
    if (offset != offset_ || !size || size > std::min<size_t>(BlockSize, size_ - offset_))
        return fail("invalid_file_block");

    chunk_ = LargeByteBuffer::copy_and_freeze(data, size);
    if (!chunk_) return fail("out_of_memory");
    phase_ = Phase::Write;
}

void O2RingRecordingImport::step_path() {
    if (!ticket_.valid()) {
        StoragePathCommand c;
        c.generation = 1;
        c.source = phase_ == Phase::FinalStat ? final_ : part_;
        if (phase_ == Phase::Publish) {
            c.operation = StoragePathOperation::Move;
            c.destination = final_;
        }
        const auto s = paths_->request(c);
        if (s.admission == OperationAdmission::Rejected) return fail("storage_unavailable");
        ticket_ = s.ticket;
        return;
    }
    StoragePathCompletion result;
    if (!paths_->take_completion(ticket_, result)) return;
    ticket_ = {};
    if (result.outcome.disposition != OperationDisposition::Succeeded) return fail(result.error);

    if (phase_ == Phase::FinalStat) {
        if (result.exists && (result.directory || result.size != size_))
            Log::logf(CAT_OXI, LOG_WARN, "recording ID conflict file=%s; keeping SD copy\n", id_);
        skip_publication_ = result.exists;
        phase_ = skip_publication_ ? Phase::Close : Phase::PartialStat;
    } else if (phase_ == Phase::PartialStat) {
        if (result.directory || result.size > size_) {
            Log::logf(CAT_OXI, LOG_WARN, "recording partial file conflict file=%s\n", id_);
            skip_publication_ = true;
            phase_ = Phase::Close;
            return;
        }
        // Legacy READ addresses blocks; OxyII can continue at the exact SD length.
        offset_ = result.size;
        if (offset_ != size_ && addressing_ == Addressing::Blocks) offset_ -= offset_ % BlockSize;
        // A partial file is older unfinished work, not a newly discovered recording.
        if (!result.exists) recheck_pending_ = false;
        const auto space = writes_->space_snapshot();
        if (offset_ < size_ && space.known &&
            !space.fits(size_ - offset_, StorageSpacePolicy::WarnBelow))
            return fail("insufficient_free_space");

        phase_ = offset_ == size_ ? Phase::Close : Phase::Read;
        Log::logf(CAT_OXI, LOG_DEBUG, "recording file=%s bytes=%u resume_offset=%u\n",
                  id_, unsigned(size_), unsigned(offset_));
    } else {
        ++saved_;
        if (published_) published_(ReportOximetryChange::file(
            OximetryArchiveFile::RingBin, source_, std::string_view(id_, 8)));
        phase_ = Phase::Select;
    }
}

void O2RingRecordingImport::step_write(uint32_t now_ms) {
    if (!ticket_.valid()) {
        StorageRangeWriteCommand c;
        c.generation = 1;
        c.path = part_;
        c.offset = offset_;
        c.bytes = chunk_;
        const auto s = writes_->request_write(c);
        if (s.admission == OperationAdmission::Rejected) return fail("storage_write_unavailable");
        ticket_ = s.ticket;
        return;
    }
    StorageRangeWriteCompletion result;
    if (!writes_->take_completion(ticket_, result)) return;
    ticket_ = {};
    if (result.outcome.disposition != OperationDisposition::Succeeded ||
        result.bytes_written != chunk_->size()) return fail(result.error);

    offset_ += result.bytes_written;
    last_write_ms_ = now_ms;
    chunk_.reset();
    phase_ = offset_ == size_ ? Phase::Close : Phase::Read;
}

}  // namespace aircannect
