#pragma once

#include <FS.h>
#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "sleephq_protocol.h"
#include "storage_export_plan.h"
#include "storage_path.h"

namespace aircannect {

struct SleepHqSyncFileState {
    char path[AC_STORAGE_PATH_MAX] = {};
    char sleep_path[AC_STORAGE_PATH_MAX] = {};
    char name[AC_STORAGE_NAME_MAX] = {};
    char state_path[AC_STORAGE_PATH_MAX] = {};
    char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
    uint64_t size = 0;
    uint64_t mtime = 0;
    StorageExportStateWriteMode state_write_mode =
        StorageExportStateWriteMode::Append;
    bool attach_by_hash = false;
};

class SleepHqSyncFile {
public:
    class UploadReader {
    public:
        UploadReader(SleepHqSyncFile &file, std::atomic<bool> &abort_requested)
            : file_(file), abort_requested_(abort_requested) {}

        bool read(uint8_t *out, size_t length, size_t &read);
        bool rewind();

        static bool read_callback(void *ctx, uint8_t *out, size_t length, size_t &read);
        static bool reset_callback(void *ctx);

    private:
        SleepHqSyncFile &file_;
        std::atomic<bool> &abort_requested_;
    };

    SleepHqSyncFile() = default;
    ~SleepHqSyncFile();
    SleepHqSyncFile(const SleepHqSyncFile &) = delete;
    SleepHqSyncFile &operator=(const SleepHqSyncFile &) = delete;

    void configure(const char *path,
                   const char *sleep_path,
                   const char *name,
                   const char *state_path,
                   uint64_t size,
                   uint64_t mtime,
                   StorageExportStateWriteMode state_write_mode,
                   bool attach_by_hash);
    void reset();

    const SleepHqSyncFileState &state() const { return state_; }
    bool has_content_hash() const { return state_.content_hash[0] != '\0'; }
    void set_content_hash(const char *content_hash);
    void set_attach_by_hash(bool attach) { state_.attach_by_hash = attach; }

    bool open_candidate(File &out) const;
    void adopt(File &file);
    void close();
    bool matches_snapshot() const;
    bool compute_content_hash(char *out, size_t out_size);

private:
    SleepHqSyncFileState state_;
    File local_;
    bool local_open_ = false;
};

}  // namespace aircannect
