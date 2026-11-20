#include "report_oximetry_signals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include "large_json_allocator.h"
#include "large_object.h"
#include "little_endian.h"
#include "memory_manager.h"
#include "report_oximetry_samples.h"
#include "report_signal_tile.h"
#include "storage_space_policy.h"
#ifdef ARDUINO
#include "debug_log.h"
#endif

namespace aircannect {
namespace {
constexpr size_t SampleStepBytes = 10000, RawBytes = 16;
constexpr int64_t Block = REPORT_SIGNAL_STORE_BLOCK_MS;
using Counts = std::map<float, uint32_t, std::less<float>,
                        LargeAllocator<std::pair<const float, uint32_t>>>;
using LittleEndian::put_le16;
using LittleEndian::put_le32;
using LittleEndian::get_le32;

struct BlockStatistics {
    uint32_t first = 0, rows = 0;
    int64_t start = 0, end = 0;
    uint32_t histogram_offset = 0, histogram_size = 0;
};

// This small fixed record is enough to reopen a published supplement without
// retaining its histograms or reading its RAW data on boot.
constexpr size_t PublicationBytes = ReportOximetryPlanes::PublicationBytes;
void encode_publication(uint8_t *out, const ReportOximetryPlanes &p) {
    memcpy(out, "ACOXSG02", 8);
    put_le32(out + 8, p.rows);
    put_le32(out + 12, p.index_bytes);
    put_le32(out + 48, p.histogram_bytes);
    for (size_t c = 0; c < 4; ++c) {
        put_le32(out + 16 + 4*c, p.plane_bytes[c]);
        put_le32(out + 32 + 4*c, p.compressed_bytes[c]);
    }
}
}  // namespace

bool ReportOximetryPlanes::decode(const uint8_t *in, size_t len) {
    if (len != PublicationBytes || memcmp(in, "ACOXSG02", 8)) return false;
    rows = get_le32(in + 8);
    index_bytes = get_le32(in + 12);
    histogram_bytes = get_le32(in + 48);
    for (size_t c = 0; c < 4; ++c) {
        plane_bytes[c] = get_le32(in + 16 + 4*c);
        compressed_bytes[c] = get_le32(in + 32 + 4*c);
    }
    return index_bytes > 0 && rows <= UINT32_MAX / RawBytes;
}

std::string report_oximetry_signal_path(const ReportOximetryRecording &r, const char *plane) {
    SleepDayId day;
    char night[AC_STORAGE_PATH_MAX]{};
    if (!SleepDayId::from_yyyymmdd(r.day, day) ||
        !report_signal_store_night_directory(day, night, sizeof(night))) return {};
    return std::string(night) + "/ox/" + r.source + "/" + r.id + "/" +
        std::to_string(r.start_ms) + "-" + std::to_string(r.size()) + "-" +
        std::to_string(r.modified) + "-" + std::to_string(r.noon_s) + "-" +
        std::to_string(r.next_noon_s) + "/" + plane;
}

bool report_oximetry_overview_tile(const ReportOximetryRecording &r, ReportSignalTile &tile) {
    ReportSignalStoreTrack track{};
    track.sample_interval_ms = 1000;
    track.lod_mask = REPORT_SIGNAL_STORE_LOD_10S;
    const int64_t start = std::max(r.start_ms, r.noon_s * 1000);
    const int64_t end = std::min(r.end_ms(), r.next_noon_s * 1000);
    track.first_block_start_ms = start - start % Block;
    track.last_valid_sample_ms = end - 1;
    const auto slots = (end - track.first_block_start_ms + Block - 1) / Block;
    if (slots <= 0 || slots > REPORT_SIGNAL_STORE_MAX_BLOCKS) return false;

    track.block_slot_count = track.present_block_count = slots;
    for (int i = 0; i < slots; ++i) track.present_blocks[i/8] |= 1u << (i%8);
    return ReportSignalTile::describe(track, ReportSignalStoreLevel::TenSeconds, 0, tile);
}

struct ReportOximetrySignalBuilder::Work {
    enum class Phase { Probe, Samples, Plane, Tile, Histograms, Index, Publish, Published, Done };
    Phase phase = Phase::Probe;
    ReportOximetryPlanes product;
    ReportOximetrySampleReader reader;
    ReportSignalTileEncoder encoder;
    ReportOximetryRecording *record = nullptr;
    int64_t first_block = 0;
    size_t slots = 0, channel = 0, read_offset = 0;
    std::array<std::shared_ptr<LargeByteBuffer>,4> planes;
    std::vector<BlockStatistics,LargeAllocator<BlockStatistics>> blocks;
    // BLE measurements present in each UTC second, independent per channel.
    std::vector<uint8_t,LargeAllocator<uint8_t>> coverage;
    Counts block_counts[4], total_counts[4];
    size_t current_block = SIZE_MAX;
    std::unique_ptr<LargeByteBuffer> raw, histograms;
    size_t raw_used = 0, hist_used = 0;
    const size_t raw_limit = Memory::buffer_budget(AC_STORAGE_RANGE_WRITE_MAX_BYTES, 32 * 1024);
    const size_t read_limit = Memory::buffer_budget(128 * 1024, 32 * 1024);
    const size_t hist_limit = Memory::buffer_budget(64 * 1024, 8 * 1024);
    std::shared_ptr<const LargeByteBuffer> output;
    StorageReadCommand read;
    OperationTicket read_ticket, write_ticket;
    StoragePreparedRead prepared;
    std::string path;
    bool atomic = false;
    bool retry = false, verified = false;
#ifdef ARDUINO
    uint32_t build_started_ms = 0;
    uint32_t paused_at_ms = 0, paused_ms = 0;
#endif
    uint64_t offset = 0;
    size_t sent = 0, chunk = 0;

