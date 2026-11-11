#include "report_session_statistics.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include "little_endian.h"
#include "large_object.h"
#include "string_util.h"

namespace aircannect {
using namespace LittleEndian;

bool ReportSessionStatistics::account_current() {
    if (current_ == SIZE_MAX) return true;

    auto &part = parts_[current_];
    size_t bytes = 2048; // Accumulator state and allocation overhead.
    for (size_t i = 0; i < ReportCalculatedMetrics::Count; ++i) {
        ReportMetricAccumulator::Histogram histogram;
        if (part.metrics->histogram(i, histogram)) bytes += histogram.bin_count * sizeof(uint32_t);
    }
    working_bytes_ += bytes - part.bytes;
    part.bytes = bytes;
    return working_bytes_ <= Memory::buffer_budget(MaxBytes, 64 * 1024) || pack_parts();
}

bool ReportSessionStatistics::pack_parts() {
    // The wire format pools repeated session/cadence contributions. Retire
    // dense working histograms without restoring them when that session recurs.
    for (auto &part : parts_) {
        if (!part.metrics) continue;

        auto packed = part.metrics->snapshot();
        if (!packed || packed_bytes_ > MaxBytes - EntryBytes ||
            packed->size() > MaxBytes - packed_bytes_ - EntryBytes) return false;

        packed_bytes_ += EntryBytes + packed->size();
        part.packed = std::move(packed);
        part.metrics.reset();
        part.bytes = 0;
    }

    working_bytes_ = 0;
    current_ = SIZE_MAX;
    return true;
}

bool ReportSessionStatistics::accept(uint16_t session, uint32_t interval_ms,
    ReportSignalId signal, int32_t value_milli, bool expiration, bool closed) {
    if (!interval_ms) return true;

    if (current_ == SIZE_MAX || parts_[current_].session != session ||
        parts_[current_].interval_ms != interval_ms || parts_[current_].closed != closed ||
        signal_ != signal) {
        if (!account_current()) return false;
        size_t index = 0;
        for (; index < parts_.size(); ++index) {
            if (parts_[index].metrics && parts_[index].session == session &&
                parts_[index].interval_ms == interval_ms && parts_[index].closed == closed) break;
        }
        if (index == parts_.size()) {
            auto metrics = std::unique_ptr<ReportMetricAccumulator>(
                new (std::nothrow) ReportMetricAccumulator);

            if (!metrics || !metrics->begin(UINT32_MAX)) return false;
            try {
                parts_.push_back({session, interval_ms, std::move(metrics), 0, {}, closed});
            } catch (const std::bad_alloc &) { return false; }
        }
        current_ = index;
        signal_ = signal;
    }

    auto &metrics = *parts_[current_].metrics;
    return expiration ? metrics.accept_expiration_time(value_milli)
        : metrics.accept(signal, value_milli, interval_ms);
}

bool ReportSessionStatistics::resume(StorageWritePayload previous,
    uint32_t retained_signals, bool close_retained_tail) {
    clear();
    StorageWritePayload::Cursor cursor;
    cursor.offset = entries_offset(*previous.bytes);
    while (previous.span(cursor)) {
        Entry entry;
        if (!next(previous, cursor, entry)) return false;
        if (!entry.closed && !retained_signals) continue;

        std::shared_ptr<const LargeByteBuffer> packed;
        if (entry.closed) {
            packed = LargeByteBuffer::slice(entry.owner,
                entry.metrics - entry.owner->data(), entry.size);
        } else {
            ReportMetricAccumulator retained;
            if (!retained.restore(entry.metrics, entry.size)) return false;
            retained.retain_signals(retained_signals);
            packed = retained.snapshot();
        }
        if (!packed) return false;

        const size_t size = packed->size();
        if (packed_bytes_ > MaxBytes - EntryBytes ||
            size > MaxBytes - packed_bytes_ - EntryBytes) return false;

        try {
            parts_.push_back({entry.session, entry.interval_ms, {}, 0, std::move(packed),
                              entry.closed || close_retained_tail});
        } catch (const std::bad_alloc &) { return false; }
        packed_bytes_ += EntryBytes + size;
    }
    return true;
}

StorageWritePayload ReportSessionStatistics::finish(
    const ReportSignalStoreNight &night, int64_t closed_before_ms) {
    if (!pack_parts()) return {};

    // Keep payload owners; only the file and contribution headers are new.
    auto headers = LargeByteBuffer::allocate(HeaderBytes + parts_.size() * EntryBytes);
    auto buffers = std::shared_ptr<StorageWriteBuffers>(
        LargeObject::create<StorageWriteBuffers>(), LargeObject::destroy<StorageWriteBuffers>);
    if (!headers || !buffers || !buffers->allocate(parts_.size() * 2)) return {};

    auto *p = headers->data();
    memset(p, 0, HeaderBytes);
    put_le16(p, 2);
    put_le16(p + 2, HeaderBytes);
    put_le32(p + 4, packed_bytes_);
    put_le32(p + 8, night.sleep_day.epoch_days());
    put_le32(p + 12, night.generation);
    put_le64(p + 16, night.source_revision.value());
    put_le16(p + 24, night.session_count);
    p[26] = night.checkpoint_slot;
    put_le64(p + 32, static_cast<uint64_t>(closed_before_ms));
    p += HeaderBytes;

    for (size_t i = 0; i < parts_.size(); ++i) {
        const auto &part = parts_[i];
        const auto &metrics = part.packed;
        memset(p, 0, EntryBytes);
        put_le16(p, part.session);
        p[2] = part.closed ? 1 : 0;
        put_le32(p + 4, part.interval_ms);
        put_le32(p + 8, metrics->size());
        p += EntryBytes;
    }

    const auto header_owner = LargeByteBuffer::freeze(std::move(headers));
    StorageWritePayload output;
    output.bytes = LargeByteBuffer::slice(header_owner, 0, HeaderBytes);
    if (!output.bytes) return {};

    for (size_t i = 0; i < parts_.size(); ++i) {
        auto entry = LargeByteBuffer::slice(header_owner, HeaderBytes + i * EntryBytes, EntryBytes);
        if (!entry) return {};

        *buffers->append() = std::move(entry);
        *buffers->append() = std::move(parts_[i].packed);
    }
    output.buffers = std::move(buffers);
    clear();
    return output;
}

void ReportSessionStatistics::clear() {
    decltype(parts_){}.swap(parts_);
    current_ = SIZE_MAX;
    working_bytes_ = 0;
    packed_bytes_ = HeaderBytes;
}

namespace {
bool matches_header(const LargeByteBuffer &bytes, size_t length,
                    const ReportSignalStoreNight &night) {
    if (bytes.size() < 32 || length > ReportSessionStatistics::MaxBytes) return false;
    const auto *p = bytes.data();
    const auto version = get_le16(p);
    const size_t header = version == 1 ? 32 : ReportSessionStatistics::HeaderBytes;
    return (version == 1 || version == 2) && bytes.size() >= header &&
        get_le16(p + 2) == header &&
        get_le32(p + 4) == length &&
        get_le32(p + 8) == uint32_t(night.sleep_day.epoch_days()) &&
        get_le32(p + 12) == night.generation &&
        get_le64(p + 16) == night.source_revision.value() &&
        get_le16(p + 24) == night.session_count && p[26] == night.checkpoint_slot;
}
} // namespace

bool ReportSessionStatistics::matches(const LargeByteBuffer &bytes,
                                      const ReportSignalStoreNight &night) {
    return matches_header(bytes, bytes.size(), night);
}

bool ReportSessionStatistics::matches(const StorageWritePayload &bytes,
                                      const ReportSignalStoreNight &night) {
    return bytes.bytes && matches_header(*bytes.bytes, bytes.size(), night);
}

bool ReportSessionStatistics::resumable(const LargeByteBuffer &bytes,
    const ReportSignalStoreNight &night, int64_t closed_before_ms) {
    return matches(bytes, night) && get_le16(bytes.data()) == 2 &&
        static_cast<int64_t>(get_le64(bytes.data() + 32)) == closed_before_ms;
}

size_t ReportSessionStatistics::entries_offset(const LargeByteBuffer &bytes) {
    return get_le16(bytes.data() + 2);
}

bool ReportSessionStatistics::next(const LargeByteBuffer &bytes, size_t &offset,
                                  Entry &entry) {
    if (offset > bytes.size() || bytes.size() - offset < EntryBytes) return false;
    const auto *p = bytes.data() + offset;
    entry.session = get_le16(p);
    entry.interval_ms = get_le32(p + 4);
    entry.size = get_le32(p + 8);
    entry.closed = get_le16(bytes.data()) == 2 && p[2] == 1;
    if (entry.session >= get_le16(bytes.data() + 24) || !entry.interval_ms ||
        (get_le16(bytes.data()) == 2 && p[2] > 1) ||
        entry.interval_ms > REPORT_SIGNAL_STORE_BLOCK_MS ||
        entry.size > bytes.size() - offset - EntryBytes) return false;

    entry.metrics = p + EntryBytes;
    offset += EntryBytes + entry.size;
    return true;
}

bool ReportSessionStatistics::next(const StorageWritePayload &bytes,
    StorageWritePayload::Cursor &cursor, Entry &entry) {
    if (!bytes.buffers) {
        if (!next(*bytes.bytes, cursor.offset, entry)) return false;
        entry.owner = bytes.bytes;
        return true;
    }

    // Only finish() produces segmented data; loaded files use the decoder above.
    const auto &header = bytes.span(cursor);
    if (!header || header->size() - cursor.offset < EntryBytes) return false;

    const auto *p = header->data() + cursor.offset;
    entry.session = get_le16(p);
    entry.closed = p[2] == 1;
    entry.interval_ms = get_le32(p + 4);
    entry.size = get_le32(p + 8);
    cursor.offset += EntryBytes;

    entry.owner = bytes.span(cursor);
    if (!entry.owner || entry.size > entry.owner->size() - cursor.offset) return false;

    entry.metrics = entry.owner->data() + cursor.offset;
    cursor.offset += entry.size;
    return true;
}

bool ReportSessionStatistics::path(const ReportSignalStoreNight &night,
                                   char *out, size_t size) {
    char directory[AC_STORAGE_PATH_MAX];
    if (!report_signal_store_generation_path(night.sleep_day, night.generation,
                                              directory, sizeof(directory))) return false;
    const int count = snprintf(out, size, "%s/sessions.%u.bin", directory,
                               unsigned(night.checkpoint_slot));
    return count > 0 && size_t(count) < size;
}

struct ReportSessionStatisticsReader::Runtime {
    StorageReadPort &port;
    ReportSignalStoreMetadata metadata;
    std::vector<NightCatalogTimeRange, LargeAllocator<NightCatalogTimeRange>> sessions;
    ReportSessionStatistics statistics;
    ReportSignalStoreTrack track;
    ReportSignalStoreTrack ratio;
    size_t track_index = 0;
    size_t slot = 0;
    size_t sample = 0;
    size_t session = 0;
    int64_t closed_before_ms = 0;
    bool paired = false;
    OperationTicket ticket;
    StoragePreparedRead prepared;
    std::shared_ptr<const LargeByteBuffer> raw;
    std::shared_ptr<const LargeByteBuffer> ratio_raw;
    StorageWritePayload completed;
    size_t expected = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
    enum class Phase { Select, Read, Wait, Samples, Done } phase = Phase::Done;
    bool reading_ratio = false;

