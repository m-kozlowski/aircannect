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
    uint32_t first_record = 0;
    uint32_t record_count = 0;
    char signal_label[AC_EDF_REPORT_DATA_SIGNAL_LABEL_MAX] = {};
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

}  // namespace aircannect
