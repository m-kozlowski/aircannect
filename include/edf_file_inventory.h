#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_reader.h"

namespace aircannect {

enum class EdfInventoryStatus : uint8_t {
    Ok,
    InvalidPath,
    InvalidHeader,
    FileTooSmall,
};

enum class EdfInventoryFileKind : uint8_t {
    Unknown,
    Brp,
    Pld,
    Sa2,
    Eve,
    Csl,
    Str,
};

static constexpr uint32_t AC_EDF_INVENTORY_WARN_PARTIAL_TAIL = 1u << 0;
static constexpr uint32_t AC_EDF_INVENTORY_WARN_RECORD_COUNT_MISMATCH =
    1u << 1;

struct EdfInventoryEntry {
    EdfInventoryStatus status = EdfInventoryStatus::InvalidPath;
    EdfInventoryFileKind kind = EdfInventoryFileKind::Unknown;
    EdfHeaderSummary header;
    size_t file_size = 0;
    size_t data_size = 0;
    size_t complete_records_from_size = 0;
    size_t partial_tail_bytes = 0;
    uint32_t warnings = 0;
    char sleep_day[9] = {};
    char session_stamp[16] = {};
    char tag[4] = {};
};

const char *edf_inventory_file_kind_name(EdfInventoryFileKind kind);
const char *edf_inventory_status_name(EdfInventoryStatus status);
bool edf_inventory_describe_path(const char *path, EdfInventoryEntry &out);

EdfInventoryStatus edf_inventory_describe_file(const char *path,
                                               const uint8_t *header,
                                               size_t header_size,
                                               size_t file_size,
                                               EdfInventoryEntry &out);

}  // namespace aircannect