    explicit Runtime(StorageReadPort &owner) : port(owner) {}
    ~Runtime() { release(); }

    void release() {
        if (ticket.valid()) port.abandon(ticket);
        ticket = {};
        if (prepared.valid()) port.release_prepared(prepared);
        prepared = {};
        port.release_handle();
    }

    void fail(const char *reason) {
        copy_cstr(error, sizeof(error), reason);
        phase = Phase::Done;
        release();
        raw.reset();
        ratio_raw.reset();
        statistics.clear();
    }

    int64_t block_start() const {
        return track.first_block_start_ms + slot * REPORT_SIGNAL_STORE_BLOCK_MS;
    }

    static int64_t first_sample(const ReportSignalStoreTrack &t, int64_t block) {
        const int64_t remainder = (block - t.grid_phase_ms) % t.sample_interval_ms;
        return block + (remainder ? t.sample_interval_ms - remainder : 0);
    }

    bool select() {
        if (track_index == metadata.view.night.track_count) {
            completed = statistics.finish(metadata.view.night, closed_before_ms);
            if (!completed) fail("session_statistics_alloc");
            phase = Phase::Done;
            port.release_handle();
            return true;
        }

        metadata.view.track(track_index, track);
        if (!report_signal_has_metric_consumer(track.signal) ||
            !track.valid_sample_count || slot == track.block_slot_count) {
            ++track_index;
            slot = session = 0;
            port.release_handle();
            return true;
        }

        ReportSignalStorePlaneRange range;
        if (!ReportSignalStoreFileCodec::plane_range(track, block_start(), 1,
                ReportSignalStoreLevel::Raw, range) || !range.length) {
            ++slot;
            return true;
        }

        paired = false;
        if (track.signal == ReportSignalId::InspiratoryDuration) {
            for (size_t i = 0; i < metadata.view.night.track_count; ++i) {
                metadata.view.track(i, ratio);
                if (ratio.signal != ReportSignalId::IeRatio || !ratio.valid_sample_count ||
                    ratio.sample_interval_ms != track.sample_interval_ms ||
                    ratio.grid_phase_ms != track.grid_phase_ms) continue;
                if (ReportSignalStoreFileCodec::plane_range(ratio, block_start(), 1,
                        ReportSignalStoreLevel::Raw, range) && range.length) { paired = true; break; }
            }
        }

        raw.reset();
        ratio_raw.reset();
        reading_ratio = false;
        phase = Phase::Read;
        return true;
    }

