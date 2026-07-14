#include "report_store.h"

#include <Arduino.h>
#include <stdio.h>

#include "board.h"
#include "crc32.h"
#include "debug_log.h"
#include "report_store_internal.h"
#include "storage_manager.h"
#include "string_util.h"

namespace aircannect {
namespace ReportStoreInternal {
namespace {

void integrity_note_error(ReportStoreIntegrityResult &out,
                          const char *error) {
    out.errors++;
    copy_cstr(out.last_error, sizeof(out.last_error), error);
}

}  // namespace

bool check_summary_integrity(ReportStoreIntegrityResult &out, bool repair) {
    char path[REPORT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/summary/nights.idx", BASE_DIR);
    if (!Storage::exists(path)) return true;
    out.summary_checked = 1;

    File file = Storage::open(path, "r");
    if (!file) {
        integrity_note_error(out, "summary_open_failed");
        return false;
    }

    uint8_t header[SUMMARY_HEADER_SIZE];
    uint32_t record_count = 0;
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
    bool ok = read_all(file, header, sizeof(header)) &&
              decode_summary_header(header,
                                    record_count,
                                    payload_len,
                                    payload_crc);

    ReportSpoolBuffer payload;
    if (ok && payload_len && !payload.reserve_capacity(payload_len)) ok = false;

    if (ok && payload_len) {
        size_t offset = 0;
        uint8_t *dst = payload.append_uninitialized(payload_len, offset);
        ok = dst && read_all(file, dst, payload_len);
    }

    file.close();

    if (ok) {
        const uint32_t actual_crc = payload.size()
            ? crc32_ieee(payload.data(), payload.size())
            : crc32_ieee(nullptr, 0);
        ok = actual_crc == payload_crc;
    }

    for (uint32_t i = 0; ok && i < record_count; ++i) {
        ReportSummaryRecord record;
        ok = report_summary_record_decode(payload.data() +
                                              i * SUMMARY_RECORD_SIZE,
                                          SUMMARY_RECORD_SIZE,
                                          record);
    }

    if (ok) return true;

    out.summary_invalid++;
    if (!repair) return true;

    if (!Storage::remove(path)) {
        integrity_note_error(out, "summary_remove_failed");
        return false;
    }

    out.summary_removed = 1;
    out.repaired = true;
    return true;
}

}  // namespace ReportStoreInternal

namespace ReportStore {

using namespace ReportStoreInternal;

bool write_summary_records(const ReportSummaryRecord *records,
                           size_t count) {
    Storage::Guard g;

    if (!records && count) {
        note_error("bad_summary_records", &current.write_errors);
        return false;
    }

    if (count > UINT32_MAX ||
        count > (AC_REPORT_MAX_PAYLOAD_BYTES / SUMMARY_RECORD_SIZE)) {
        note_error("summary_too_large", &current.write_errors);
        return false;
    }

    if (!ensure_layout()) return false;

    ReportSpoolBuffer payload;
    payload.set_max_size(AC_REPORT_MAX_PAYLOAD_BYTES);

    for (size_t i = 0; i < count; ++i) {
        if (!append_summary_record(payload, records[i])) {
            note_error("summary_encode_failed", &current.write_errors);
            return false;
        }
    }

    char final_path[REPORT_PATH_MAX];
    char tmp_path[REPORT_PATH_MAX];
    snprintf(final_path, sizeof(final_path), "%s/summary/nights.idx",
             BASE_DIR);
    snprintf(tmp_path, sizeof(tmp_path), "%s/summary/nights.idx.tmp",
             BASE_DIR);

    Storage::remove(tmp_path);

    File file = Storage::open(tmp_path, "w");
    if (!file) {
        note_error("summary_open_failed", &current.write_errors);
        return false;
    }

    uint8_t header[SUMMARY_HEADER_SIZE];
    const uint32_t crc = payload.size()
        ? crc32_ieee(payload.data(), payload.size())
        : crc32_ieee(nullptr, 0);

    encode_summary_header(header,
                          static_cast<uint32_t>(count),
                          static_cast<uint32_t>(payload.size()),
                          crc);

    bool ok = write_all(file, header, sizeof(header)) &&
              write_all(file, payload.data(), payload.size());
    file.close();

    if (!ok) {
        Storage::remove(tmp_path);
        note_error("summary_write_failed", &current.write_errors);
        return false;
    }

    Storage::remove(final_path);

    if (!Storage::rename(tmp_path, final_path)) {
        Storage::remove(tmp_path);
        note_error("summary_commit_failed", &current.write_errors);
        return false;
    }

    current.summary_records_written += static_cast<uint32_t>(count);
    current.bytes_written += payload.size();
    set_error(current.last_error, sizeof(current.last_error), "");

    Log::logf(CAT_REPORT, LOG_DEBUG,
              "stored summary records=%lu bytes=%lu\n",
              static_cast<unsigned long>(count),
              static_cast<unsigned long>(payload.size()));

    return true;
}

bool read_summary_records(ReportSummaryRecordCallback callback,
                          void *context) {
    Storage::Guard g;

    if (!callback) {
        note_error("missing_summary_callback", &current.read_errors);
        return false;
    }

    char path[REPORT_PATH_MAX];
    snprintf(path, sizeof(path), "%s/summary/nights.idx", BASE_DIR);
    if (!ensure_layout() || !Storage::exists(path)) return false;

    File file = Storage::open(path, "r");
    if (!file) {
        note_error("summary_open_failed", &current.read_errors);
        return false;
    }

    uint8_t header[SUMMARY_HEADER_SIZE];
    uint32_t record_count = 0;
    uint32_t payload_len = 0;
    uint32_t payload_crc = 0;
    bool ok = read_all(file, header, sizeof(header)) &&
              decode_summary_header(header,
                                    record_count,
                                    payload_len,
                                    payload_crc);

    ReportSpoolBuffer payload;
    if (ok && payload_len && !payload.reserve_capacity(payload_len)) {
        ok = false;
    }

    if (ok && payload_len) {
        size_t offset = 0;
        uint8_t *dst = payload.append_uninitialized(payload_len, offset);
        ok = dst && read_all(file, dst, payload_len);
    }

    file.close();

    if (!ok) {
        note_error("summary_read_failed", &current.read_errors);
        return false;
    }

    const uint32_t actual_crc = payload.size()
        ? crc32_ieee(payload.data(), payload.size())
        : crc32_ieee(nullptr, 0);
    if (actual_crc != payload_crc) {
        note_error("summary_crc_failed", &current.read_errors);
        return false;
    }

    for (uint32_t i = 0; i < record_count; ++i) {
        ReportSummaryRecord record;
        const uint8_t *raw = payload.data() + i * SUMMARY_RECORD_SIZE;
        if (!report_summary_record_decode(raw, SUMMARY_RECORD_SIZE, record)) {
            note_error("summary_record_invalid", &current.read_errors);
            return false;
        }

        if (!callback(context, record)) {
            note_error("summary_callback_rejected", &current.read_errors);
            return false;
        }
    }

    current.summary_records_read += record_count;
    current.bytes_read += payload.size();
    set_error(current.last_error, sizeof(current.last_error), "");

    return true;
}

}  // namespace ReportStore
}  // namespace aircannect
