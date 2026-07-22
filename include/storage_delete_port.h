#pragma once

#include <stddef.h>
#include <stdint.h>

#include "storage_path.h"

namespace aircannect {

enum class StorageDeleteState : uint8_t {
    Idle,
    Deleting,
    Done,
    Error,
};

const char *storage_delete_state_name(StorageDeleteState state);

struct StorageDeleteStatus {
    StorageDeleteState state = StorageDeleteState::Idle;
    uint32_t id = 0;
    char base_path[AC_STORAGE_PATH_MAX] = {};
    char error[AC_STORAGE_ERROR_MAX] = {};
    uint32_t roots = 0;
    uint32_t roots_done = 0;
    uint32_t files_deleted = 0;
    uint32_t dirs_deleted = 0;
    uint32_t started_ms = 0;
    uint32_t updated_ms = 0;
};

class StorageDeletePort {
public:
    virtual ~StorageDeletePort() = default;

    // delete requests
    virtual bool start_selected(const char *base_path,
                                const char *const *names,
                                size_t count,
                                uint32_t *id_out = nullptr,
                                char *error_out = nullptr,
                                size_t error_out_size = 0) = 0;

    // status
    virtual bool status(StorageDeleteStatus &out,
                        uint32_t timeout_ms = 20) const = 0;
    virtual bool active() const = 0;
};

}  // namespace aircannect