    bool read() {
        const auto &source = reading_ratio ? ratio : track;
        char path[AC_STORAGE_PATH_MAX];
        ReportSignalStorePlaneRange range;
        if (!report_signal_store_signal_path(source, ReportSignalStoreLevel::Raw, path, sizeof(path)) ||
            !ReportSignalStoreFileCodec::plane_range(source, block_start(), 1,
                ReportSignalStoreLevel::Raw, range)) {
            fail("session_statistics_range");
            return true;
        }

        StorageReadCommand command;
        command.path = path;
        command.offset = range.offset;
        command.length = range.length;
        command.generation = metadata.view.night.generation;
        command.lane = StorageReadLane::Report;
        command.retain_handle = true;
        const auto submitted = port.request_read(command);
        if (submitted.admission == OperationAdmission::Busy) return false;
        if (!submitted.accepted()) { fail("session_statistics_read_rejected"); return true; }

        expected = range.length;
        ticket = submitted.ticket;
        phase = Phase::Wait;
        return true;
    }

    bool receive() {
        if (ticket.valid()) {
            StorageReadCompletion completion;
            if (!port.take_completion(ticket, completion)) return false;
            ticket = {};
            prepared = completion.prepared;
            if (completion.outcome.disposition != OperationDisposition::Succeeded ||
                prepared.length != expected) {
                fail(completion.error[0] ? completion.error : "session_statistics_samples_unavailable");
                return true;
            }
        }

        const auto view = port.view_prepared(prepared);
        if (view.state == PreparedByteReadState::Retry) return false;
        if (!view.valid() || view.length != expected) {
            fail("session_statistics_short_read");
            return true;
        }

        auto bytes = LargeByteBuffer::copy_and_freeze(view.data, view.length);
        port.release_prepared(prepared);
        prepared = {};
        if (!bytes) { fail("session_statistics_alloc"); return true; }
        if (reading_ratio) ratio_raw = std::move(bytes);
        else raw = std::move(bytes);

        if (paired && !reading_ratio) {
            reading_ratio = true;
            phase = Phase::Read;
        } else {
            sample = 0;
            phase = Phase::Samples;
        }
        return true;
    }

