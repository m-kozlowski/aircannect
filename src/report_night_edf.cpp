#include "report_night_index.h"

#include <algorithm>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace aircannect {
namespace {

constexpr int64_t REPORT_SESSION_MERGE_TOLERANCE_MS = 2 * 60 * 1000;

bool session_windows_match_with_tolerance(int64_t first_start_ms,
                                          int64_t first_end_ms,
                                          int64_t second_start_ms,
                                          int64_t second_end_ms,
                                          int64_t tolerance_ms) {
    if (first_start_ms <= 0 || second_start_ms <= 0) return false;
    if (first_end_ms <= first_start_ms) first_end_ms = first_start_ms;
    if (second_end_ms <= second_start_ms) second_end_ms = second_start_ms;

    if (first_start_ms <= second_end_ms + tolerance_ms &&
        second_start_ms <= first_end_ms + tolerance_ms) {
        return true;
    }

    return llabs(first_start_ms - second_start_ms) <= tolerance_ms;
}

bool edf_session_has_report_event_mask(
    const EdfReportSessionDescriptor &session) {
    const uint32_t event_mask =
        edf_report_file_kind_mask(EdfInventoryFileKind::Eve) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Csl);
    return (session.file_mask & event_mask) != 0;
}

void copy_annotation_file_to_numeric(EdfReportSessionDescriptor &numeric,
                                     const EdfReportSessionDescriptor &annotation,
                                     EdfInventoryFileKind kind) {
    const uint32_t mask = edf_report_file_kind_mask(kind);
    if (mask == 0 ||
        (annotation.file_mask & mask) == 0 ||
        (numeric.file_mask & mask) != 0) {
        return;
    }

    const size_t slot = edf_report_session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return;

    const EdfReportSessionFileDescriptor &src = annotation.files[slot];
    if (src.kind != kind || !src.path[0]) return;

    numeric.files[slot] = src;
    numeric.file_mask |= mask;
    numeric.file_count++;

    if (src.file_size <= UINT64_MAX - numeric.total_size) {
        numeric.total_size += src.file_size;
    }
    if (src.last_write > numeric.latest_write) {
        numeric.latest_write = src.last_write;
    }
    if (src.header_start_ms > 0 &&
        (numeric.earliest_header_start_ms == 0 ||
         src.header_start_ms < numeric.earliest_header_start_ms)) {
        numeric.earliest_header_start_ms = src.header_start_ms;
    }
    if (src.header_end_ms > numeric.latest_header_end_ms) {
        numeric.latest_header_end_ms = src.header_end_ms;
    }

    numeric.warnings |= annotation.warnings;
}

bool merge_annotation_session_into_numeric(
    EdfReportSessionDescriptor &numeric,
    const EdfReportSessionDescriptor &annotation) {
    if (!edf_session_annotation_matches_numeric(numeric, annotation)) {
        return false;
    }

    copy_annotation_file_to_numeric(numeric,
                                    annotation,
                                    EdfInventoryFileKind::Eve);
    copy_annotation_file_to_numeric(numeric,
                                    annotation,
                                    EdfInventoryFileKind::Csl);
    return true;
}

}  // namespace

bool edf_session_has_report_numeric(
    const EdfReportSessionDescriptor &session) {
    const uint32_t numeric_mask =
        edf_report_file_kind_mask(EdfInventoryFileKind::Brp) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Pld) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Sa2);
    return (session.file_mask & numeric_mask) != 0 &&
           session.earliest_header_start_ms > 0 &&
           session.latest_header_end_ms > session.earliest_header_start_ms;
}

bool edf_session_has_report_annotation(
    const EdfReportSessionDescriptor &session) {
    return edf_session_has_report_event_mask(session);
}

bool edf_session_annotation_matches_numeric(
    const EdfReportSessionDescriptor &numeric_session,
    const EdfReportSessionDescriptor &annotation_session) {
    if (!edf_session_has_report_numeric(numeric_session) ||
        !edf_session_has_report_annotation(annotation_session)) {
        return false;
    }

    if (strcmp(numeric_session.sleep_day, annotation_session.sleep_day) != 0) {
        return false;
    }

    if (numeric_session.session_stamp[0] &&
        strcmp(numeric_session.session_stamp,
               annotation_session.session_stamp) == 0) {
        return true;
    }

    return session_windows_match_with_tolerance(
        numeric_session.earliest_header_start_ms,
        numeric_session.latest_header_end_ms,
        annotation_session.earliest_header_start_ms,
        annotation_session.latest_header_end_ms,
        REPORT_SESSION_MERGE_TOLERANCE_MS);
}

void merge_edf_annotation_sessions(EdfReportSessionDescriptor *sessions,
                                   size_t &session_count) {
    if (!sessions || session_count == 0) {
        session_count = 0;
        return;
    }

    const size_t count = std::min(
        session_count,
        static_cast<size_t>(AC_REPORT_EDF_SESSION_MAX));
    for (size_t i = 0; i < count; ++i) {
        if (!edf_session_has_report_annotation(sessions[i]) ||
            edf_session_has_report_numeric(sessions[i])) {
            continue;
        }

        int best = -1;
        int64_t best_distance = INT64_MAX;
        for (size_t j = 0; j < count; ++j) {
            if (i == j || !edf_session_has_report_numeric(sessions[j])) {
                continue;
            }
            if (!edf_session_annotation_matches_numeric(sessions[j],
                                                        sessions[i])) {
                continue;
            }

            const int64_t distance =
                llabs(sessions[j].earliest_header_start_ms -
                      sessions[i].earliest_header_start_ms);
            if (distance < best_distance) {
                best = static_cast<int>(j);
                best_distance = distance;
            }
        }

        if (best >= 0 &&
            merge_annotation_session_into_numeric(
                sessions[static_cast<size_t>(best)],
                sessions[i])) {
            edf_report_session_init(sessions[i]);
        }
    }

    size_t write = 0;
    for (size_t i = 0; i < session_count; ++i) {
        if (sessions[i].file_count == 0 || sessions[i].file_mask == 0) {
            continue;
        }
        if (write != i) sessions[write] = sessions[i];
        write++;
    }

    for (size_t i = write; i < session_count; ++i) {
        edf_report_session_init(sessions[i]);
    }
    session_count = write;
}

}  // namespace aircannect
