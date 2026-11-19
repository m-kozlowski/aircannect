#include "o2ring_recording_import.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include "board_oximetry.h"
#include "debug_log.h"
#include "large_json_allocator.h"
#include "storage_space_policy.h"

namespace aircannect {

void O2RingRecordingImport::begin(const char *peer, StoragePathPort &paths,
                                  StorageRangeWritePort &writes) {
    reset();
    paths_ = &paths;
    writes_ = &writes;
    source_ = peer;
    source_.erase(std::remove(source_.begin(), source_.end(), ':'), source_.end());
    window_ = Window::Unknown;
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

bool O2RingRecordingImport::take_list(const uint8_t *data, size_t size) {
    LargeJsonAllocator allocator;
    JsonDocument doc(&allocator);
    if (deserializeJson(doc, data, size)) return false;
    const JsonString list = doc["FileList"].as<JsonString>();
    if (list.isNull()) return false;

    // On the second INFO, skip the already checked prefix of complete IDs.
    const size_t checked = list_ ? std::min(cursor_, list_->size()) : 0;
    cursor_ = 0;
    if (list_ && list.size()) {
        const std::string_view before(reinterpret_cast<const char *>(list_->data()), list_->size());
        const std::string_view after(list.c_str(), list.size());
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
        list_ = LargeByteBuffer::copy_and_freeze(list.c_str(), list.size());
        if (!list_) return false;
    }

    const JsonString serial = doc["SN"].as<JsonString>();
    if (!serial.isNull() && serial.size() && serial.size() <= 32) {
        const std::string_view sn(serial.c_str(), serial.size());
        if (sn.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ-_") == sn.npos)
            source_ = std::string(sn);
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
        final_ = "/aircannect/oximetry/" + source_ + "/" + std::string(id.substr(0, 8))
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

void O2RingRecordingImport::skip_open() {
    if (phase_ != Phase::Open) return;
    Log::logf(CAT_OXI, LOG_WARN, "recording not available file=%s\n", id_);
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
        case Command::Info:
            if (phase_ != Phase::Info) return;
            if (!take_list(data, len)) return fail("invalid_recording_list");
            phase_ = Phase::Select;
            break;
        case Command::Open:
            if (phase_ != Phase::Open) return;
            if (len != 4) return fail("invalid_file_size");
            size_ = uint32_t(data[0]) | uint32_t(data[1]) << 8 |
                    uint32_t(data[2]) << 16 | uint32_t(data[3]) << 24;
            if (!size_ || size_ > BlockSize * 65536u) return fail("unsupported_file_size");
            part_ = final_ + "." + std::to_string(size_) + ".part";
            phase_ = Phase::FinalStat;
            break;
        case Command::Read:
            if (phase_ != Phase::Read) return;
            if (index != block() || len != std::min<size_t>(BlockSize, size_ - offset_))
                return fail("invalid_file_block");
            chunk_ = LargeByteBuffer::copy_and_freeze(data, len);
            if (!chunk_) return fail("out_of_memory");
            phase_ = Phase::Write;
            break;
        case Command::Close:
            if (phase_ != Phase::Close) return;
            phase_ = skip_publication_ ? Phase::Select : Phase::Publish;
            break;
        case Command::None: break;
    }
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
        // Re-read an interrupted block in full. A complete partial file only needs publication.
        offset_ = result.size == size_ ? size_ : (result.size / BlockSize) * BlockSize;
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
