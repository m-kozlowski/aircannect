#pragma once

#include "storage_browser_port.h"

namespace aircannect {

class StorageDirectoryListing;
class StorageDownloadProducer;

enum class StorageBrowserStep : uint8_t {
    Idle,
    Waiting,
    Working,
};

class StorageBrowserService final : public StorageBrowserPort {
public:
    using WakeCallback = void (*)();

    ~StorageBrowserService();

    bool begin(WakeCallback wake);
    void set_task_available(bool available);
    StorageBrowserStep step();

    StorageListingRead listing(
        const char *path,
        bool refresh,
        std::shared_ptr<const StorageDirectorySnapshot> &snapshot_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) override;

    StorageDownloadPrepareState prepare_download(
        const char *path,
        StorageDownloadPrepareStatus &status_out) override;
    bool begin_download(
        uint32_t id,
        std::shared_ptr<StoragePreparedDownload> &download_out,
        char *filename_out,
        size_t filename_out_size,
        uint64_t &size_out,
        char *error_out = nullptr,
        size_t error_out_size = 0) override;
    PreparedByteRead read_download(StoragePreparedDownload &download,
                                   uint8_t *buffer,
                                   size_t max_length,
                                   size_t offset) override;
    void finish_download(StoragePreparedDownload &download) override;

private:
    bool ensure_owners();
    bool ready() const;
    void wake() const;

    StorageDirectoryListing *listing_ = nullptr;
    StorageDownloadProducer *download_ = nullptr;
    WakeCallback wake_ = nullptr;
    bool task_available_ = false;
};

}  // namespace aircannect
