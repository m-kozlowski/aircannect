#pragma once

#include <stdint.h>
#include <string>

#include "operation_outcome.h"
#include "storage_path.h"

namespace aircannect {

enum class StoragePathOperation : uint8_t {
    Stat,
    EnsureDirectory,
    MoveReplacing,
    Remove,
};

struct StoragePathCommand {
    StoragePathOperation operation = StoragePathOperation::Stat;
    std::string source;
    std::string destination;
    uint32_t generation = 0;

    bool valid() const {
        if (generation == 0 || !storage_user_path_valid(source.c_str())) {
            return false;
        }
        if (operation == StoragePathOperation::Stat ||
            operation == StoragePathOperation::EnsureDirectory) {
            return destination.empty();
        }
        if (operation == StoragePathOperation::Remove) {
            return destination.empty() && source != "/";
        }
        return storage_user_path_valid(destination.c_str()) &&
               source != destination && source != "/" &&
               destination != "/";
    }
};

struct StoragePathCompletion {
    OperationTicket ticket;
    OperationOutcome outcome;
    bool exists = false;
    bool directory = false;
    uint64_t size = 0;
    uint64_t modified = 0;
    char error[AC_STORAGE_ERROR_MAX] = {};
};

class StoragePathPort {
public:
    virtual ~StoragePathPort() = default;

    virtual OperationSubmission request(const StoragePathCommand &command) = 0;
    virtual bool abandon(OperationTicket ticket) = 0;
    virtual bool take_completion(
        OperationTicket ticket,
        StoragePathCompletion &completion) = 0;
};

}  // namespace aircannect