    void flush(std::unique_ptr<LargeByteBuffer> &buffer, size_t &used,
               const char *plane, size_t total_bytes) {
        buffer->truncate(used);
        output = LargeByteBuffer::freeze(std::move(buffer));
        path = report_oximetry_signal_path(*record, plane);
        offset = total_bytes - used;
        used = 0;
    }

    // Keep maps only for the current block. Batch the packed blocks on SD.
    bool finish_block() {
        if (current_block == SIZE_MAX) return true;
        auto &b = blocks[current_block];
        size_t length = 16;
        for (const auto &counts : block_counts) length += 8 * counts.size();

        const size_t needed = hist_used + length;
        if (!histograms) histograms = LargeByteBuffer::allocate(std::max<size_t>(1024, needed));
        if (!histograms) return false;
        if (needed > histograms->size()) {
            const size_t capacity = std::max(needed, std::min(hist_limit, histograms->size() * 2));
            if (!histograms->grow(capacity)) return false;
        }

        auto *bytes = histograms->data() + hist_used;
        size_t pos = 16;
        for (size_t c = 0; c < 4; ++c) {
            put_le32(bytes+4*c, block_counts[c].size());
            for (const auto &[value, weight] : block_counts[c]) {
                memcpy(bytes+pos, &value, sizeof(value));
                put_le32(bytes+pos+4, weight);
                pos += 8;
            }
            block_counts[c].clear();
        }
        b.histogram_offset = product.histogram_bytes;
        b.histogram_size = length;
        product.histogram_bytes += length;
        hist_used += length;
        current_block = SIZE_MAX;
        return true;
    }

