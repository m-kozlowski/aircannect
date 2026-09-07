#include "report_signal_tile_writer.h"

#include <algorithm>
#include <string.h>

namespace aircannect {

ReportSignalTileWriter::~ReportSignalTileWriter() { reset(); }

void ReportSignalTileWriter::begin(StorageReadPort &read,
                                   StorageRangeWritePort &write) {
    reset();
    read_ = &read;
    write_ = &write;
}

void ReportSignalTileWriter::start(
    const ReportSignalStoreTrack &track, ReportSignalStoreLevel level,
    size_t first_slot, size_t block_count, StorageRangeWriteCommand memory,
    size_t memory_prefix, uint32_t generation, StorageAtomicWriteLane lane) {
    reset();
    track_ = track;
    level_ = level;
    cursor_ = first_slot;
    end_slot_ = first_slot + block_count;
    memory_ = std::move(memory);
    memory_prefix_ = memory_prefix;
    generation_ = generation;
    lane_ = lane;
    succeeded_ = read_ && write_ && block_count && generation &&
        end_slot_ <= track.block_slot_count &&
        ReportSignalStoreFileCodec::plane_range(track,
            track.first_block_start_ms + first_slot * REPORT_SIGNAL_STORE_BLOCK_MS,
            block_count, level, memory_range_);
    if (succeeded_) phase_ = Phase::Select;
}

void ReportSignalTileWriter::release_read() {
    if (read_ticket_.valid()) read_->abandon(read_ticket_);
    read_ticket_ = {};
    if (prepared_.valid()) read_->release_prepared(prepared_);
    prepared_ = {};
}

void ReportSignalTileWriter::advance(bool success) {
    succeeded_ = succeeded_ && success;
    release_read();
    input_.reset();
    output_.reset();
    encoder_.reset();
    copied_ = 0;
    cursor_ = tile_.first_slot + tile_.block_count;
    phase_ = cursor_ < end_slot_ ? Phase::Select : Phase::Idle;
    if (!active()) memory_ = {};
}

bool ReportSignalTileWriter::poll() {
    if (!active()) return false;

    if (phase_ == Phase::Select) {
        if (!ReportSignalTile::describe(track_, level_, cursor_, tile_)) {
            succeeded_ = false;
            phase_ = Phase::Idle;
            memory_ = {};
            return true;
        }
        // A later batch owns a tile that still has unwritten blocks.
        for (size_t s = end_slot_; s < tile_.first_slot + tile_.block_count; ++s) {
            if (track_.present_blocks[s / 8] & (1u << (s % 8))) {
                advance();
                return true;
            }
        }
        if (tile_.range.length < ReportSignalTile::MinRawBytes) {
            advance();
            return true;
        }

        const bool memory = memory_.bytes || memory_.buffers;
        from_memory_ = memory && tile_.range.offset >= memory_range_.offset &&
            tile_.range.offset + tile_.range.length <=
                memory_range_.offset + memory_range_.length;
        if (from_memory_) {
            memory_offset_ = memory_prefix_ + tile_.range.offset - memory_range_.offset;

            // Most raw tiles and all LOD batches already have contiguous bytes.
            // Keep a slice alive instead of copying those bytes for compression.
            size_t offset = memory_offset_;
            auto parent = memory_.bytes;
            if (!parent || offset >= parent->size()) {
                if (parent) offset -= parent->size();
                parent.reset();
                if (memory_.buffers) {
                    for (size_t i = 0; i < memory_.buffers->size(); ++i) {
                        const auto &part = memory_.buffers->data()[i];
                        if (offset < part->size()) { parent = part; break; }
                        offset -= part->size();
                    }
                }
            }
            if (parent && tile_.range.length <= parent->size() - offset) {
                const auto slice = LargeByteBuffer::slice(parent, offset, tile_.range.length);
                if (encoder_.start(slice, tile_)) phase_ = Phase::Compress;
                else advance(encoder_.succeeded());
                return true;
            }

            input_ = LargeByteBuffer::allocate(tile_.range.length);
            if (!input_) { advance(false); return true; }
            phase_ = Phase::Copy;
        } else {
            // Existing reports are inspected only by background preparation.
            phase_ = memory ? Phase::Read : Phase::Check;
        }
        return true;
    }

    if (phase_ == Phase::Check || phase_ == Phase::Read) {
        checking_ = phase_ == Phase::Check;
        char path[AC_STORAGE_PATH_MAX] = {};
        const bool path_ok = checking_
            ? tile_.path(track_, level_, path, sizeof(path))
            : report_signal_store_signal_path(track_, level_, path, sizeof(path));
        if (!path_ok) { advance(false); return true; }

        StorageReadCommand command;
        command.path = path;
        command.offset = checking_ ? 0 : tile_.range.offset;
        command.length = checking_ ? ReportSignalTile::HeaderBytes : tile_.range.length;
        command.generation = generation_;
        command.lane = lane_ == StorageAtomicWriteLane::Foreground
            ? StorageReadLane::Foreground : StorageReadLane::Maintenance;
        const auto submitted = read_->request_read(command);
        if (submitted.admission == OperationAdmission::Busy) return false;
        if (!submitted.accepted()) { advance(false); return true; }
        read_ticket_ = submitted.ticket;
        phase_ = Phase::WaitRead;
        return true;
    }

    if (phase_ == Phase::WaitRead) {
        if (!prepared_.valid()) {
            StorageReadCompletion completion;
            if (!read_->take_completion(read_ticket_, completion)) return false;
            read_ticket_ = {};
            prepared_ = completion.prepared;
            read_file_size_ = completion.file_size;
            if (completion.outcome.disposition != OperationDisposition::Succeeded ||
                !prepared_.valid()) {
                release_read();
                if (checking_) phase_ = Phase::Read;
                else advance(false);
                return true;
            }
        }
        const auto view = read_->view_prepared(prepared_);
        if (view.state == PreparedByteReadState::Retry) return false;
        if (checking_) {
            const bool matches = view.valid() &&
                tile_.matches(view.data, view.length, read_file_size_);
            release_read();
            if (matches) advance();
            else phase_ = Phase::Read;
            return true;
        }
        if (!view.valid() || view.length != tile_.range.length) {
            advance(false);
            return true;
        }
        input_ = LargeByteBuffer::allocate(view.length);
        if (!input_) { advance(false); return true; }
        phase_ = Phase::Copy;
        return true;
    }

    if (phase_ == Phase::Copy) {
        size_t count = std::min<size_t>(2048, input_->size() - copied_);
        if (from_memory_) {
            size_t available = 0;
            const uint8_t *source = memory_.span(memory_offset_ + copied_, available);
            if (!source || !available) { advance(false); return true; }
            count = std::min(count, available);
            memcpy(input_->data() + copied_, source, count);
        } else {
            const auto read = read_->read_prepared(
                prepared_, copied_, input_->data() + copied_, count);
            if (read.state == PreparedByteReadState::Retry) return false;
            if (read.state != PreparedByteReadState::Data || !read.bytes) {
                advance(false);
                return true;
            }
            count = read.bytes;
        }
        copied_ += count;
        if (copied_ == input_->size()) {
            release_read();
            if (!encoder_.start(LargeByteBuffer::freeze(std::move(input_)), tile_)) {
                advance(encoder_.succeeded());
            } else phase_ = Phase::Compress;
        }
        return true;
    }

    if (phase_ == Phase::Compress) {
        encoder_.poll();
        if (encoder_.active()) return true;
        output_ = encoder_.take_result();
        if (!output_) advance(encoder_.succeeded());
        else phase_ = Phase::Write;
        return true;
    }

    if (phase_ == Phase::Write) {
        char path[AC_STORAGE_PATH_MAX] = {};
        if (!tile_.path(track_, level_, path, sizeof(path))) {
            advance(false);
            return true;
        }
        StorageRangeWriteCommand command;
        command.path = path;
        command.bytes = output_;
        command.generation = generation_;
        command.lane = lane_;
        command.truncate = true;
        command.sync_on_close = false;
        const auto submitted = write_->request_write(command);
        if (submitted.admission == OperationAdmission::Busy) return false;
        if (!submitted.accepted()) { advance(false); return true; }
        write_ticket_ = submitted.ticket;
        phase_ = Phase::WaitWrite;
        return true;
    }

    StorageRangeWriteCompletion completion;
    if (!write_->take_completion(write_ticket_, completion)) return false;
    write_ticket_ = {};
    advance(completion.outcome.disposition == OperationDisposition::Succeeded &&
        completion.bytes_written == output_->size());
    return true;
}

void ReportSignalTileWriter::reset() {
    release_read();
    if (write_ticket_.valid()) write_->abandon(write_ticket_);
    write_ticket_ = {};
    encoder_.reset();
    memory_ = {};
    input_.reset();
    output_.reset();
    copied_ = 0;
    phase_ = Phase::Idle;
    succeeded_ = true;
}

}  // namespace aircannect
