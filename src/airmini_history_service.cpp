#include "airmini_history_service.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <utility>

#include "airmini_history_data.h"
#include "edf_bytes.h"
#include "edf_day_statistics.h"
#include "edf_str_file_layout.h"
#include "edf_time.h"
#include "fixed_queue.h"
#include "large_object.h"
#include "large_text_buffer.h"
#include "string_util.h"

namespace aircannect {
namespace {

constexpr int64_t DAY_MS = 86400000;
constexpr uint32_t TRANSFER_TIMEOUT_MS = 30000;
constexpr size_t READ_BYTES = 32768;
constexpr AirMiniHistorySelector SELECTORS[] = {
    AirMiniHistorySelector::UsageEvents,
    AirMiniHistorySelector::RespiratoryEvents,
    AirMiniHistorySelector::InspiratoryPressure,
    AirMiniHistorySelector::Leak,
};

AirMiniHistoryLimits history_limits() {
    // Device retention capacities, not a small arbitrary per-night ceiling.
    AirMiniHistoryLimits limits;
    limits.samples_per_selector = 3650 * 60;
    limits.events = 80000;
    limits.settings_json_bytes = 32768;
    return limits;
}

std::string utc_text(int64_t epoch_ms) {
    const time_t seconds = static_cast<time_t>(epoch_ms / 1000);
    struct tm utc = {};
    gmtime_r(&seconds, &utc);
    char text[32] = {};
    strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S.000Z", &utc);
    return text;
}

}  // namespace

struct AirMiniHistoryService::Runtime {
    enum class ReadKind { History, StrHeader, StrRecord };

    AirMiniHistoryData fetched{history_limits()};
    AirMiniHistoryData saved{history_limits()};
    AirMiniHistoryData partial{history_limits()};
    EdfDayStatisticsReader edf;
    EdfStrSessionAccumulator seed;
    EdfStrSessionAccumulator output;
    AirMiniHistoryProjectionInput projection_input;
    AirMiniHistoryStrProjection projection;
    bool seed_valid = false;
    As11ClockTransform clock;
    int32_t timezone_minutes = 0;
    uint32_t start_at_ms = 0;
    uint32_t deadline_ms = 0;
    uint32_t rpc_generation = 0;
    uint32_t attempts = 0;
    bool settings = true;
    bool transfer_failed = false;
    bool response_received = false;
    bool overflow = false;
    OperationTicket rpc_ticket;
    FixedQueue<RpcPayloadRef, 8> notifications;

