#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "storage_export_plan.h"
#include "storage_read_port.h"
#include "storage_scan_port.h"

namespace aircannect {

struct StorageExportInventoryEntryView {
    const char *path = nullptr;
    StorageLocalNodeInfo info;
    size_t source_index = 0;
    uint8_t root_index = 0;
    bool local_state_complete = false;
};

enum class StorageExportInventoryKind : uint8_t {
    Catalog,
    DatalogDay,
};

class StorageExportInventoryView {
public:
    virtual ~StorageExportInventoryView() = default;

    virtual StorageExportInventoryKind kind() const {
        return StorageExportInventoryKind::Catalog;
    }
    virtual const char *loaded_datalog_day() const { return ""; }
    virtual uint32_t generation() const = 0;
    virtual size_t source_size() const = 0;
    virtual bool entry(size_t source_index,
                       StorageExportInventoryEntryView &out) const = 0;
    virtual bool find_file(const char *path,
                           StorageExportInventoryEntryView &out) const = 0;

    virtual size_t datalog_day_count() const = 0;
    virtual const char *datalog_day_at(size_t index) const = 0;
    virtual bool datalog_day_done(const char *day) const = 0;
    virtual bool datalog_day_has_pending(const char *day) const = 0;
    virtual const char *latest_datalog_day() const = 0;
    virtual const char *state_dir() const = 0;
};

class StorageExportInventory final : public StorageExportInventoryView {
public:
    ~StorageExportInventory();

    StorageExportInventoryKind kind() const override { return kind_; }
    const char *loaded_datalog_day() const override {
        return loaded_datalog_day_;
    }
    uint32_t generation() const override { return generation_; }
    size_t source_size() const override;
    bool entry(size_t source_index,
               StorageExportInventoryEntryView &out) const override;
    bool find_file(const char *path,
                   StorageExportInventoryEntryView &out) const override;

    size_t datalog_day_count() const override { return datalog_day_count_; }
    const char *datalog_day_at(size_t index) const override;
    bool datalog_day_done(const char *day) const override;
    bool datalog_day_has_pending(const char *day) const override;
    const char *latest_datalog_day() const override;
    const char *state_dir() const override { return state_dir_; }

private:
    friend class StorageExportInventoryLoader;

    struct DatalogDay {
        char name[9] = {};
        uint32_t file_count = 0;
        uint32_t pending_count = 0;
        bool done = false;
    };

    struct PathIndexEntry {
        uint32_t hash = 0;
        uint32_t source_index = 0;
    };

    static std::shared_ptr<StorageExportInventory> create_catalog(
        std::shared_ptr<const StorageScanSnapshot> scan,
        size_t export_root_count,
        size_t state_root_index,
        const char *state_dir,
        uint64_t now_epoch);
    static std::shared_ptr<StorageExportInventory> create_datalog_day(
        std::shared_ptr<const StorageScanSnapshot> scan,
        size_t state_root_index,
        size_t done_root_index,
        const char *state_dir,
        const char *day);

    bool build_sources();
    bool build_catalog_days(uint64_t now_epoch);
    bool build_datalog_day(const char *day, size_t done_root_index);
    bool mark_complete(const char *state_path,
                       const char *local_path,
                       uint64_t size,
                       uint64_t mtime);
    bool find_source_index(const char *path, size_t &source_index) const;
    DatalogDay *find_datalog_day(const char *day);
    const DatalogDay *find_datalog_day(const char *day) const;

    std::shared_ptr<const StorageScanSnapshot> scan_;
    uint32_t *source_indices_ = nullptr;
    PathIndexEntry *path_index_ = nullptr;
    uint8_t *complete_ = nullptr;
    DatalogDay *datalog_days_ = nullptr;
    size_t source_count_ = 0;
    size_t datalog_day_count_ = 0;
    size_t export_root_count_ = 0;
    size_t state_root_index_ = 0;
    uint32_t generation_ = 0;
    StorageExportInventoryKind kind_ = StorageExportInventoryKind::Catalog;
    char loaded_datalog_day_[9] = {};
    char state_dir_[AC_STORAGE_PATH_MAX] = {};
};

enum class StorageExportInventoryLoadResult : uint8_t {
    Waiting,
    Ready,
    Error,
};

class StorageExportInventoryLoader {
public:
    ~StorageExportInventoryLoader();

    void begin(StorageScanPort &scan_port, StorageReadPort &read_port);
    OperationAdmission request(const char *state_dir,
                               uint32_t generation,
                               uint64_t now_epoch = 0);
    OperationAdmission request_datalog_day(const char *day,
                                           uint32_t generation);
    StorageExportInventoryLoadResult poll(char *error_out,
                                          size_t error_out_size);
    void reset();

    bool active() const;
    std::shared_ptr<const StorageExportInventory> snapshot() const {
        return inventory_;
    }

private:
    enum class RequestKind : uint8_t {
        None,
        Catalog,
        DatalogDay,
    };

    enum class Phase : uint8_t {
        Idle,
        Scan,
        SelectStateFile,
        ReadStateFile,
        ParseStateFile,
        Ready,
        Error,
    };

    bool start_state_read(char *error_out, size_t error_out_size);
    bool finish_state_read();
    bool parse_state_bytes();
    void parse_state_line();
    bool select_next_state_file();
    bool state_file_matches_request(const char *path) const;
    void reset_operation();
    void fail(const char *error);

    StorageScanPort *scan_port_ = nullptr;
    StorageReadPort *read_port_ = nullptr;
    OperationTicket scan_ticket_;
    OperationTicket read_ticket_;
    StoragePreparedRead prepared_;
    std::shared_ptr<const StorageScanSnapshot> scan_;
    std::shared_ptr<StorageExportInventory> inventory_;
    Phase phase_ = Phase::Idle;
    size_t state_root_index_ = 0;
    size_t state_scan_index_ = 0;
    size_t state_file_size_ = 0;
    size_t prepared_offset_ = 0;
    size_t state_line_length_ = 0;
    bool state_line_overflow_ = false;
    uint32_t generation_ = 0;
    uint64_t now_epoch_ = 0;
    RequestKind request_kind_ = RequestKind::None;
    char state_dir_[AC_STORAGE_PATH_MAX] = {};
    char requested_day_[9] = {};
    char state_path_[AC_STORAGE_PATH_MAX] = {};
    char state_line_[AC_STORAGE_PATH_MAX + 96] = {};
    char error_[AC_STORAGE_ERROR_MAX] = {};
};

}  // namespace aircannect
