#pragma once

#include <string.h>

#include "edf_report_data_plan.h"
#include "report_data_provider.h"
#include "string_util.h"

namespace aircannect {

struct EdfReportProviderToken {
    uint8_t session_index = 0;
    uint8_t file_slot = 0;
    EdfInventoryFileKind file_kind = EdfInventoryFileKind::Unknown;
    EdfReportDataKind data_kind = EdfReportDataKind::Series;
    bool primary = false;
    bool trim_leading_padding = false;
    bool trim_trailing_padding = false;
    uint32_t first_record = 0;
    uint32_t record_count = 0;
    char signal_label[AC_EDF_REPORT_DATA_SIGNAL_LABEL_MAX] = {};
};

struct EdfReportProviderReaderGroupKey {
    uint8_t session_index = 0;
    uint8_t file_slot = 0;
    EdfInventoryFileKind file_kind = EdfInventoryFileKind::Unknown;
    EdfReportDataKind data_kind = EdfReportDataKind::Series;
    bool sample_reader = false;
};

static_assert(sizeof(EdfReportProviderToken) <= AC_REPORT_PROVIDER_TOKEN_BYTES,
              "EDF report provider token exceeds neutral token storage");

inline void edf_report_provider_pack_token(
    ReportProviderChunkRef &ref,
    const EdfReportProviderToken &token) {
    ref = {};
    ref.provider = ReportProviderId::Edf;
    memcpy(ref.data, &token, sizeof(token));
}

inline bool edf_report_provider_unpack_token(
    const ReportProviderChunkRef &ref,
    EdfReportProviderToken &token) {
    if (ref.provider != ReportProviderId::Edf) return false;
    memcpy(&token, ref.data, sizeof(token));
    return true;
}

inline bool edf_report_provider_reader_group_key(
    const ReportProviderChunkRef &ref,
    bool sample_reader,
    EdfReportProviderReaderGroupKey &key) {
    EdfReportProviderToken token;
    if (!edf_report_provider_unpack_token(ref, token)) return false;

    key.session_index = token.session_index;
    key.file_slot = token.file_slot;
    key.file_kind = token.file_kind;
    key.data_kind = token.data_kind;
    key.sample_reader = sample_reader;
    return true;
}

inline bool edf_report_provider_same_reader_group(
    const EdfReportProviderReaderGroupKey &a,
    const EdfReportProviderReaderGroupKey &b) {
    return a.session_index == b.session_index &&
           a.file_slot == b.file_slot &&
           a.file_kind == b.file_kind &&
           a.data_kind == b.data_kind &&
           a.sample_reader == b.sample_reader;
}

inline bool edf_report_provider_entry_from_chunk(
    const ReportProviderChunk &chunk,
    size_t session_count,
    EdfReportProviderToken &token,
    EdfReportDataPlanEntry &entry) {
    entry = {};
    if (!edf_report_provider_unpack_token(chunk.ref, token) ||
        token.session_index >= session_count ||
        !chunk.name || !chunk.name[0]) {
        return false;
    }

    entry.kind = token.data_kind;
    entry.signal = chunk.signal;
    entry.source = chunk.source;
    entry.name = chunk.name;
    entry.file_kind = token.file_kind;
    entry.file_slot = token.file_slot;
    copy_cstr(entry.signal_label,
              sizeof(entry.signal_label),
              token.signal_label);
    entry.first_record = token.first_record;
    entry.record_count = token.record_count;
    entry.start_ms = chunk.start_ms;
    entry.end_ms = chunk.end_ms;
    entry.record_count_estimate = chunk.record_count;
    entry.payload_len_estimate = chunk.payload_len;
    entry.primary = token.primary;
    entry.trim_leading_padding = token.trim_leading_padding;
    entry.trim_trailing_padding = token.trim_trailing_padding;
    return true;
}

}  // namespace aircannect
