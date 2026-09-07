#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "large_text_buffer.h"
#include "operation_outcome.h"
#include "published_json_snapshot.h"
#include "storage_atomic_write_port.h"
#include "storage_bounded_file_loader.h"

namespace aircannect {

class StorageReadPort;

class ReportPreferencesService {
public:
    bool begin(StorageReadPort &read_port,
               StorageAtomicWritePort &write_port);
    void poll();

    OperationAdmission update(const char *json,
                              size_t length,
                              uint32_t request_id);

    const PublishedJsonSnapshot &snapshot() const { return snapshot_; }
    bool ready_for_update() const;

private:
    enum class Phase : uint8_t {
        LoadStart,
        Loading,
        Ready,
        WriteStart,
        Writing,
    };

    bool set_defaults(const char *load_error = nullptr);
    bool accept_loaded(const uint8_t *bytes, size_t length);
    bool prepare_update(const char *json,
                        size_t length,
                        uint32_t request_id,
                        const char *&error);
    void complete_update(bool success, const char *error = nullptr);
    bool publish_snapshot();
    uint32_t next_storage_generation();

    StorageAtomicWritePort *write_port_ = nullptr;
    StorageBoundedFileLoader loader_;
    OperationTicket write_ticket_;
    std::shared_ptr<const LargeByteBuffer> write_bytes_;
    LargeTextBuffer preferences_json_;
    LargeTextBuffer writing_json_;
    PublishedJsonSnapshot snapshot_;
    Phase phase_ = Phase::LoadStart;
    uint32_t preferences_revision_ = 1;
    uint32_t pending_revision_ = 0;
    uint32_t storage_generation_ = 0;
    uint32_t update_request_id_ = 0;
    bool stored_ = false;
    bool snapshot_dirty_ = false;
    bool last_update_valid_ = false;
    bool last_update_ok_ = false;
    char load_error_[48] = {};
    char update_error_[48] = {};
};

}  // namespace aircannect
