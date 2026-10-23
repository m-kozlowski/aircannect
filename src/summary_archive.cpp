#include "summary_archive.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <new>

#ifdef ARDUINO
#include <mbedtls/sha256.h>
#else
#include <openssl/sha.h>
#endif

#include "hex_util.h"
#include "large_allocator.h"
#include "large_json_allocator.h"
#include "little_endian.h"
#include "memory_manager.h"
#include "report_proto.h"
#include "report_spool_types.h"
#include "storage_bounded_file_loader.h"
#include "string_util.h"

namespace aircannect {
namespace {

using Digest = std::array<uint8_t, 32>;
template <typename K, typename V>
using ArchiveMap = std::map<K, V, std::less<K>,
    LargeAllocator<std::pair<const K, V>>>;
using DayMap = ArchiveMap<int32_t, Digest>;
struct Blob {
    uint64_t offset = 0;
    uint32_t size = 0;
};
using BlobMap = ArchiveMap<Digest, Blob>;

constexpr size_t HEADER = 32;
constexpr size_t HEAD = 24;
constexpr size_t SEGMENT_HEAD = HEAD + AC_STORAGE_ERROR_MAX;
constexpr size_t CHUNK = 16384;
constexpr size_t MAX_FRAME = AC_REPORT_MAX_PAYLOAD_BYTES * 2;
constexpr char FRAME_MAGIC[] = "ACSUM001";
constexpr char HEAD_MAGIC[] = "ACSUMHD1";
constexpr char SEGMENT_HEAD_MAGIC[] = "ACSUMHD2";
constexpr char ARCHIVE_ROOT[] = "/aircannect/as11/summary";

Digest digest(const void *data, size_t length) {
    Digest out{};
#ifdef ARDUINO
    mbedtls_sha256(static_cast<const uint8_t *>(data), length, out.data(), 0);
#else
    SHA256(static_cast<const unsigned char *>(data), length, out.data());
#endif
    return out;
}

std::string hex(const Digest &value) {
    char text[65];
    hex_encode(value.data(), value.size(), text, sizeof(text), HexCase::Lower);
    return text;
}

bool decode_digest(JsonVariantConst value, Digest &out) {
    const char *text = value.as<const char *>();
    size_t length = 0;
    return text && strlen(text) == 64 &&
        hex_decode(text, 64, out.data(), out.size(), length) && length == out.size();
}

Digest mapping_digest(const DayMap &mapping) {
    ReportSpoolBuffer bytes;
    constexpr size_t ROW_SIZE = 36;
    if (mapping.size() > bytes.max_size() / ROW_SIZE ||
        !bytes.reserve_capacity(mapping.size() * ROW_SIZE)) {
        throw std::bad_alloc();
    }

    for (const auto &entry : mapping) {
        uint8_t row[ROW_SIZE];
        LittleEndian::put_le32(row, static_cast<uint32_t>(entry.first));
        memcpy(row + 4, entry.second.data(), entry.second.size());
        if (!bytes.append(row, sizeof(row))) throw std::bad_alloc();
    }
    return digest(bytes.data(), bytes.size());
}

void encode_clock(JsonObject target, const SummaryClockObservation &clock) {
    target["utc_ms"] = clock.utc_ms;
    target["uptime_ms"] = clock.uptime_ms;
    target["utc_valid"] = clock.utc_valid;
    target["ntp_synced"] = clock.ntp_synced;
    target["clock_source"] = clock.clock_source;
    target["timezone"] = clock.timezone;
    target["timezone_offset_valid"] = clock.timezone_offset_valid;
    target["timezone_offset_min"] = clock.timezone_offset_min;
    target["device_time"] = clock.device_time;
    target["device_sample_age_ms"] = clock.device_sample_age_ms;
    target["device_offset_valid"] = clock.device_offset_valid;
    target["device_minus_utc_ms"] = clock.device_minus_utc_ms;
    target["device_timezone_valid"] = clock.device_timezone_valid;
    target["device_timezone_min"] = clock.device_timezone_min;
}

}  // namespace

struct SummaryArchive::Impl {
    enum class Phase {
        Idle, Discover, Head, CheckNew, Initialize, Header, Body, Prepare, Write, Commit, Failed
    };

