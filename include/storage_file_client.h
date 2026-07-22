#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>
#include <string>

#include "large_byte_buffer.h"
#include "storage_atomic_write_port.h"
#include "storage_path_port.h"
#include "storage_read_port.h"

namespace aircannect {

class StoragePreparedFile {
public:
    StoragePreparedFile() = default;
    ~StoragePreparedFile();
    StoragePreparedFile(const StoragePreparedFile &) = delete;
    StoragePreparedFile &operator=(const StoragePreparedFile &) = delete;
    StoragePreparedFile(StoragePreparedFile &&other) noexcept;
    StoragePreparedFile &operator=(StoragePreparedFile &&other) noexcept;

    bool exists() const { return exists_; }
    size_t size() const { return prepared_.length; }
    size_t read(size_t offset, uint8_t *buffer, size_t capacity) const;
    void reset();

private:
    friend class StorageFileClient;

    void adopt(StorageReadPort &port,
               StoragePreparedRead prepared,
               bool exists);
    void move_from(StoragePreparedFile &other);

    StorageReadPort *port_ = nullptr;
    StoragePreparedRead prepared_;
    bool exists_ = false;
};

struct StorageFileInfo {
    bool exists = false;
    bool directory = false;
    uint64_t size = 0;
    uint64_t modified = 0;
};

enum class StorageFileClientResult : uint8_t {
    Idle,
    Waiting,
    Ready,
    Error,
};

class StorageFileClient {
public:
    ~StorageFileClient();

    void begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               StoragePathPort &path_port);

    OperationAdmission request_stat(const char *path, uint32_t generation);
    OperationAdmission request_read(const char *path,
                                    size_t max_length,
                                    uint32_t generation);
    OperationAdmission request_replace(
        const char *path,
        std::shared_ptr<const LargeByteBuffer> bytes,
        uint32_t generation);
    OperationAdmission request_remove(const char *path, uint32_t generation);

    StorageFileClientResult poll();
    void reset();

    bool active() const;
    const StorageFileInfo &info() const { return info_; }
    const char *error() const { return error_; }
    StoragePreparedFile take_file();

private:
    enum class Operation : uint8_t {
        None,
        Stat,
        Read,
        Replace,
        Remove,
    };

    enum class Phase : uint8_t {
        Idle,
        WaitStat,
        SubmitRead,
        WaitRead,
        WaitWrite,
        WaitRemove,
        Ready,
        Error,
    };

    OperationAdmission request_path_info(const char *path,
                                         size_t max_length,
                                         uint32_t generation,
                                         Operation operation);
    StorageFileClientResult poll_stat();
    StorageFileClientResult submit_read();
    StorageFileClientResult poll_read();
    StorageFileClientResult poll_write();
    StorageFileClientResult poll_remove();
    StorageFileClientResult fail(const char *error);

    StorageReadPort *read_port_ = nullptr;
    StorageAtomicWritePort *write_port_ = nullptr;
    StoragePathPort *path_port_ = nullptr;

    Operation operation_ = Operation::None;
    Phase phase_ = Phase::Idle;
    OperationTicket ticket_;
    uint32_t generation_ = 0;
    size_t max_length_ = 0;
    std::string path_;
    StorageFileInfo info_;
    StoragePreparedFile file_;
    char error_[AC_STORAGE_ERROR_MAX] = {};
};

}  // namespace aircannect
