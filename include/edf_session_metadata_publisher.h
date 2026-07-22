#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_session_metadata.h"
#include "fixed_queue.h"
#include "storage_atomic_write_port.h"

namespace aircannect {

struct EdfSessionMetadataPublication {
    uint32_t generation = 0;

    bool valid() const { return generation != 0; }
};

class EdfSessionMetadataPublisher {
public:
    void begin(StorageAtomicWritePort &storage);

    EdfSessionMetadataPublication publish(const EdfSessionMetadata &metadata);
    void poll(uint32_t now_ms);

    bool completed(EdfSessionMetadataPublication publication) const;
    const char *last_error() const { return last_error_; }

private:
    struct PendingPublication {
        EdfSessionMetadata metadata;
        uint32_t generation = 0;
    };

    uint32_t next_generation();
    bool load_next();
    void submit_active(uint32_t now_ms);
    void poll_active(uint32_t now_ms);
    void retry_active(uint32_t now_ms, const char *error);

    StorageAtomicWritePort *storage_ = nullptr;
    FixedQueue<PendingPublication, 4> pending_;
    PendingPublication active_;
    OperationTicket ticket_;

    uint32_t generation_ = 0;
    uint32_t completed_generation_ = 0;
    uint32_t retry_at_ms_ = 0;
    char last_error_[AC_STORAGE_ERROR_MAX] = {};
};

}  // namespace aircannect