    StorageReadPort *read = nullptr;
    StorageRangeWritePort *range = nullptr;
    StorageAtomicWritePort *atomic = nullptr;
    StorageScanPort *scan = nullptr;
    StorageBoundedFileLoader head_loader;
    OperationTicket ticket;
    StoragePreparedRead prepared;
    size_t prepared_used = 0;
    size_t requested_bytes = 0;
    Phase phase = Phase::Idle;
    SummaryArchiveStatus status;

    SummaryObservation observation;
    std::string serial;
    std::string base_path;
    std::string path;
    uint32_t segment = 0;
    char recovery_reason[AC_STORAGE_ERROR_MAX] = {};

    DayMap mapping;
    DayMap next;
    BlobMap blobs;
    BlobMap added;
    uint64_t end = 0;
    uint64_t sequence = 0;
    uint64_t cursor = 0;
    uint64_t replay_sequence = 0;
    bool loaded = false;

    uint32_t generation = 0;
    size_t frame_size = 0;
    size_t raw_size = 0;
    size_t progress = 0;
    ReportSpoolBuffer input;
    std::shared_ptr<const LargeByteBuffer> output;

    Impl() { input.set_max_size(MAX_FRAME); }

    ~Impl() { cancel(); }

    void cancel() {
        head_loader.reset();
        if (prepared.valid()) read->release_prepared(prepared);
        prepared = {};
        prepared_used = 0;
        if (ticket.valid()) {
            if (phase == Phase::Discover) scan->abandon(ticket);
            else if (phase == Phase::Write) range->abandon(ticket);
            else if (phase == Phase::Commit || phase == Phase::Initialize) atomic->abandon(ticket);
            else read->abandon(ticket);
        }
        ticket = {};
        if (read && (phase == Phase::Header || phase == Phase::Body)) read->release_handle();
    }

    bool fail(const char *error) {
        copy_cstr(status.error, sizeof(status.error), error);
        cancel();
        status.active = false;
        phase = Phase::Failed;
        loaded = false;
        observation = {};
        output.reset();
        input.clear();
        next.clear();
        added.clear();
        return true;
    }

    void forget() {
        cancel();
        phase = Phase::Idle;
        status.active = false;
        loaded = false;
        observation = {};
        input.clear();
        output.reset();
        mapping.clear();
        next.clear();
        blobs.clear();
        added.clear();
    }

    bool continue_segment(const char *reason) {
        if (segment == UINT32_MAX) return fail("summary_archive_segment_limit");

        cancel();
        input.clear();
        mapping.clear();
        blobs.clear();
        sequence = end = cursor = replay_sequence = 0;
        loaded = false;

        ++segment;
        path = base_path + "." + std::to_string(segment);
        copy_cstr(recovery_reason, sizeof(recovery_reason), reason);
        status.segment = segment;
        status.sequence = status.committed_bytes = 0;
        status.records = 0;
        copy_cstr(status.recovery_reason, sizeof(status.recovery_reason), reason);
        phase = Phase::Head;
        return true;
    }

    bool discover() {
        if (!ticket.valid()) {
            const StorageScanRoot root{ARCHIVE_ROOT, false};
            const StorageScanCommand command{&root, 1, false, generation};
            const auto submitted = scan->request_scan(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("summary_archive_scan_rejected");
            ticket = submitted.ticket;
            return true;
        }

        StorageScanCompletion completion;
        if (!scan->take_completion(ticket, completion)) return false;
        ticket = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded || !completion.snapshot)
            return fail("summary_archive_scan_failed");

        const std::string prefix = base_path + ".";
        for (size_t i = 0; i < completion.snapshot->size(); ++i) {
            StorageScanEntryView entry;
            if (!completion.snapshot->entry(i, entry) || entry.directory) continue;
            const std::string name = entry.path;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            const auto dot = name.rfind('.');
            const auto extension = name.substr(dot);
            if (dot < prefix.size() || (extension != ".head" && extension != ".journal")) continue;

            const auto number = name.substr(prefix.size(), dot - prefix.size());
            uint32_t candidate = 0;
            if (parse_uint32_decimal(number.c_str(), candidate) &&
                number == std::to_string(candidate)) segment = std::max(segment, candidate);
        }

        path = base_path + (segment ? "." + std::to_string(segment) : "");
        status.segment = segment;
        phase = Phase::Head;
        return true;
    }

