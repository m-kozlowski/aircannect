#pragma once

#include <stddef.h>
#include <stdint.h>

#include "storage_path.h"

namespace aircannect {

static constexpr size_t AC_SLEEPHQ_SECRET_MAX = 193;
static constexpr size_t AC_SLEEPHQ_ID_MAX = 21;
static constexpr size_t AC_SLEEPHQ_ERROR_MAX = 96;
static constexpr size_t AC_SLEEPHQ_STATUS_MAX = 32;
static constexpr size_t AC_SLEEPHQ_SERIAL_MAX = 33;
static constexpr size_t AC_SLEEPHQ_HTTP_RESPONSE_MAX = 16 * 1024;
static constexpr size_t AC_SLEEPHQ_CONTENT_HASH_MAX = 33;

enum class SleepHqImportStatusKind : uint8_t {
    Success,
    Transient,
    Failure,
    Unknown,
};

struct SleepHqRemoteFile {
    uint32_t id = 0;
    uint64_t size = 0;
    char name[AC_STORAGE_NAME_MAX] = {};
    char path[AC_STORAGE_PATH_MAX] = {};
    char content_hash[AC_SLEEPHQ_CONTENT_HASH_MAX] = {};
};

struct SleepHqMachine {
    uint32_t id = 0;
    char serial_number[AC_SLEEPHQ_SERIAL_MAX] = {};
};

struct SleepHqMachineDate {
    uint32_t id = 0;
    uint32_t machine_id = 0;
    char date[11] = {};
};

using SleepHqRemoteFileCallback =
    bool (*)(void *ctx, const SleepHqRemoteFile &file);
using SleepHqMachineCallback =
    bool (*)(void *ctx, const SleepHqMachine &machine);

SleepHqImportStatusKind sleephq_classify_import_status(const char *status);

bool sleephq_parse_remote_file_list_json(const char *json,
                                         uint32_t per_page,
                                         SleepHqRemoteFileCallback callback,
                                         void *ctx,
                                         size_t &count,
                                         bool &has_more,
                                         char *error,
                                         size_t error_size);

bool sleephq_parse_machine_list_json(const char *json,
                                     uint32_t per_page,
                                     SleepHqMachineCallback callback,
                                     void *ctx,
                                     size_t &count,
                                     bool &has_more,
                                     char *error,
                                     size_t error_size);

bool sleephq_parse_machine_date_json(const char *json,
                                     SleepHqMachineDate &out,
                                     char *error,
                                     size_t error_size);

}  // namespace aircannect
