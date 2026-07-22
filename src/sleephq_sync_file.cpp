#include "sleephq_sync_file.h"

#include <esp_rom_md5.h>
#include <string.h>

#include "debug_log.h"
#include "memory_manager.h"
#include "storage_export_state.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace {

static constexpr size_t HASH_BUFFER_BYTES = 4096;

void digest_to_hex(const uint8_t digest[16],
                   char out[AC_SLEEPHQ_CONTENT_HASH_MAX]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = HEX_DIGITS[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0f];
    }
    out[32] = '\0';
}

}  // namespace

bool SleepHqSyncFile::UploadReader::read(uint8_t *out, size_t length,
                                         size_t &read) {
    read = 0;
    if (!out || abort_requested_.load() || !file_.local_open_) return false;

    Storage::Guard guard;
    read = file_.local_.read(out, length);
    return read == length;
}

bool SleepHqSyncFile::UploadReader::rewind() {
    if (!file_.local_open_) return false;

    Storage::Guard guard;
    return file_.local_.seek(0);
}

bool SleepHqSyncFile::UploadReader::read_callback(void *ctx, uint8_t *out,
                                                  size_t length, size_t &read) {
    UploadReader *reader = static_cast<UploadReader *>(ctx);
    return reader && reader->read(out, length, read);
}

bool SleepHqSyncFile::UploadReader::reset_callback(void *ctx) {
    UploadReader *reader = static_cast<UploadReader *>(ctx);
    return reader && reader->rewind();
}

SleepHqSyncFile::~SleepHqSyncFile() {
    close();
}

void SleepHqSyncFile::configure(const char *path, const char *sleep_path,
                                const char *name, const char *state_path,
                                uint64_t size, uint64_t mtime,
                                StorageExportStateWriteMode state_write_mode,
                                bool attach_by_hash) {
    reset();

    copy_cstr(state_.path, sizeof(state_.path), path);
    copy_cstr(state_.sleep_path, sizeof(state_.sleep_path), sleep_path);
    copy_cstr(state_.name, sizeof(state_.name), name);
    copy_cstr(state_.state_path, sizeof(state_.state_path), state_path);
    state_.size = size;
    state_.mtime = mtime;
    state_.state_write_mode = state_write_mode;
    state_.attach_by_hash = attach_by_hash;
}

void SleepHqSyncFile::reset() {
    close();
    state_ = SleepHqSyncFileState{};
}

void SleepHqSyncFile::set_content_hash(const char *content_hash) {
    copy_cstr(state_.content_hash, sizeof(state_.content_hash), content_hash);
}

bool SleepHqSyncFile::open_candidate(File &out) const {
    Storage::Guard guard;
    out = Storage::open(state_.path, "r");
    if (out && !out.isDirectory()) return true;

    if (out) out.close();
    return false;
}

void SleepHqSyncFile::adopt(File &file) {
    close();
    local_ = file;
    file = File();
    local_open_ = true;
}

void SleepHqSyncFile::close() {
    if (!local_open_) return;

    Storage::Guard guard;
    local_.close();
    local_open_ = false;
}

bool SleepHqSyncFile::matches_snapshot() const {
    const StorageLocalNodeInfo info = storage_stat_local_node(state_.path);
    return info.exists && !info.is_dir && info.size == state_.size &&
           info.mtime == state_.mtime;
}

bool SleepHqSyncFile::compute_content_hash(char *out, size_t out_size) {
    if (!out || out_size < AC_SLEEPHQ_CONTENT_HASH_MAX || !local_open_ ||
        !state_.name[0]) {
        return false;
    }

    uint8_t *buffer = static_cast<uint8_t *>(
        Memory::alloc_large(HASH_BUFFER_BYTES, false));
    if (!buffer) {
        Log::logf(CAT_SLEEPHQ, LOG_ERROR,
                  "hash buffer allocation failed bytes=%u\n",
                  static_cast<unsigned>(HASH_BUFFER_BYTES));
        return false;
    }

    md5_context_t md5;
    esp_rom_md5_init(&md5);
    uint64_t read_total = 0;
    bool ok = true;
    {
        Storage::Guard guard;
        ok = local_.seek(0);
    }

    while (ok && read_total < state_.size) {
        const uint64_t remaining = state_.size - read_total;
        const size_t wanted = remaining > HASH_BUFFER_BYTES
            ? HASH_BUFFER_BYTES
            : static_cast<size_t>(remaining);
        size_t read = 0;
        {
            Storage::Guard guard;
            read = local_.read(buffer, wanted);
        }
        if (read != wanted) {
            ok = false;
            break;
        }

        esp_rom_md5_update(&md5, buffer, read);
        read_total += read;
        taskYIELD();
    }
    Memory::free(buffer);
    if (!ok) return false;

    esp_rom_md5_update(&md5,
                       reinterpret_cast<const uint8_t *>(state_.name),
                       strlen(state_.name));
    uint8_t digest[16];
    esp_rom_md5_final(digest, &md5);
    digest_to_hex(digest, out);

    {
        Storage::Guard guard;
        ok = local_.seek(0);
    }
    return ok;
}

}  // namespace aircannect
