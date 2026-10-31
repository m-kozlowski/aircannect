#pragma once

#include "edf_file_writer.h"
#include "operation_outcome.h"
#include "sleep_day_id.h"
#include "storage_path.h"

namespace aircannect {

// Borrowed inputs are copied before request_write returns.
struct StorageEdfStrWriteCommand {
    const char *path = nullptr;
    EdfHeaderInfo header;
    EdfStrRecordView record;
    bool replace_existing = false;
    SleepDayId changed_day;
    uint32_t generation = 0;
};

struct StorageEdfStrWriteCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class StorageEdfStrPort {
public:
    virtual ~StorageEdfStrPort() = default;

    virtual OperationSubmission request_write(
        const StorageEdfStrWriteCommand &command) = 0;
    virtual bool take_completion(OperationTicket ticket,
                                 StorageEdfStrWriteCompletion &completion) = 0;
};

}  // namespace aircannect