    bool samples() {
        const size_t end = std::min(raw->size() / 2, sample + 512);
        const int64_t first = first_sample(track, block_start());
        for (; sample < end; ++sample) {
            const int64_t timestamp = first + sample * track.sample_interval_ms;
            while (session < sessions.size() && timestamp >= sessions[session].end_ms) ++session;
            if (session == sessions.size()) break;
            if (timestamp < sessions[session].start_ms) continue;

            const int16_t digital = static_cast<int16_t>(get_le16(raw->data() + sample * 2));
            if (digital == track.missing_value) continue;
            int32_t value = 0;
            if (!ReportSignalStoreFileCodec::physical_value(track, digital, value)) {
                fail("session_statistics_sample_invalid");
                return true;
            }

            const uint32_t duration = std::min<int64_t>(track.sample_interval_ms,
                sessions[session].end_ms - timestamp);
            if (!statistics.accept(session, duration, track.signal, value, false,
                                   timestamp < closed_before_ms)) {
                fail("session_statistics_alloc");
                return true;
            }

            if (ratio_raw && value > 0 && sample * 2 + 2 <= ratio_raw->size()) {
                const int16_t digital_ratio = static_cast<int16_t>(get_le16(ratio_raw->data() + sample * 2));
                int32_t ratio_value = 0;
                if (digital_ratio == ratio.missing_value ||
                    !ReportSignalStoreFileCodec::physical_value(ratio, digital_ratio, ratio_value) ||
                    ratio_value <= 0) continue;
                const int64_t expiration = (int64_t(value) * 100000 + ratio_value / 2) / ratio_value;
                if (expiration <= INT32_MAX && !statistics.accept(session, duration,
                        ReportSignalId::InspiratoryDuration, expiration, true,
                        timestamp < closed_before_ms)) {
                    fail("session_statistics_alloc");
                    return true;
                }
            }
        }

        if (sample == raw->size() / 2 || session == sessions.size()) {
            raw.reset();
            ratio_raw.reset();
            ++slot;
            phase = Phase::Select;
        }
        return true;
    }
};

ReportSessionStatisticsReader::ReportSessionStatisticsReader(StorageReadPort &port)
    : runtime_(LargeObject::create<Runtime>(port)) {}

ReportSessionStatisticsReader::~ReportSessionStatisticsReader() { LargeObject::destroy(runtime_); }

bool ReportSessionStatisticsReader::start(ReportSignalStoreMetadata metadata, int64_t closed_before_ms) {
    if (!runtime_ || !metadata.metadata || active()) return false;
    auto &r = *runtime_;
    r.metadata = std::move(metadata);
    try {
        r.sessions.resize(r.metadata.view.night.session_count);
    } catch (const std::bad_alloc &) { return false; }
    for (size_t i = 0; i < r.sessions.size(); ++i) r.metadata.view.session(i, r.sessions[i]);
    r.closed_before_ms = closed_before_ms;
    if (!r.closed_before_ms) {
        for (const auto &session : r.sessions) {
            r.closed_before_ms = std::max(r.closed_before_ms,
                session.end_ms - session.end_ms % REPORT_SIGNAL_STORE_BLOCK_MS);
        }
    }

    r.phase = Runtime::Phase::Select;
    return true;
}

bool ReportSessionStatisticsReader::poll() {
    if (!runtime_) return false;
    switch (runtime_->phase) {
        case Runtime::Phase::Select: return runtime_->select();
        case Runtime::Phase::Read: return runtime_->read();
        case Runtime::Phase::Wait: return runtime_->receive();
        case Runtime::Phase::Samples: return runtime_->samples();
        case Runtime::Phase::Done: return false;
    }
    return false;
}

bool ReportSessionStatisticsReader::active() const {
    return runtime_ && runtime_->phase != Runtime::Phase::Done;
}

const char *ReportSessionStatisticsReader::error() const {
    return runtime_ ? runtime_->error : "session_statistics_alloc";
}

StorageWritePayload ReportSessionStatisticsReader::take_completed() {
    return runtime_ && !active() ? std::move(runtime_->completed) : StorageWritePayload{};
}

}  // namespace aircannect
