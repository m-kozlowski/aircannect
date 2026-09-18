#pragma once

#include <memory>
#include <stdint.h>

#include "storage_scan_port.h"

namespace aircannect {

struct EdfRecordingOverviewSnapshot {
    uint32_t revision = 0;
    bool ready = false;
    bool refreshing = true;
    char session[16] = {};
    uint8_t file_count = 0;
    uint64_t bytes = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

// Storage-task owned. HTTP and main read only the published value copy.
class EdfRecordingOverview {
public:
    explicit EdfRecordingOverview(StorageScanPort &scan) : scan_(scan) {}

    bool poll(bool mounted, bool allow_scan, uint32_t now_ms);
    void note_file(const char *path, uint64_t bytes);
    void path_changed(const char *path);
    const EdfRecordingOverviewSnapshot &snapshot() const { return snapshot_; }

private:
    enum class Phase : uint8_t { Idle, Root, SelectDay, Day };

    struct Session {
        char stamp[16] = {};
        uint64_t sizes[6] = {};
        uint8_t present = 0;

        bool note_file(const char *path, uint64_t bytes);
    };

    void request_refresh();
    void publish_session(bool refreshing);
    void fail(const char *error, uint32_t now_ms);
    bool discard_scan();
    bool request_scan(uint32_t now_ms);
    void select_day();
    void read_day();

    StorageScanPort &scan_;
    EdfRecordingOverviewSnapshot snapshot_;
    Session latest_;
    Session candidate_;

    Phase phase_ = Phase::Root;
    bool mounted_ = false;
    bool restart_ = true;
    uint32_t retry_at_ms_ = 0;
    uint32_t generation_ = 0;
    OperationTicket ticket_;
    std::shared_ptr<const StorageScanSnapshot> days_;
    std::shared_ptr<const StorageScanSnapshot> files_;
    size_t cursor_ = 0;
    char day_[9] = {};
    char next_day_[9] = {};
};

}  // namespace aircannect
