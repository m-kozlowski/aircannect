#pragma once

#include <stddef.h>
#include <stdint.h>

#include "edf_report_catalog.h"
#include "report_records.h"
#include "report_sources.h"

namespace aircannect {

static constexpr size_t AC_EDF_REPORT_DATA_SIGNAL_LABEL_MAX =
    AC_EDF_SIGNAL_LABEL_TEXT_SIZE;

enum class EdfReportDataKind : uint8_t {
    Series,
    Events,
};

struct EdfReportDataPlanEntry {
    EdfReportDataKind kind = EdfReportDataKind::Series;
    ReportSignalId signal = ReportSignalId::Flow;
    ReportSourceId source = ReportSourceId::Summary;
    const char *name = nullptr;
    EdfInventoryFileKind file_kind = EdfInventoryFileKind::Unknown;
    uint8_t file_slot = 0;
    char signal_label[AC_EDF_REPORT_DATA_SIGNAL_LABEL_MAX] = {};
    uint32_t first_record = 0;
    uint32_t record_count = 0;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    uint32_t sample_interval_ms = 0;
    uint32_t record_count_estimate = 0;
    uint32_t payload_len_estimate = 0;
    bool primary = false;
};

struct EdfReportDataCoverage {
    uint32_t event_entries = 0;
    uint32_t scored_event_sources = 0;
    uint32_t signals_required = 0;
    uint32_t signals_covered = 0;
};

using EdfReportDataPlanCallback =
    bool (*)(void *context, const EdfReportDataPlanEntry &entry);

bool edf_report_session_has_file(const EdfReportSessionDescriptor &session,
                                 EdfInventoryFileKind kind);

bool edf_report_plan_events(const EdfReportSessionDescriptor &session,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context);

bool edf_report_plan_signal(const EdfReportSessionDescriptor &session,
                            ReportSignalId signal,
                            int64_t range_start_ms,
                            int64_t range_end_ms,
                            EdfReportDataPlanCallback callback,
                            void *context);

bool edf_report_plan_covers_report(
    const EdfReportSessionDescriptor *sessions,
    size_t session_count,
    int64_t range_start_ms,
    int64_t range_end_ms,
    EdfReportDataCoverage *coverage_out = nullptr);

}  // namespace aircannect
