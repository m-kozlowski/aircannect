#include "edf_file_inventory.h"

#include <string.h>

#include "edf_identification.h"
#include "edf_storage_catalog.h"

namespace aircannect {
namespace {

EdfInventoryFileKind kind_from_tag(const char *tag) {
    if (!tag) return EdfInventoryFileKind::Unknown;
    if (strncmp(tag, "BRP", 3) == 0) return EdfInventoryFileKind::Brp;
    if (strncmp(tag, "PLD", 3) == 0) return EdfInventoryFileKind::Pld;
    if (strncmp(tag, "SA2", 3) == 0) return EdfInventoryFileKind::Sa2;
    if (strncmp(tag, "TCV", 3) == 0) return EdfInventoryFileKind::Tcv;
    if (strncmp(tag, "EVE", 3) == 0) return EdfInventoryFileKind::Eve;
    if (strncmp(tag, "CSL", 3) == 0) return EdfInventoryFileKind::Csl;
    return EdfInventoryFileKind::Unknown;
}

void copy_fixed(char *dst, size_t dst_size, const char *src, size_t len) {
    if (!dst || dst_size == 0) return;
    dst[0] = 0;
    if (!src) return;
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

bool parse_path(const char *path, EdfInventoryEntry &out) {
    if (!edf_valid_pull_path(path)) return false;
    if (strcmp(path, "/STR.edf") == 0) {
        out.kind = EdfInventoryFileKind::Str;
        copy_fixed(out.tag, sizeof(out.tag), "STR", 3);
        return true;
    }
    if (strcmp(path, AC_EDF_IDENTIFICATION_JSON_PATH) == 0) {
        out.kind = EdfInventoryFileKind::IdentificationJson;
        copy_fixed(out.tag, sizeof(out.tag), "IDJ", 3);
        return true;
    }
    if (strcmp(path, AC_EDF_IDENTIFICATION_CRC_PATH) == 0) {
        out.kind = EdfInventoryFileKind::IdentificationCrc;
        copy_fixed(out.tag, sizeof(out.tag), "IDC", 3);
        return true;
    }

    copy_fixed(out.sleep_day, sizeof(out.sleep_day), path + 9, 8);
    copy_fixed(out.session_stamp, sizeof(out.session_stamp), path + 18, 15);
    copy_fixed(out.tag, sizeof(out.tag), path + 34, 3);
    out.kind = kind_from_tag(out.tag);
    return out.kind != EdfInventoryFileKind::Unknown;
}

bool metadata_kind(EdfInventoryFileKind kind) {
    return kind == EdfInventoryFileKind::IdentificationJson ||
           kind == EdfInventoryFileKind::IdentificationCrc;
}

}  // namespace

bool edf_inventory_describe_path(const char *path, EdfInventoryEntry &out) {
    out = {};
    if (!parse_path(path, out)) {
        out.status = EdfInventoryStatus::InvalidPath;
        return false;
    }
    out.status = EdfInventoryStatus::Ok;
    return true;
}

EdfInventoryStatus edf_inventory_describe_file(const char *path,
                                               const uint8_t *header,
                                               size_t header_size,
                                               size_t file_size,
                                               EdfInventoryEntry &out) {
    out = {};
    out.file_size = file_size;
    if (!edf_inventory_describe_path(path, out)) {
        out.status = EdfInventoryStatus::InvalidPath;
        return out.status;
    }
    out.file_size = file_size;
    if (metadata_kind(out.kind)) {
        out.data_size = file_size;
        out.status = EdfInventoryStatus::Ok;
        return out.status;
    }

    if (!edf_parse_header_summary(header, header_size, out.header)) {
        out.status = EdfInventoryStatus::InvalidHeader;
        return out.status;
    }
    if (file_size < out.header.header_size) {
        out.status = EdfInventoryStatus::FileTooSmall;
        return out.status;
    }

    out.data_size = file_size - out.header.header_size;
    if (out.header.record_size == 0) {
        out.status = EdfInventoryStatus::InvalidHeader;
        return out.status;
    }
    const size_t records_from_size = out.data_size / out.header.record_size;
    out.complete_records_from_size = records_from_size;
    out.partial_tail_bytes = out.data_size % out.header.record_size;
    if (out.partial_tail_bytes != 0) {
        out.warnings |= AC_EDF_INVENTORY_WARN_PARTIAL_TAIL;
    }
    if (out.header.record_count != records_from_size) {
        out.warnings |= AC_EDF_INVENTORY_WARN_RECORD_COUNT_MISMATCH;
        if (out.header.record_count < out.complete_records_from_size) {
            out.complete_records_from_size = out.header.record_count;
        }
    }

    out.status = EdfInventoryStatus::Ok;
    return out.status;
}

}  // namespace aircannect