    ReadKind read_kind = ReadKind::History;
    OperationTicket read_ticket;
    OperationTicket write_ticket;
    StoragePreparedRead prepared;
    std::unique_ptr<LargeByteBuffer> cache;
    std::shared_ptr<const LargeByteBuffer> publication;
    size_t read_offset = 0;
    uint64_t str_offset = 0;
    char path[96] = {};
    int64_t day_start_ms = 0;
    int64_t day_end_ms = 0;
};

const char *airmini_history_phase_name(AirMiniHistoryPhase phase) {
    switch (phase) {
        case AirMiniHistoryPhase::Idle: return "idle";
        case AirMiniHistoryPhase::Waiting: return "waiting";
        case AirMiniHistoryPhase::Settings: return "settings";
        case AirMiniHistoryPhase::LoggedData: return "logged_data";
        case AirMiniHistoryPhase::ReadingSaved: return "reading_saved";
        case AirMiniHistoryPhase::ReadingEdf: return "reading_edf";
        case AirMiniHistoryPhase::Saving: return "saving";
        case AirMiniHistoryPhase::Finalizing: return "finalizing";
        case AirMiniHistoryPhase::RecordReady: return "record_ready";
        case AirMiniHistoryPhase::Finishing: return "finishing";
        case AirMiniHistoryPhase::Complete: return "complete";
        case AirMiniHistoryPhase::Failed: return "failed";
    }
    return "unknown";
}

AirMiniHistoryService::~AirMiniHistoryService() {
    cancel("shutdown");
    LargeObject::destroy(runtime_);
}

void AirMiniHistoryService::begin(StorageReadPort &read,
                                 StorageAtomicWritePort &write,
                                 StorageScanPort &scan) {
    read_ = &read;
    write_ = &write;
    scan_ = &scan;
}

OperationAdmission AirMiniHistoryService::request(
    SleepDayId start, SleepDayId end, uint32_t generation, uint32_t now_ms,
    uint32_t delay_ms, int32_t timezone_offset_minutes,
    const As11ClockTransform &clock) {
    if (status_.active()) return OperationAdmission::Busy;
    if (!read_ || !write_ || !scan_ || !start.valid() || !end.valid() ||
        end < start || generation == 0 || start.epoch_days() < 0 ||
        end.epoch_days() > INT16_MAX) {
        return OperationAdmission::Rejected;
    }

    LargeObject::destroy(runtime_);
    runtime_ = LargeObject::create<Runtime>();
    if (!runtime_) return OperationAdmission::Rejected;

    Runtime &work = *runtime_;
    work.edf.begin(*read_, *scan_);
    work.clock = clock;
    work.timezone_minutes = timezone_offset_minutes;
    work.start_at_ms = now_ms + delay_ms;
    const int64_t correction = clock.externally_referenced
        ? clock.device_minus_utc_ms : 0;
    const int64_t begin = (static_cast<int64_t>(start.epoch_days()) * 24 + 12) *
        3600000 - static_cast<int64_t>(timezone_offset_minutes) * 60000;
    const int64_t finish = (static_cast<int64_t>(end.epoch_days()) * 24 + 36) *
        3600000 - static_cast<int64_t>(timezone_offset_minutes) * 60000;
    work.fetched.set_window(begin - DAY_MS + correction, finish + correction);

    status_ = {};
    status_.generation = generation;
    status_.start_day = start;
    status_.end_day = end;
    status_.current_day = start;
    status_.phase = AirMiniHistoryPhase::Waiting;
    return OperationAdmission::Accepted;
}

void AirMiniHistoryService::cancel_rpc() {
    if (!runtime_ || !runtime_->rpc_ticket.valid()) return;

    (void)rpc_.cancel(runtime_->rpc_ticket);
    RpcRequestCompletion ignored;
    (void)rpc_.take_completion(runtime_->rpc_ticket, ignored);
    runtime_->rpc_ticket = {};
}

void AirMiniHistoryService::finish(const char *error) {
    if (error && error[0]) copy_cstr(status_.error, sizeof(status_.error), error);
    status_.phase = AirMiniHistoryPhase::Finishing;
    cancel_rpc();
    if (runtime_) runtime_->edf.cancel();
    poll_finish();
}

void AirMiniHistoryService::poll_finish() {
    if (runtime_) {
        if (runtime_->read_ticket.valid()) {
            if (!read_->abandon(runtime_->read_ticket)) return;
            runtime_->read_ticket = {};
        }
        if (runtime_->prepared.valid()) {
            read_->release_prepared(runtime_->prepared);
            runtime_->prepared = {};
        }
        if (runtime_->write_ticket.valid()) {
            if (!write_->abandon(runtime_->write_ticket)) return;
            runtime_->write_ticket = {};
        }
        if (runtime_->edf.status().active()) {
            runtime_->edf.poll();
            if (runtime_->edf.status().active()) return;
        }
        runtime_->prepared = {};
        runtime_->notifications.clear();
    }
    status_.phase = status_.error[0]
        ? AirMiniHistoryPhase::Failed : AirMiniHistoryPhase::Complete;
    LargeObject::destroy(runtime_);
    runtime_ = nullptr;
}

void AirMiniHistoryService::cancel(const char *reason) {
    if (status_.active()) finish(reason);
}

void AirMiniHistoryService::enqueue_notification(const RpcPayloadRef &payload) {
    if (!runtime_ || (status_.phase != AirMiniHistoryPhase::Settings &&
                      status_.phase != AirMiniHistoryPhase::LoggedData)) return;
    if (!runtime_->notifications.push(payload)) runtime_->overflow = true;
}

void AirMiniHistoryService::start_transfer(uint32_t now_ms) {
    Runtime &work = *runtime_;
    RpcRequestCommand command;
    command.method = work.settings ? "GetHistory" : "GetLoggedData";
    if (work.settings) {
        command.params_json =
            "{\"Settings\":{\"fromDateTime\":\"2008-01-01T00:00:00.000Z\"}}";
    } else {
        const int64_t correction = work.clock.externally_referenced
            ? work.clock.device_minus_utc_ms : 0;
        const int64_t start =
            (static_cast<int64_t>(status_.start_day.epoch_days()) * 24 + 12) *
            3600000 - static_cast<int64_t>(work.timezone_minutes) * 60000 -
            DAY_MS + correction;
        const std::string from = utc_text(start);
        command.params_json = "[";
        for (AirMiniHistorySelector selector : SELECTORS) {
            if (command.params_json.size() > 1) command.params_json += ',';
            command.params_json += "{\"dataId\":\"";
            command.params_json += airmini_history_selector_name(selector);
            command.params_json += "\",\"fromTime\":\"";
            command.params_json += from;
            command.params_json += "\"}";
        }
        command.params_json += ']';
    }
    command.source = RpcSource::EdfRecorder;
    command.timeout_ms = 12000;
    command.generation = ++work.rpc_generation;
    if (!command.generation) command.generation = ++work.rpc_generation;

    const OperationSubmission submitted = rpc_.request(command);
    if (submitted.admission == OperationAdmission::Busy) return;
    if (!submitted.accepted()) {
        finish("history_rpc_rejected");
        return;
    }

    work.rpc_ticket = submitted.ticket;
    work.response_received = false;
    work.deadline_ms = now_ms + TRANSFER_TIMEOUT_MS;
    const bool started = work.settings
        ? work.fetched.begin_settings_history(submitted.ticket.id)
        : work.fetched.begin_logged_data(submitted.ticket.id, SELECTORS,
                                         sizeof(SELECTORS) / sizeof(SELECTORS[0]));
    if (!started) {
        finish("history_decoder_busy");
        return;
    }
    status_.phase = work.settings ? AirMiniHistoryPhase::Settings
                                  : AirMiniHistoryPhase::LoggedData;
}

void AirMiniHistoryService::poll_transfer(uint32_t now_ms) {
    Runtime &work = *runtime_;
    const char *failure = nullptr;
    if (work.overflow) failure = "history_notification_overflow";

    if (work.rpc_ticket.valid()) {
        RpcRequestCompletion completion;
        if (rpc_.take_completion(work.rpc_ticket, completion)) {
            work.rpc_ticket = {};
            if (completion.cause != RpcCompletionCause::Response ||
                completion.response_error) {
                failure = "history_rpc_failed";
            } else {
                const RpcPayloadView payload = rpc_payload_view(completion.payload);
                const auto decoded = work.fetched.consume_response(
                    payload.data(), payload.size());
                if (!decoded.ok) {
                    failure = airmini_history_decode_error_name(decoded.error);
                } else {
                    work.response_received = true;
                }
            }
        }
    }

    RpcPayloadRef notification;
    if (!failure && work.response_received &&
        work.notifications.pop(notification)) {
        const RpcPayloadView payload = rpc_payload_view(notification);
        const auto decoded = work.fetched.consume_notification(
            payload.data(), payload.size());
        if (!decoded.ok && decoded.error !=
                               AirMiniHistoryDecodeError::StreamIdMismatch) {
            failure = airmini_history_decode_error_name(decoded.error);
        } else if (decoded.ok) {
            work.deadline_ms = now_ms + TRANSFER_TIMEOUT_MS;
        }
    }

    const bool complete = work.response_received &&
        !(work.settings ? work.fetched.settings_transfer_active()
                         : work.fetched.logged_transfer_active());
    if (complete &&
        (work.settings
            ? work.fetched.settings().state == AirMiniHistoryTransferState::Rejected
            : work.fetched.rejected_mask() != 0)) {
        failure = "history_selector_rejected";
    }
    if (!complete && static_cast<int32_t>(now_ms - work.deadline_ms) >= 0) {
        failure = "history_transfer_timeout";
    }
    if (failure) {
        cancel_rpc();
        if (!work.partial.merge_from(work.fetched)) {
            finish("history_partial_merge_failed");
            return;
        }
        work.notifications.clear();
        work.overflow = false;
        if (++work.attempts < 3) {
            status_.phase = AirMiniHistoryPhase::Waiting;
            work.start_at_ms = now_ms + 1000;
            return;
        }
        work.transfer_failed = true;
        copy_cstr(status_.error, sizeof(status_.error), failure);
    }
    if (!complete && !failure) return;

    if (work.settings) {
        if (!work.partial.merge_from(work.fetched)) {
            finish("history_partial_merge_failed");
            return;
        }
        work.fetched = std::move(work.partial);
        work.partial.clear();
        work.attempts = 0;
        work.settings = false;
        status_.phase = AirMiniHistoryPhase::Waiting;
        work.start_at_ms = now_ms;
        return;
    }

    if (!work.partial.merge_from(work.fetched)) {
        finish("history_partial_merge_failed");
        return;
    }
    work.fetched = std::move(work.partial);
    work.partial.clear();
    status_.transfer_complete = !work.transfer_failed;
    start_day();
}

void AirMiniHistoryService::start_day() {
    Runtime &work = *runtime_;
    char day[9] = {};
    (void)status_.current_day.format_yyyymmdd(day, sizeof(day));
    snprintf(work.path, sizeof(work.path),
             "/aircannect/airmini/history/%s.json", day);
    work.saved.clear();
    work.seed_valid = false;
    work.cache.reset();
    work.publication.reset();
    work.read_offset = 0;
    work.read_kind = Runtime::ReadKind::History;
    work.day_start_ms =
        (static_cast<int64_t>(status_.current_day.epoch_days()) * 24 + 12) *
        3600000 - static_cast<int64_t>(work.timezone_minutes) * 60000;
    work.day_end_ms = work.day_start_ms + DAY_MS;
    work.edf.reset();
    status_.phase = AirMiniHistoryPhase::ReadingSaved;
}

void AirMiniHistoryService::poll_day() {
    Runtime &work = *runtime_;
    if (status_.phase == AirMiniHistoryPhase::ReadingSaved) {
        if (!work.read_ticket.valid() && !work.prepared.valid()) {
            StorageReadCommand command;
            command.path = work.read_kind == Runtime::ReadKind::History
                ? work.path : "/STR.edf";
            command.offset = work.read_kind == Runtime::ReadKind::StrRecord
                ? work.str_offset : work.read_offset;
            command.length = work.read_kind == Runtime::ReadKind::History
                ? READ_BYTES : (work.read_kind == Runtime::ReadKind::StrHeader
                    ? edf_str_header_size() : edf_str_record_size());
            command.generation = status_.generation;
            command.lane = StorageReadLane::Maintenance;
            const auto submitted = read_->request_read(command);
            if (submitted.admission == OperationAdmission::Busy) return;
            if (!submitted.accepted()) {
                finish("history_read_rejected");
                return;
            }
            work.read_ticket = submitted.ticket;
            return;
        }

        StorageReadCompletion completion;
        if (work.read_ticket.valid()) {
            if (!read_->take_completion(work.read_ticket, completion)) return;
            work.read_ticket = {};
            if (completion.outcome.disposition != OperationDisposition::Succeeded) {
                if (completion.prepared.valid()) read_->release_prepared(completion.prepared);
                if (strcmp(completion.error, "read_not_found") != 0) {
                    finish(completion.error);
                    return;
                }
                if (work.read_kind == Runtime::ReadKind::History) {
                    work.read_kind = Runtime::ReadKind::StrHeader;
                    work.read_offset = 0;
                    return;
                }
                status_.phase = AirMiniHistoryPhase::ReadingEdf;
                return;
            }
            work.prepared = completion.prepared;
            if (work.read_kind == Runtime::ReadKind::History && !work.cache) {
                if (completion.file_size > SIZE_MAX) {
                    finish("history_file_too_large");
                    return;
                }
                work.cache = LargeByteBuffer::allocate(
                    static_cast<size_t>(completion.file_size));
                if (!work.cache) {
                    finish("history_allocation_failed");
                    return;
                }
            }
        }

        const auto view = read_->view_prepared(work.prepared);
        if (view.state == PreparedByteReadState::Retry) return;
        if (!view.valid()) {
            finish("history_read_incomplete");
            return;
        }
        const auto release = [&]() {
            read_->release_prepared(work.prepared);
            work.prepared = {};
        };
        if (work.read_kind == Runtime::ReadKind::History) {
            if (!view.length || view.length > work.cache->size() - work.read_offset) {
                release();
                finish("history_read_length");
                return;
            }
            memcpy(work.cache->data() + work.read_offset, view.data, view.length);
            work.read_offset += view.length;
            release();
            if (work.read_offset < work.cache->size()) return;
            if (!work.saved.deserialize(
                    reinterpret_cast<const char *>(work.cache->data()),
                    work.cache->size())) {
                finish("history_saved_invalid");
                return;
            }
            work.cache.reset();
            work.read_offset = 0;
            work.read_kind = Runtime::ReadKind::StrHeader;
            return;
        }

        if (work.read_kind == Runtime::ReadKind::StrHeader) {
            EdfHeaderSummary header;
            int64_t start_ms = 0;
            if (!edf_parse_header_summary(view.data, view.length, header) ||
                header.header_size != edf_str_header_size() ||
                header.record_size != edf_str_record_size() ||
                !edf_parse_header_start_ms(header, start_ms)) {
                release();
                finish("history_str_header_invalid");
                return;
            }
            const int64_t index = status_.current_day.epoch_days() - start_ms / DAY_MS;
            release();
            if (index >= 0 && index < header.record_count) {
                work.str_offset = edf_str_record_offset(static_cast<uint32_t>(index));
                work.read_kind = Runtime::ReadKind::StrRecord;
                return;
            }
        } else {
            if (view.length != edf_str_record_size()) {
                release();
                finish("history_str_record_incomplete");
                return;
            }
            const int16_t duration = edf_read_i16_le_sample(view.data,
                edf_str_signal_sample_offset(AC_EDF_STR_DURATION_SIGNAL));
            if (duration >= 0) {
                work.seed_valid = work.seed.restore_record(view.data, view.length) &&
                    work.seed.day_epoch_days() == status_.current_day.epoch_days();
                if (!work.seed_valid) {
                    release();
                    finish("history_str_record_invalid");
                    return;
                }
            }
            release();
        }
        status_.phase = AirMiniHistoryPhase::ReadingEdf;
        return;
    }

    if (status_.phase == AirMiniHistoryPhase::ReadingEdf) {
        if (work.edf.status().state == EdfDayStatisticsState::Idle) {
            if (work.edf.start(status_.current_day, work.timezone_minutes) !=
                OperationAdmission::Accepted) {
                finish("history_edf_start_failed");
            }
            return;
        }
        work.edf.poll();
        if (!work.edf.status().terminal()) return;
        if (work.edf.status().state != EdfDayStatisticsState::Complete) {
            finish(work.edf.status().error_text);
            return;
        }

        AirMiniHistoryData day(history_limits());
        const int64_t correction = work.clock.externally_referenced
            ? work.clock.device_minus_utc_ms : 0;
        if (!day.copy_window_from(work.fetched, work.day_start_ms + correction,
                                   work.day_end_ms + correction) ||
            !work.saved.merge_from(day)) {
            finish("history_merge_failed");
            return;
        }
        LargeTextBuffer text;
        if (!work.saved.serialize(text)) {
            finish("history_serialize_failed");
            return;
        }
        work.publication = LargeByteBuffer::copy_and_freeze(
            text.c_str(), text.length());
        if (!work.publication) {
            finish("history_allocation_failed");
            return;
        }
        status_.phase = AirMiniHistoryPhase::Saving;
        return;
    }

    if (status_.phase == AirMiniHistoryPhase::Saving) {
        if (!work.write_ticket.valid()) {
            StorageAtomicWriteCommand command;
            command.path = work.path;
            command.bytes = work.publication;
            command.generation = status_.generation;
            const auto submitted = write_->request_write(command);
            if (submitted.admission == OperationAdmission::Busy) return;
            if (!submitted.accepted()) {
                finish("history_write_rejected");
                return;
            }
            work.write_ticket = submitted.ticket;
            return;
        }
        StorageAtomicWriteCompletion completion;
        if (!write_->take_completion(work.write_ticket, completion)) return;
        work.write_ticket = {};
        work.publication.reset();
        if (completion.outcome.disposition != OperationDisposition::Succeeded) {
            finish(completion.error);
            return;
        }

        work.projection_input = {};
        AirMiniHistoryProjectionInput &input = work.projection_input;
        input.day_start_ms = work.day_start_ms;
        input.day_end_ms = work.day_end_ms;
        input.timezone_offset_minutes = work.timezone_minutes;
        input.clock = work.clock;
        input.clock_window_start_ms = work.day_start_ms;
        input.clock_window_end_ms = work.day_end_ms;
        input.day = status_.current_day;
        input.local_str = work.seed_valid ? &work.seed : nullptr;
        input.local_str_has_session = work.seed_valid;
        const char *error = nullptr;
        if (!airmini_history_prepare_str(work.saved, input, work.edf,
                                         work.projection, error)) {
            finish(error ? error : "history_str_projection_failed");
            return;
        }
        status_.phase = AirMiniHistoryPhase::Finalizing;
        return;
    }

    if (status_.phase == AirMiniHistoryPhase::Finalizing) {
        work.edf.poll();
        if (!work.edf.status().terminal()) return;
        if (work.edf.status().state != EdfDayStatisticsState::Complete) {
            finish(work.edf.status().error_text);
            return;
        }

        const char *error = nullptr;
        if (!airmini_history_apply_str(work.saved, work.projection_input,
                                       work.edf, work.projection,
                                       work.output, error)) {
            finish(error ? error : "history_str_projection_failed");
            return;
        }
        if (!work.output.active()) {
            ++status_.empty;
            record_published();
            return;
        }
        status_.phase = AirMiniHistoryPhase::RecordReady;
    }
}

void AirMiniHistoryService::poll(uint32_t now_ms, bool rpc_available,
                                bool therapy_active) {
    if (!runtime_ || !status_.active()) return;
    if (status_.phase == AirMiniHistoryPhase::Finishing) {
        poll_finish();
        return;
    }
    if (therapy_active) {
        finish("history_therapy_started");
        return;
    }
    try {
        if (status_.phase == AirMiniHistoryPhase::Waiting) {
            if (static_cast<int32_t>(now_ms - runtime_->start_at_ms) < 0) return;
            if (!rpc_available) {
                // Local EDF and previously saved history remain usable offline.
                runtime_->transfer_failed = true;
                copy_cstr(status_.error, sizeof(status_.error), "history_rpc_unavailable");
                start_day();
            } else {
                if (static_cast<int32_t>(now_ms - runtime_->start_at_ms) >=
                    static_cast<int32_t>(TRANSFER_TIMEOUT_MS)) {
                    finish("history_rpc_busy_timeout");
                    return;
                }
                start_transfer(now_ms);
            }
        } else if (status_.phase == AirMiniHistoryPhase::Settings ||
                   status_.phase == AirMiniHistoryPhase::LoggedData) {
            poll_transfer(now_ms);
        } else {
            poll_day();
        }
    } catch (const std::bad_alloc &) {
        finish("history_allocation_failed");
    }
}

const EdfStrSessionAccumulator *AirMiniHistoryService::record() const {
    return runtime_ && status_.phase == AirMiniHistoryPhase::RecordReady
        ? &runtime_->output : nullptr;
}

void AirMiniHistoryService::record_published() {
    if (!runtime_) return;
    if (status_.phase == AirMiniHistoryPhase::RecordReady) ++status_.records_queued;
    if (status_.current_day == status_.end_day) {
        finish(nullptr);
        return;
    }
    (void)SleepDayId::from_epoch_days(status_.current_day.epoch_days() + 1,
                                     status_.current_day);
    start_day();
}

}  // namespace aircannect
