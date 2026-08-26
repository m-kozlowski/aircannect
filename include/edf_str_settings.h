#pragma once

#include <stdint.h>
#include <string>

#include "edf_str_session.h"
#include "report_proto.h"
#include "report_spool_types.h"
#include "rpc_payload.h"

namespace aircannect {

struct EdfStrSettingsApplyResult {
    bool ok = false;
    const char *error = nullptr;
    uint32_t values = 0;
    uint32_t missing = 0;
    uint32_t unmapped = 0;
};

struct EdfStrSummaryApplyResult : EdfStrSettingsApplyResult {
    bool record_found = false;
};

std::string edf_str_setting_get_names();
bool edf_str_summary_field_for_tag(const char *tag,
                                   ReportSummaryField &out);

bool edf_str_apply_settings_response(RpcPayloadView payload,
                                     EdfStrSessionAccumulator &session,
                                     EdfStrSettingsApplyResult &result);
bool edf_str_apply_summary_record(const ReportSummaryRecord &record,
                                  EdfStrSessionAccumulator &session,
                                  EdfStrSettingsApplyResult &result);
bool edf_str_apply_summary_spool(const ReportSpoolResult &spool,
                                 uint16_t sleep_day,
                                 uint64_t session_start_ms,
                                 uint64_t session_end_ms,
                                 EdfStrSessionAccumulator &session,
                                 EdfStrSummaryApplyResult &result);

}  // namespace aircannect