    bool start(SummaryObservation value) {
        if (!read || status.active || (!value.payload && !value.empty_payload) ||
            value.device_serial.empty()) return false;
        if (value.payload && value.payload->size() > AC_REPORT_MAX_PAYLOAD_BYTES) return false;

        if (!loaded || serial != value.device_serial) {
            forget();
            serial = value.device_serial;
            base_path = std::string(ARCHIVE_ROOT) + "/" + hex(digest(serial.data(), serial.size()));
            segment = 0;
            recovery_reason[0] = '\0';
            sequence = end = cursor = replay_sequence = 0;
        }

        observation = std::move(value);
        status = {};
        status.active = true;
        status.sequence = sequence;
        status.committed_bytes = end;
        status.records = mapping.size();
        status.segment = segment;
        copy_cstr(status.recovery_reason, sizeof(status.recovery_reason), recovery_reason);
        if (++generation == 0) ++generation;
        phase = loaded ? Phase::Prepare : Phase::Discover;
        return true;
    }

    bool load_head() {
        const size_t head_size = segment ? SEGMENT_HEAD : HEAD;
        if (head_loader.status().state == StorageBoundedFileLoadState::Idle) {
            if (head_loader.start((path + ".head").c_str(), head_size, generation,
                                  StorageReadLane::Maintenance) != OperationAdmission::Accepted) return false;
        }
        const bool worked = head_loader.poll();
        if (head_loader.status().active()) return worked;
        const auto state = head_loader.status().state;
        if (state == StorageBoundedFileLoadState::Missing) {
            head_loader.reset();
            phase = Phase::CheckNew;
            return true;
        }
        if (state != StorageBoundedFileLoadState::Ready) {
            if (strcmp(head_loader.status().error, "storage_file_size_invalid") == 0)
                return continue_segment("summary_archive_head_invalid");
            return fail(head_loader.status().error);
        }

        const auto file_size = head_loader.status().file_size;
        auto bytes = head_loader.take_completed();
        head_loader.reset();
        if (!bytes || bytes->size() != head_size || file_size != head_size ||
            memcmp(bytes->data(), segment ? SEGMENT_HEAD_MAGIC : HEAD_MAGIC, 8))
            return continue_segment("summary_archive_head_invalid");

        if (segment) {
            const char *reason = reinterpret_cast<const char *>(bytes->data() + HEAD);
            if (!reason[0] || !memchr(reason, '\0', AC_STORAGE_ERROR_MAX))
                return continue_segment("summary_archive_head_invalid");
            copy_cstr(recovery_reason, sizeof(recovery_reason), reason);
            copy_cstr(status.recovery_reason, sizeof(status.recovery_reason), reason);
        }

        end = LittleEndian::get_le64(bytes->data() + 8);
        sequence = LittleEndian::get_le64(bytes->data() + 16);
        if ((end == 0) != (sequence == 0) || end > UINT32_MAX || (end && end < HEADER))
            return continue_segment("summary_archive_head_invalid");
        if (!end) {
            loaded = true;
            phase = Phase::Prepare;
            return true;
        }
        phase = Phase::Header;
        return true;
    }

    bool check_new() {
        if (head_loader.status().state == StorageBoundedFileLoadState::Idle) {
            if (head_loader.start((path + ".journal").c_str(), 1, generation,
                                  StorageReadLane::Maintenance) != OperationAdmission::Accepted) return false;
        }
        const bool worked = head_loader.poll();
        if (head_loader.status().active()) return worked;
        const auto state = head_loader.status().state;
        if (state == StorageBoundedFileLoadState::Ready ||
            strcmp(head_loader.status().error, "storage_file_size_invalid") == 0)
            return continue_segment("summary_archive_missing_head");
        if (state != StorageBoundedFileLoadState::Missing) return fail(head_loader.status().error);

        head_loader.reset();
        phase = Phase::Initialize;
        return true;
    }

