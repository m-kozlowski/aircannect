#pragma once

#include <stddef.h>
#include <stdint.h>

#include "large_text_buffer.h"
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

class SleepHqRemoteFileListStreamParser {
public:
    bool begin(uint32_t per_page, SleepHqRemoteFileCallback callback,
               void *ctx);
    bool feed(const uint8_t *data, size_t size);
    bool finish(size_t &count, bool &has_more,
                char *error, size_t error_size);

private:
    enum class Stage : uint8_t {
        SeekData,
        SeekArray,
        SeekItem,
        CaptureItem,
        Done,
        Failed,
    };

    bool feed_seek_data(char ch);
    bool feed_seek_array(char ch);
    bool feed_seek_item(char ch);
    bool feed_capture_item(char ch);
    bool publish_item();
    bool fail(const char *error);

    uint32_t per_page_ = 0;
    SleepHqRemoteFileCallback callback_ = nullptr;
    void *ctx_ = nullptr;
    size_t count_ = 0;

    Stage stage_ = Stage::SeekData;
    size_t container_depth_ = 0;
    size_t item_depth_ = 0;
    bool in_string_ = false;
    bool escaped_ = false;
    bool capture_key_ = false;
    bool data_key_candidate_ = false;
    char key_[8] = {};
    size_t key_length_ = 0;
    LargeTextBuffer item_;
    char error_[AC_SLEEPHQ_ERROR_MAX] = {};
};

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