    bool add(const ReportOximetrySample &s) {
        const int64_t start = std::max(s.time_ms, record->noon_s * 1000);
        const int64_t end = std::min(s.time_ms + (s.duration_ms ? s.duration_ms : 1),
                                     record->next_noon_s * 1000);
        if (end <= start) return true;
        const size_t slot = (start - first_block) / Block;
        if (slot >= slots || raw_used + RawBytes > raw->size()) return false;
        if (slot != current_block && !finish_block()) return false;
        current_block = slot;

        auto &b = blocks[slot];
        if (!b.rows) {
            b.first = product.rows;
            b.start = start;
        }
        b.end = end;
        ++b.rows;
        ++product.rows;
        auto *p = raw->data() + raw_used;
        raw_used += RawBytes;
        put_le32(p, static_cast<uint32_t>(s.time_ms - record->start_ms));
        for (size_t c = 0; c < 4; ++c) {
            const float v = s.values[c];
            if (c == 3) memcpy(p + 12, &v, sizeof(v));
            else put_le16(p + 4 + 2*c, static_cast<int16_t>(v));
            if (v < 0) continue;
            if (record->csv_bytes && !strncmp(record->source, "ble-", 4)) {
                const size_t second = (start - first_block) / 1000;
                if (second >= coverage.size()) coverage.resize(second + 1);
                coverage[second] |= uint8_t(1u << c);
            }
            const auto weight = s.duration_ms ? uint32_t(end - start) : 1;
            block_counts[c][v] += weight;
            total_counts[c][v] += weight;
            if (!planes[c]) {
                planes[c] = LargeByteBuffer::allocate_shared(slots * (Block / 10000) * 4);
                if (!planes[c]) return false;
                auto *cells = reinterpret_cast<int16_t *>(planes[c]->data());
                std::fill_n(cells, planes[c]->size()/2, REPORT_SIGNAL_STORE_MISSING_S16);
            }
            // PI is measured in hundredths; use a biased signed value to
            // cover the full unsigned Nonin range. RAW retains the float.
            const int code = c == 3 ? int(std::lround(v * 100)) - 32767 : int(v);
            const auto value = int16_t(std::clamp(code, -32767, 32767));
            for (auto bin = (start-first_block)/10000; bin <= (end-1-first_block)/10000; ++bin) {
                auto *cells = reinterpret_cast<int16_t *>(planes[c]->data()) + 2*bin;
                cells[0] = cells[0] == INT16_MIN ? value : std::min(cells[0], value);
                cells[1] = cells[1] == INT16_MIN ? value : std::max(cells[1], value);
            }
        }
        p[10] = p[11] = 0;
        return true;
    }