    bool read_part(size_t wanted) {
        if (!ticket.valid() && !prepared.valid()) {
            StorageReadCommand command;
            command.path = path + ".journal";
            command.offset = cursor + input.size();
            command.length = std::min<uint64_t>(CHUNK, end - command.offset);
            command.generation = generation;
            command.lane = StorageReadLane::Maintenance;
            command.retain_handle = true;
            const auto submitted = read->request_read(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("summary_archive_read_rejected");
            requested_bytes = command.length;
            ticket = submitted.ticket;
            return true;
        }
        if (ticket.valid()) {
            StorageReadCompletion completion;
            if (!read->take_completion(ticket, completion)) return false;
            ticket = {};
            prepared = completion.prepared;
            prepared_used = 0;
            if (strcmp(completion.error, "read_not_found") == 0 ||
                strcmp(completion.error, "read_offset_out_of_range") == 0 ||
                (completion.outcome.disposition == OperationDisposition::Succeeded && completion.file_size < end))
                return continue_segment("summary_archive_journal_truncated");
            if (completion.outcome.disposition != OperationDisposition::Succeeded || !prepared.valid())
                return fail("summary_archive_read_failed");
            if (prepared.length != requested_bytes) return fail("summary_archive_read_short");
        }
        const auto view = read->view_prepared(prepared);
        if (view.state == PreparedByteReadState::Retry) return false;

        if (!view.valid() || view.length != requested_bytes || prepared_used > view.length)
            return fail("summary_archive_read_short");
        const size_t bytes = std::min(view.length - prepared_used, wanted - input.size());
        if (!input.append(view.data + prepared_used, bytes)) return fail("summary_archive_memory");
        prepared_used += bytes;
        if (prepared_used == view.length) {
            read->release_prepared(prepared);
            prepared = {};
        }
        return true;
    }

    bool replay() {
        if (phase == Phase::Header) {
            if (input.size() != HEADER) return read_part(HEADER);
            const auto *p = input.data();
            frame_size = LittleEndian::get_le32(p + 8);
            raw_size = LittleEndian::get_le32(p + 12);
            if (memcmp(p, FRAME_MAGIC, 8) || frame_size < HEADER || frame_size > MAX_FRAME ||
                frame_size > end - cursor || raw_size > frame_size - HEADER ||
                LittleEndian::get_le32(p + 16) != frame_size - HEADER - raw_size ||
                LittleEndian::get_le64(p + 24) != replay_sequence + 1)
                return continue_segment("summary_archive_frame_invalid");

            if (!input.reserve_capacity(frame_size)) return fail("summary_archive_memory");

            phase = Phase::Body;
            return true;
        }
        if (input.size() != frame_size) return read_part(frame_size);

        LargeJsonAllocator allocator;
        JsonDocument doc(&allocator);
        const auto *raw = input.data() + HEADER;
        const auto decoded = deserializeJson(doc, raw + raw_size, frame_size - HEADER - raw_size);
        if (decoded == DeserializationError::NoMemory) return fail("summary_archive_memory");
        if (decoded ||
            doc["device"].as<std::string>() != serial ||
            doc["base"].as<uint64_t>() != replay_sequence ||
            !doc["new"].is<JsonArray>() || !doc["put"].is<JsonArray>() ||
            !doc["remove"].is<JsonArray>()) return continue_segment("summary_archive_metadata_invalid");

        for (JsonArrayConst item : doc["new"].as<JsonArrayConst>()) {
            Digest key;
            const size_t offset = item[1].as<size_t>();
            const size_t size = item[2].as<size_t>();
            if (item.size() != 3 || !decode_digest(item[0], key) || offset > raw_size ||
                size > raw_size - offset || digest(raw + offset, size) != key)
                return continue_segment("summary_archive_record_invalid");
            blobs.emplace(key, Blob{cursor + HEADER + offset, static_cast<uint32_t>(size)});
        }
        for (JsonVariantConst day : doc["remove"].as<JsonArrayConst>()) {
            if (!day.is<int32_t>()) return continue_segment("summary_archive_day_invalid");
            mapping.erase(day.as<int32_t>());
        }
        for (JsonArrayConst item : doc["put"].as<JsonArrayConst>()) {
            Digest key;
            if (item.size() != 2 || !item[0].is<int32_t>() || !decode_digest(item[1], key) ||
                blobs.find(key) == blobs.end()) return continue_segment("summary_archive_reference_invalid");
            mapping[item[0].as<int32_t>()] = key;
        }
        Digest expected;
        if (!decode_digest(doc["mapping"], expected) || mapping_digest(mapping) != expected)
            return continue_segment("summary_archive_mapping_invalid");

        cursor += frame_size;
        ++replay_sequence;
        input.clear();
        if (cursor == end) {
            if (replay_sequence != sequence) return continue_segment("summary_archive_sequence_invalid");
            read->release_handle();
            loaded = true;
            status.sequence = sequence;
            status.committed_bytes = end;
            status.records = mapping.size();
            phase = Phase::Prepare;
        } else {
            if (end - cursor < HEADER) return continue_segment("summary_archive_truncated_header");
            phase = Phase::Header;
        }
        return true;
    }

    bool prepare() {
        LargeJsonAllocator allocator;
        JsonDocument doc(&allocator);
        doc["base"] = sequence;
        doc["segment"] = segment;
        if (segment && !sequence) doc["recovery_reason"] = recovery_reason;
        doc["device"] = serial;
        doc["firmware"] = observation.firmware;
        doc["device_firmware"] = observation.device_firmware;
        doc["boot_id"] = observation.boot_id;
        doc["reason"] = static_cast<unsigned>(observation.reason);
        doc["session_id"] = observation.session_id;
        doc["session_start"] = observation.session_start;
        doc["session_end"] = observation.session_end;
        doc["from_ms"] = observation.from_ms;
        encode_clock(doc["started"].to<JsonObject>(), observation.started);
        encode_clock(doc["finished"].to<JsonObject>(), observation.finished);
        auto additions = doc["new"].to<JsonArray>();
        auto changes = doc["put"].to<JsonArray>();
        auto removals = doc["remove"].to<JsonArray>();
        ReportSpoolBuffer raw;
        next.clear();
        added.clear();
        size_t ordinal = 0;

        auto add_record = [&](const uint8_t *bytes, size_t length) {
            int32_t day = -static_cast<int32_t>(++ordinal);
            struct Context { int32_t day = -1; size_t count = 0; } context;
            report_parse_summary_records(bytes, length,
                [](void *opaque, const ReportSummaryRecord &record) {
                    auto &value = *static_cast<Context *>(opaque);
                    ++value.count;
                    report_summary_sleep_day_epoch_days(record, value.day);
                    return true;
                }, &context, nullptr, 0);
            if (context.count == 1 && context.day >= 0 && !next.count(context.day)) day = context.day;

            const Digest key = digest(bytes, length);
            next[day] = key;
            if (!blobs.count(key) && !added.count(key)) {
                const size_t offset = raw.size();
                if (!raw.append(bytes, length)) throw std::bad_alloc();
                added[key] = {end + HEADER + offset, static_cast<uint32_t>(length)};
                auto item = additions.add<JsonArray>();
                item.add(hex(key));
                item.add(offset);
                item.add(length);
            }

            const auto previous = mapping.find(day);
            if (previous == mapping.end() || previous->second != key) {
                auto item = changes.add<JsonArray>();
                item.add(day);
                item.add(hex(key));
            }
        };

        const auto &payload = observation.payload;
        if (payload && report_proto_all_length_fields(payload->data(), payload->size(), 2)) {
            size_t index = 0;
            ReportProtoField field;
            while (report_proto_next(payload->data(), payload->size(), index, field))
                add_record(field.data, field.len);
        } else if (payload && payload->size()) {
            // Preserve opaque or future formats even when no day can be decoded.
            add_record(payload->data(), payload->size());
        }

        for (const auto &entry : mapping) if (!next.count(entry.first)) removals.add(entry.first);
        doc["mapping"] = hex(mapping_digest(next));
        status.changed = changes.size();
        status.removed = removals.size();
        const size_t json_size = measureJson(doc);
        const size_t total = HEADER + raw.size() + json_size;
        if (doc.overflowed() || total > MAX_FRAME || end > UINT32_MAX - total || sequence == UINT64_MAX)
            return fail("summary_archive_capacity");

        auto bytes = LargeByteBuffer::allocate(total);
        if (!bytes) return fail("summary_archive_memory");
        memset(bytes->data(), 0, HEADER);
        memcpy(bytes->data(), FRAME_MAGIC, 8);
        LittleEndian::put_le32(bytes->data() + 8, total);
        LittleEndian::put_le32(bytes->data() + 12, raw.size());
        LittleEndian::put_le32(bytes->data() + 16, json_size);
        LittleEndian::put_le64(bytes->data() + 24, sequence + 1);
        if (raw.size()) memcpy(bytes->data() + HEADER, raw.data(), raw.size());
        if (serializeJson(doc, bytes->data() + HEADER + raw.size(), json_size) != json_size)
            return fail("summary_archive_encode");

        output = LargeByteBuffer::freeze(std::move(bytes));
        observation = {};
        progress = 0;
        phase = Phase::Write;
        return true;
    }

    bool write() {
        if (!ticket.valid()) {
            StorageRangeWriteCommand command;
            command.path = path + ".journal";
            command.offset = end + progress;
            command.bytes = LargeByteBuffer::slice(output, progress, std::min(CHUNK, output->size() - progress));
            command.generation = generation;
            const auto submitted = range->request_write(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("summary_archive_write_rejected");
            ticket = submitted.ticket;
            return true;
        }

        StorageRangeWriteCompletion completion;
        if (!range->take_completion(ticket, completion)) return false;
        ticket = {};
        const size_t expected = std::min(CHUNK, output->size() - progress);
        if (completion.outcome.disposition != OperationDisposition::Succeeded || completion.bytes_written != expected)
            return fail("summary_archive_write_failed");
        progress += expected;
        if (progress == output->size()) phase = Phase::Commit;
        return true;
    }

    bool commit() {
        const bool initialize = phase == Phase::Initialize;
        if (!ticket.valid()) {
            auto bytes = LargeByteBuffer::allocate(segment ? SEGMENT_HEAD : HEAD);
            if (!bytes) return fail("summary_archive_memory");
            memcpy(bytes->data(), segment ? SEGMENT_HEAD_MAGIC : HEAD_MAGIC, 8);
            if (segment) memcpy(bytes->data() + HEAD, recovery_reason, AC_STORAGE_ERROR_MAX);
            LittleEndian::put_le64(bytes->data() + 8, initialize ? 0 : end + output->size());
            LittleEndian::put_le64(bytes->data() + 16, initialize ? 0 : sequence + 1);
            StorageAtomicWriteCommand command;
            command.path = path + ".head";
            command.bytes = LargeByteBuffer::freeze(std::move(bytes));
            command.generation = generation;
            const auto submitted = atomic->request_write(command);
            if (submitted.admission == OperationAdmission::Busy) return false;
            if (!submitted.accepted()) return fail("summary_archive_commit_rejected");
            ticket = submitted.ticket;
            return true;
        }

        StorageAtomicWriteCompletion completion;
        if (!atomic->take_completion(ticket, completion)) return false;
        ticket = {};
        if (completion.outcome.disposition != OperationDisposition::Succeeded)
            return fail("summary_archive_commit_failed");

        if (initialize) {
            loaded = true;
            phase = Phase::Prepare;
            return true;
        }

        end += output->size();
        ++sequence;
        mapping.swap(next);
        blobs.merge(added);
        next.clear();
        added.clear();
        output.reset();
        status.active = false;
        status.sequence = sequence;
        status.committed_bytes = end;
        status.records = mapping.size();
        phase = Phase::Idle;
        return true;
    }
};

SummaryArchive::SummaryArchive() {
    void *memory = Memory::alloc_large(sizeof(Impl), false);
    if (memory) impl_ = new (memory) Impl();
}

SummaryArchive::~SummaryArchive() {
    if (impl_) {
        impl_->~Impl();
        Memory::free(impl_);
    }
}

void SummaryArchive::begin(StorageReadPort &read, StorageRangeWritePort &range,
                           StorageAtomicWritePort &atomic, StorageScanPort &scan) {
    if (!impl_) return;
    impl_->read = &read;
    impl_->range = &range;
    impl_->atomic = &atomic;
    impl_->scan = &scan;
    impl_->head_loader.begin(read);
}

bool SummaryArchive::start(SummaryObservation observation) {
    if (!impl_) return false;

    try {
        return impl_->start(std::move(observation));
    } catch (const std::bad_alloc &) {
        impl_->fail("summary_archive_memory");
        return false;
    }
}

bool SummaryArchive::poll() {
    if (!impl_) return false;
    try {
        switch (impl_->phase) {
            case Impl::Phase::Discover: return impl_->discover();
            case Impl::Phase::Head: return impl_->load_head();
            case Impl::Phase::CheckNew: return impl_->check_new();
            case Impl::Phase::Header:
            case Impl::Phase::Body: return impl_->replay();
            case Impl::Phase::Prepare: return impl_->prepare();
            case Impl::Phase::Write: return impl_->write();
            case Impl::Phase::Initialize:
            case Impl::Phase::Commit: return impl_->commit();
            default: return false;
        }
    } catch (const std::bad_alloc &) {
        return impl_->fail("summary_archive_memory");
    }
}

void SummaryArchive::forget_media() {
    if (impl_) impl_->forget();
}

const SummaryArchiveStatus &SummaryArchive::status() const {
    static const SummaryArchiveStatus unavailable{false, 0, 0, 0, 0, 0, 0, "summary_archive_memory"};
    return impl_ ? impl_->status : unavailable;
}

}  // namespace aircannect
