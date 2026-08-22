#include "report_payload_deflater.h"

#include <algorithm>
#include <limits>
#include <string.h>

#include "board_report.h"
#include "memory_manager.h"

#ifdef ARDUINO
#include <uzlib/uzlib.h>
#endif

namespace aircannect {

#ifdef ARDUINO
namespace {

constexpr unsigned int DEFLATE_HASH_BITS = 12;
constexpr size_t DEFLATE_HASH_ENTRIES = 1u << DEFLATE_HASH_BITS;
constexpr unsigned int DEFLATE_DICTIONARY_BYTES = 32 * 1024;

unsigned int write_deflate_byte(uzlib_comp *compressor, unsigned char value) {
    if (!compressor || !compressor->outbuf ||
        compressor->outlen < 0 ||
        static_cast<size_t>(compressor->outlen) >=
            static_cast<size_t>(compressor->outsize)) {
        if (compressor) compressor->reserved[0] = 1;
        return 0;
    }

    compressor->outbuf[compressor->outlen] = value;
    return 1;
}

void append_deflate_byte(uzlib_comp *compressor, unsigned char value) {
    (void)write_deflate_byte(compressor, value);
    compressor->outlen++;
}

size_t deflate_output_capacity(size_t source_size) {
    const size_t overhead = (source_size + 7) / 8 + 64;
    if (source_size > std::numeric_limits<size_t>::max() - overhead) {
        return 0;
    }
    return source_size + overhead;
}

}  // namespace
#endif

ReportPayloadDeflater::~ReportPayloadDeflater() {
    reset();
}

bool ReportPayloadDeflater::start(
    const ReportArtifactDescriptor &artifact,
    std::shared_ptr<const LargeByteBuffer> source,
    size_t psram_reserve) {
    reset();
    if (!artifact.valid() || !source || source->size() != artifact.size ||
        source->size() < AC_REPORT_HTTP_DEFLATE_MIN_BYTES) {
        return false;
    }

#ifdef ARDUINO
    const size_t output_capacity = deflate_output_capacity(source->size());
    const size_t compressor_bytes = sizeof(uzlib_comp);
    const size_t hash_bytes =
        DEFLATE_HASH_ENTRIES * sizeof(uzlib_hash_entry_t);
    const MemoryStatus memory = Memory::status();
    if (output_capacity == 0 || !memory.psram_available ||
        memory.psram_free < output_capacity + compressor_bytes + hash_bytes +
                                psram_reserve) {
        return false;
    }

    compressor_ = Memory::calloc_large(1, compressor_bytes, false);

    void *hash_table = Memory::calloc_large(
        DEFLATE_HASH_ENTRIES, sizeof(uzlib_hash_entry_t), false);

    output_ = LargeByteBuffer::allocate(output_capacity);
    if (!compressor_ || !hash_table || !output_) {
        if (hash_table) Memory::free(hash_table);
        reset();
        return false;
    }

    uzlib_comp *compressor = static_cast<uzlib_comp *>(compressor_);
    compressor->outbuf = output_->data();
    compressor->outsize = static_cast<int>(output_capacity);
    compressor->outlen = 2;
    compressor->hash_table =
        static_cast<uzlib_hash_entry_t *>(hash_table);
    compressor->hash_bits = DEFLATE_HASH_BITS;
    compressor->dict_size = DEFLATE_DICTIONARY_BYTES;
    compressor->writeDestByte = write_deflate_byte;
    compressor->checksum = 1;

    output_->data()[0] = 0x78;
    output_->data()[1] = 0x01;
    zlib_start_block(compressor);

    artifact_ = artifact;
    source_ = std::move(source);
    output_offset_ = 2;
    state_ = State::Compressing;
    return true;
#else
    (void)psram_reserve;
    return false;
#endif
}

bool ReportPayloadDeflater::poll(size_t input_budget) {
    if (!active() || input_budget == 0) return false;

#ifdef ARDUINO
    uzlib_comp *compressor = static_cast<uzlib_comp *>(compressor_);
    const size_t remaining = source_->size() - source_offset_;
    const size_t input_size = std::min(remaining, input_budget);
    const bool final_input = input_size == remaining;

    compressor->checksum = uzlib_adler32(
        source_->data() + source_offset_,
        static_cast<unsigned int>(input_size),
        compressor->checksum);

    uzlib_compress(compressor,
                   source_->data() + source_offset_,
                   static_cast<unsigned int>(input_size));
    if (final_input) {
        zlib_finish_block(compressor);

        const uint32_t checksum = compressor->checksum;
        append_deflate_byte(compressor, checksum >> 24);
        append_deflate_byte(compressor, checksum >> 16);
        append_deflate_byte(compressor, checksum >> 8);
        append_deflate_byte(compressor, checksum);
    }

    source_offset_ += input_size;
    output_offset_ = static_cast<size_t>(compressor->outlen);

    if (compressor->reserved[0] != 0 ||
        output_offset_ > output_->size()) {
        fail();
    } else if (final_input) {
        finish();
    }
    return true;
#else
    (void)input_budget;
    fail();
    return true;
#endif
}

void ReportPayloadDeflater::finish() {
    const size_t source_size = source_->size();
    const bool smaller = output_offset_ < source_size;
    const size_t saved = smaller ? source_size - output_offset_ : 0;
    const size_t minimum_saved = std::max(
        static_cast<size_t>(256),
        source_size * AC_REPORT_DEFLATE_MIN_SAVINGS_PERCENT / 100);

    if (!smaller || saved < minimum_saved) {
        output_.reset();
    } else {
        std::unique_ptr<LargeByteBuffer> compact =
            LargeByteBuffer::allocate(output_offset_);

        if (compact) {
            memcpy(compact->data(), output_->data(), output_offset_);
            completed_ = LargeByteBuffer::freeze(std::move(compact));
        }
        output_.reset();
    }

    source_.reset();
    release_compressor();
    state_ = State::Finished;
}

void ReportPayloadDeflater::fail() {
    source_.reset();
    output_.reset();
    completed_.reset();
    release_compressor();
    state_ = State::Finished;
}

void ReportPayloadDeflater::release_compressor() {
#ifdef ARDUINO
    if (compressor_) {
        uzlib_comp *compressor = static_cast<uzlib_comp *>(compressor_);
        if (compressor->hash_table) Memory::free(compressor->hash_table);
    }
#endif
    if (compressor_) Memory::free(compressor_);
    compressor_ = nullptr;
}

void ReportPayloadDeflater::reset() {
    source_.reset();
    output_.reset();
    completed_.reset();
    release_compressor();
    artifact_ = {};
    source_offset_ = 0;
    output_offset_ = 0;
    state_ = State::Idle;
}

std::shared_ptr<const LargeByteBuffer> ReportPayloadDeflater::take_completed() {
    if (!finished()) return {};
    return std::move(completed_);
}

}  // namespace aircannect