    std::shared_ptr<const LargeByteBuffer> index() const {
        LargeJsonAllocator allocator;
        JsonDocument doc(&allocator);
        doc["first"] = first_block;
        doc["slots"] = slots;
        auto histogram = doc["histogram"].to<JsonArray>();
        for (const auto &counts : total_counts) {
            auto bins = histogram.add<JsonArray>();
            for (const auto &[value, weight] : counts) {
                bins.add(value);
                bins.add(weight);
            }
        }
        auto list = doc["blocks"].to<JsonArray>();
        for (const auto &b : blocks) {
            if (!b.rows) continue;
            auto entry = list.add<JsonArray>();
            entry.add(b.first); entry.add(b.rows); entry.add(b.start); entry.add(b.end);
            entry.add(b.histogram_offset);
            entry.add(b.histogram_size);
        }
        if (!coverage.empty()) {
            // Usually the same few channels cover an entire recording. Store
            // runs as [end second (exclusive), mask]; bound noisy input by
            // using one hex digit per second if runs would cost more.
            size_t runs = 1;
            for (size_t i = 1; i < coverage.size(); ++i)
                if (coverage[i] != coverage[i - 1]) ++runs;
            if (runs * 10 < coverage.size()) {
                auto ranges = doc["coverage"].to<JsonArray>();
                for (size_t i = 1; i <= coverage.size(); ++i) {
                    if (i < coverage.size() && coverage[i] == coverage[i - 1]) continue;
                    ranges.add(i);
                    ranges.add(coverage[i - 1]);
                }
            } else {
                auto masks = LargeByteBuffer::allocate(coverage.size() + 1);
                if (!masks) return {};
                for (size_t i = 0; i < coverage.size(); ++i)
                    masks->data()[i] = "0123456789abcdef"[coverage[i]];
                masks->data()[coverage.size()] = 0;
                doc["coverage"] = reinterpret_cast<char *>(masks->data());
            }
        }
        if (doc.overflowed()) return {};
        auto bytes = LargeByteBuffer::allocate(measureJson(doc));
        if (!bytes || serializeJson(doc, bytes->data(), bytes->size()) != bytes->size()) return {};
        return LargeByteBuffer::freeze(std::move(bytes));
    }
};

void ReportOximetrySignalBuilder::begin(StorageReadPort &reads, StorageRangeWritePort &ranges,
                                        StorageAtomicWritePort &writes) {
    reads_ = &reads;
    ranges_ = &ranges;
    writes_ = &writes;
}

ReportOximetrySignalBuilder::~ReportOximetrySignalBuilder() { reset(); }

void ReportOximetrySignalBuilder::reset() {
    if (!work_) return;
    if (work_->read_ticket.valid()) reads_->abandon(work_->read_ticket);
    if (work_->prepared.valid()) reads_->release_prepared(work_->prepared);
    if (work_->write_ticket.valid()) {
        if (work_->atomic) writes_->abandon(work_->write_ticket);
        else ranges_->abandon(work_->write_ticket);
    }
    reads_->release_handle();
    LargeObject::destroy(work_);
    work_ = nullptr;
}

bool ReportOximetrySignalBuilder::finished() const {
    return work_ && work_->phase == Work::Phase::Done;
}

bool ReportOximetrySignalBuilder::retryable() const { return work_ && work_->retry; }
bool ReportOximetrySignalBuilder::verified() const { return work_ && work_->verified; }

bool ReportOximetrySignalBuilder::complete_write(Work &w) {
    bool ok;
    if (w.atomic) {
        StorageAtomicWriteCompletion done;
        if (!writes_->take_completion(w.write_ticket, done)) return false;
        ok = done.outcome.disposition == OperationDisposition::Succeeded;
    } else {
        StorageRangeWriteCompletion done;
        if (!ranges_->take_completion(w.write_ticket, done)) return false;
        ok = done.outcome.disposition == OperationDisposition::Succeeded &&
             done.bytes_written == w.chunk;
    }
    w.write_ticket = {};
    if (!ok) {
        w.retry = true;
        w.phase = Work::Phase::Done;
        return true;
    }

    w.sent += w.chunk;
    if (!w.atomic && w.sent < w.output->size()) return true;
    w.sent = 0;
    w.output.reset();
    if (w.atomic) {
        w.phase = Work::Phase::Published;
#ifdef ARDUINO
        const uint32_t now = millis();
        const uint32_t paused = w.paused_ms + (w.paused_at_ms ? now - w.paused_at_ms : 0);
        Log::logf(CAT_REPORT, LOG_DEBUG, "oximetry tracks id=%s rows=%lu active_ms=%lu paused_ms=%lu",
                  w.record->id, static_cast<unsigned long>(w.product.rows),
                  static_cast<unsigned long>(now - w.build_started_ms - paused),
                  static_cast<unsigned long>(paused));
#endif
    }
    return true;
}

void ReportOximetrySignalBuilder::pause() {
    if (!work_) return;
    auto &w = *work_;
#ifdef ARDUINO
    if (w.build_started_ms && !w.paused_at_ms) w.paused_at_ms = millis();
#endif
    // An unconsumed read can be requested again at read_offset. A write
    // already submitted must be acknowledged, not replayed after the pause.
    if (w.read_ticket.valid() && reads_->abandon(w.read_ticket)) w.read_ticket = {};
    if (w.prepared.valid()) {
        reads_->release_prepared(w.prepared);
        w.prepared = {};
    }
    if (!w.read_ticket.valid() && w.read.retain_handle) {
        reads_->release_handle();
        w.read.retain_handle = false;
    }
    if (w.write_ticket.valid()) complete_write(w);
}

bool ReportOximetrySignalBuilder::step(ReportOximetryRecording &r, Mode mode) {
    const bool manifest_only = mode == Mode::Manifest;
    using Phase = Work::Phase;
    if (!work_) {
        work_ = LargeObject::create<Work>();
        if (!work_) return false;
        work_->record = &r;
        work_->reader.begin(r);
        work_->read_offset = r.csv_bytes ? 0 : r.bin.offset(0);
        const int64_t lo = std::max(r.start_ms, r.noon_s * 1000);
        const int64_t hi = std::min(r.end_ms(), r.next_noon_s * 1000);
        work_->first_block = lo - lo % Block;
        work_->slots = (hi - work_->first_block + Block - 1) / Block;
        if (!work_->slots || work_->slots > REPORT_SIGNAL_STORE_MAX_BLOCKS) {
            work_->phase = Phase::Done;
            return true;
        }
        if (mode == Mode::Build) work_->blocks.resize(work_->slots);
        if (mode == Mode::VerifyOriginal) work_->phase = Phase::Samples;
    }
    auto &w = *work_;
#ifdef ARDUINO
    if (w.paused_at_ms) {
        w.paused_ms += millis() - w.paused_at_ms;
        w.paused_at_ms = 0;
    }
#endif
    auto fail = [&](bool retry = true) {
        w.retry = retry;
        w.phase = Phase::Done;
        return true;
    };
    if (w.write_ticket.valid()) return complete_write(w);
    if (w.phase == Phase::Done) return false;
    if (w.phase == Phase::Published) {
        r.planes = std::allocate_shared<ReportOximetryPlanes>(LargeAllocator<ReportOximetryPlanes>(), w.product);
        w.phase = Phase::Done;
        return true;
    }
    if (w.output) {
        OperationSubmission submitted;
        if (w.atomic) {
            StorageAtomicWriteCommand cmd;
            cmd.path = w.path; cmd.bytes = w.output; cmd.generation = 1;
            submitted = writes_->request_write(cmd);
        } else {
            StorageRangeWriteCommand cmd;
            w.chunk = std::min<size_t>(AC_STORAGE_RANGE_WRITE_MAX_BYTES, w.output->size()-w.sent);
            cmd.path = w.path;
            cmd.bytes = w.chunk == w.output->size() ? w.output :
                LargeByteBuffer::slice(w.output, w.sent, w.chunk);
            cmd.generation = 1;
            cmd.offset = w.offset + w.sent;
            cmd.truncate = !cmd.offset;
            cmd.free_reserve_bytes = StorageSpacePolicy::ReclaimBelow;
            submitted = ranges_->request_write(cmd);
        }
        if (submitted.admission == OperationAdmission::Rejected) return fail();
        w.write_ticket = submitted.ticket;
        return submitted.accepted();
    }
    if (w.hist_used && (w.hist_used >= w.hist_limit || w.phase == Phase::Histograms)) {
        w.flush(w.histograms, w.hist_used, "histograms", w.product.histogram_bytes);
        return true;
    }

    if (w.phase == Phase::Probe || w.phase == Phase::Samples) {
        const bool probe = w.phase == Phase::Probe;
        // A BIN slice contains whole samples and must fit the board's RAW budget.
        const size_t stride = r.bin.stride();
        const size_t step_bytes = r.csv_bytes ? SampleStepBytes :
            std::min(SampleStepBytes / stride, w.raw_limit / RawBytes) * stride;
        const uint64_t end = r.data_end();
        if (!probe && w.read_offset == end) {
            if (!w.reader.complete()) return fail(false);
            if (mode == Mode::VerifyOriginal) {
                w.verified = true;
                w.phase = Phase::Done;
            } else {
                if (!w.finish_block()) return fail();
                if (w.raw_used) w.flush(w.raw, w.raw_used, "raw", w.product.rows * RawBytes);
                w.phase = Phase::Plane;
            }
            reads_->release_handle();
            return true;
        }
        if (!w.prepared.valid() && !w.read_ticket.valid()) {
            w.read.path = probe ? report_oximetry_signal_path(r,"manifest") : r.path();
            w.read.generation = 1;
            w.read.offset = probe ? 0 : w.read_offset;
            const size_t limit = r.csv_bytes && mode == Mode::Build ? w.read_limit : step_bytes;
            w.read.length = probe ? PublicationBytes : std::min<uint64_t>(limit, end - w.read_offset);
            w.read.retain_handle = !probe;
            const auto submit = reads_->request_read(w.read);
            if (submit.admission == OperationAdmission::Rejected) return fail();
            w.read_ticket = submit.ticket;
#ifdef ARDUINO
            if (!w.build_started_ms && !probe && mode == Mode::Build && submit.accepted())
                w.build_started_ms = millis();
#endif
            return submit.accepted();
        }
        if (!w.prepared.valid()) {
            StorageReadCompletion done;
            if (!reads_->take_completion(w.read_ticket, done)) return false;
            w.read_ticket = {};
            w.prepared = done.prepared;
            if (done.outcome.disposition != OperationDisposition::Succeeded ||
                !w.prepared.valid()) {
                if (probe && !strcmp(done.error, "read_not_found"))
                    w.phase = manifest_only ? Phase::Done : Phase::Samples;
                else return fail();
                return true;
            }
            if (!probe && (done.file_size != r.size() || done.modified != r.modified)) return fail();
        }
        const auto view = reads_->view_prepared(w.prepared);
        if (view.state == PreparedByteReadState::Retry) return false;
        if (!view.valid()) return fail();
        if (probe) {
            if (w.product.decode(view.data, view.length)) {
                r.planes = std::allocate_shared<ReportOximetryPlanes>(LargeAllocator<ReportOximetryPlanes>(), w.product);
                w.phase = Phase::Done;
            } else { w.product = {}; w.phase = manifest_only ? Phase::Done : Phase::Samples; }
        } else if (mode == Mode::VerifyOriginal) {
            if (view.length != w.read.length || !w.reader.consume(view.data, view.length, nullptr,
                [](void *, const ReportOximetrySample &) { return true; })) return fail(false);
            w.read_offset += view.length;
        } else {
            if (view.length != w.read.length) return fail();
            const size_t pos = w.read_offset - w.read.offset;
            const size_t length = std::min(step_bytes, view.length - pos);

            // A CSV slice may complete a line carried over from the last slice.
            const auto max_rows = [&](size_t bytes) {
                return r.csv_bytes ? bytes / 24 + 2 : bytes / stride;
            };
            if (!w.raw) {
                const size_t remaining = std::min<uint64_t>(2 * w.raw_limit, end - w.read_offset);
                w.raw = LargeByteBuffer::allocate(std::min(w.raw_limit, max_rows(remaining) * RawBytes));
                if (!w.raw) return fail();
            }

            if (w.raw_used && max_rows(length) * RawBytes > w.raw->size() - w.raw_used) {
                w.flush(w.raw, w.raw_used, "raw", w.product.rows * RawBytes);
                return true;
            }

            if (!w.reader.consume(view.data + pos, length, &w, [](void *ctx, const ReportOximetrySample &s) {
                auto &work = *static_cast<Work *>(ctx);
                if (work.add(s)) return true;
                work.retry = true;
                return false;
            })) return fail(w.retry);
            w.read_offset += length;
            if (pos + length < view.length) return true;
        }
        reads_->release_prepared(w.prepared);
        w.prepared = {};
        return true;
    }

    w.offset = 0;
    if (w.phase == Phase::Plane) {
        while (w.channel < 4 && !w.planes[w.channel]) ++w.channel;
        if (w.channel == 4) { w.phase = Phase::Histograms; return true; }
        const auto &plane = w.planes[w.channel];
        w.product.plane_bytes[w.channel] = plane->size();
        w.output = plane;
        w.path = report_oximetry_signal_path(r, ReportOximetryChannels[w.channel]);
        ReportSignalTile tile;
        if (report_oximetry_overview_tile(r, tile))
            w.encoder.start(plane,tile);
        w.phase = Phase::Tile;
        return true;
    }
    if (w.phase == Phase::Tile) {
        if (w.encoder.active()) return w.encoder.poll();
        w.output = w.encoder.take_result();
        if (w.output) {
            w.path += ".deflate";
            w.product.compressed_bytes[w.channel] = w.output->size();
        }
        w.planes[w.channel++].reset();
        w.phase = Phase::Plane;
        return true;
    }
    if (w.phase == Phase::Histograms) {
        w.phase = Phase::Index;
        return true;
    }
    if (w.phase == Phase::Index) {
        w.output = w.index();
        if (!w.output) return fail();
        w.product.index_bytes = w.output->size();
        w.path = report_oximetry_signal_path(r,"index");
        w.phase = Phase::Publish;
        return true;
    }
    if (w.phase == Phase::Publish) {
        uint8_t bytes[PublicationBytes];
        encode_publication(bytes,w.product);
        w.output = LargeByteBuffer::copy_and_freeze(bytes,sizeof(bytes));
        if (!w.output) return fail();
        w.path = report_oximetry_signal_path(r,"manifest");
        w.atomic = true;
        return true;
    }
    return false;
}
}  // namespace aircannect
