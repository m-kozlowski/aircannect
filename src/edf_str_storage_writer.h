#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_file_writer.h"

namespace aircannect {

enum class EdfStrStorageErrorKind : uint8_t {
    None,
    Allocation,
    Open,
    Read,
    Write,
    Publish,
};

struct EdfStrStorageWriteRequest {
    const char *path = nullptr;
    EdfHeaderInfo header;
    const uint8_t *record = nullptr;
    size_t record_size = 0;
};

struct EdfStrStorageWriteResult {
    bool success = false;
    bool timeline_rewritten = false;
    bool retention_applied = false;
    uint32_t record_count = 0;
    uint32_t filler_records = 0;
    uint32_t merged_records = 0;
    uint32_t discarded_records = 0;
    size_t bytes_written = 0;
    EdfStrStorageErrorKind error_kind = EdfStrStorageErrorKind::None;
    const char *error = nullptr;
};

bool edf_str_storage_write(const EdfStrStorageWriteRequest &request,
                           EdfStrStorageWriteResult &result);
bool edf_str_storage_recover(const char *path,
                             bool &recovered,
                             const char *&error);

}  // namespace aircannect
