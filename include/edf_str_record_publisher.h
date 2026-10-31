#pragma once

#include "edf_str_session.h"
#include "fixed_queue.h"
#include "storage_edf_str_port.h"

namespace aircannect {

// Recorder-owned immutable records awaiting ordered storage publication.
class EdfStrRecordPublisher {
public:
    void begin(StorageEdfStrPort &storage) { storage_ = &storage; }
    bool publish(const EdfStrSessionAccumulator &record,
                  const EdfHeaderInfo &header,
                  bool replace_existing);
    void poll(uint32_t now_ms);

    bool idle() const { return active_.generation == 0 && pending_.empty(); }
    const char *last_error() const { return last_error_; }

private:
    struct Publication {
        EdfStrSessionAccumulator record;
        char patient_id[81] = {};
        char recording_id[97] = {};
        char date[9] = {};
        char time[9] = {};
        bool replace_existing = false;
        uint32_t generation = 0;
    };

    StorageEdfStrPort *storage_ = nullptr;
    FixedQueue<Publication, 4> pending_;
    Publication active_;
    OperationTicket ticket_;
    uint32_t generation_ = 0;
    uint32_t retry_at_ms_ = 0;
    char last_error_[AC_STORAGE_ERROR_MAX] = {};
};

}  // namespace aircannect
