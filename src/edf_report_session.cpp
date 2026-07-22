#include "edf_report_session.h"

#include <algorithm>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

namespace aircannect {
namespace {

constexpr int64_t ANNOTATION_MATCH_TOLERANCE_MS = 2 * 60 * 1000;

bool valid_file_bounds(const EdfReportSessionFileDescriptor &file,
                       bool local) {
    const int64_t start_ms = local ? file.local_header_start_ms
                                   : file.header_start_ms;
    const int64_t end_ms = local ? file.local_header_end_ms
                                 : file.header_end_ms;

    return file.kind != EdfInventoryFileKind::Unknown && file.path[0] &&
           start_ms > 0 && end_ms >= start_ms;
}

bool add_file_bounds(const EdfReportSessionDescriptor &session,
                     EdfInventoryFileKind kind,
                     bool local,
                     int64_t &start_ms,
                     int64_t &end_ms) {
    const size_t slot = edf_report_session_file_slot(kind);
    if (slot >= AC_EDF_REPORT_SESSION_FILE_MAX) return false;

    const EdfReportSessionFileDescriptor &file = session.files[slot];
    if (file.kind != kind || !valid_file_bounds(file, local)) return false;

    const int64_t file_start_ms = local ? file.local_header_start_ms
                                        : file.header_start_ms;
    const int64_t file_end_ms = local ? file.local_header_end_ms
                                      : file.header_end_ms;

    if (start_ms == 0 || file_start_ms < start_ms) {
        start_ms = file_start_ms;
    }
    end_ms = std::max(end_ms, std::max(file_end_ms, file_start_ms));
    return true;
}

void refresh_bounds(EdfReportSessionDescriptor &session,
                    bool local,
                    int64_t &start_ms,
                    int64_t &end_ms) {
    start_ms = 0;
    end_ms = 0;

    if (!add_file_bounds(session,
                         EdfInventoryFileKind::Brp,
                         local,
                         start_ms,
                         end_ms)) {
        (void)add_file_bounds(session,
                              EdfInventoryFileKind::Pld,
                              local,
                              start_ms,
                              end_ms);
        (void)add_file_bounds(session,
                              EdfInventoryFileKind::Sa2,
                              local,
                              start_ms,
                              end_ms);
    }

    if (start_ms == 0) {
        (void)add_file_bounds(session,
                              EdfInventoryFileKind::Eve,
                              local,
                              start_ms,
                              end_ms);
        (void)add_file_bounds(session,
                              EdfInventoryFileKind::Csl,
                              local,
                              start_ms,
                              end_ms);
    }
}

int64_t session_match_start_ms(
    const EdfReportSessionDescriptor &session) {
    return session.local_earliest_header_start_ms > 0
        ? session.local_earliest_header_start_ms
        : session.earliest_header_start_ms;
}

int64_t session_match_end_ms(const EdfReportSessionDescriptor &session) {
    return session.local_latest_header_end_ms > 0
        ? session.local_latest_header_end_ms
        : session.latest_header_end_ms;
}

bool session_windows_match_with_tolerance(int64_t first_start_ms,
                                          int64_t first_end_ms,
                                          int64_t second_start_ms,
                                          int64_t second_end_ms) {
    if (first_start_ms <= 0 || second_start_ms <= 0) return false;
    if (first_end_ms <= first_start_ms) first_end_ms = first_start_ms;
    if (second_end_ms <= second_start_ms) second_end_ms = second_start_ms;

    if (first_start_ms <= second_end_ms + ANNOTATION_MATCH_TOLERANCE_MS &&
        second_start_ms <= first_end_ms + ANNOTATION_MATCH_TOLERANCE_MS) {
        return true;
    }

    return llabs(first_start_ms - second_start_ms) <=
           ANNOTATION_MATCH_TOLERANCE_MS;
}

void copy_annotation_file(EdfReportSessionDescriptor &numeric,
                          const EdfReportSessionDescriptor &annotation,
                          EdfInventoryFileKind kind) {
    const uint32_t mask = edf_report_file_kind_mask(kind);
    if (mask == 0 || (annotation.file_mask & mask) == 0 ||
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
    numeric.latest_write = std::max(numeric.latest_write, src.last_write);
    numeric.warnings |= annotation.warnings;

    edf_report_session_refresh_bounds(numeric);
}

bool merge_annotation_session(EdfReportSessionDescriptor &numeric,
                              const EdfReportSessionDescriptor &annotation) {
    if (!edf_session_annotation_matches_numeric(numeric, annotation)) {
        return false;
    }

    copy_annotation_file(numeric, annotation, EdfInventoryFileKind::Eve);
    copy_annotation_file(numeric, annotation, EdfInventoryFileKind::Csl);
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
    const uint32_t annotation_mask =
        edf_report_file_kind_mask(EdfInventoryFileKind::Eve) |
        edf_report_file_kind_mask(EdfInventoryFileKind::Csl);
    return (session.file_mask & annotation_mask) != 0;
}

bool edf_report_session_reportable(
    const EdfReportSessionDescriptor &session) {
    return edf_session_has_report_numeric(session) &&
           edf_session_has_report_annotation(session);
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
        session_match_start_ms(numeric_session),
        session_match_end_ms(numeric_session),
        session_match_start_ms(annotation_session),
        session_match_end_ms(annotation_session));
}

void edf_report_session_refresh_bounds(EdfReportSessionDescriptor &session) {
    // BRP carries the two required high-resolution report signals. Its
    // physical window remains canonical in both wall-clock and UTC timelines.
    refresh_bounds(session,
                   true,
                   session.local_earliest_header_start_ms,
                   session.local_latest_header_end_ms);
    refresh_bounds(session,
                   false,
                   session.earliest_header_start_ms,
                   session.latest_header_end_ms);
}

void normalize_edf_report_sessions(EdfReportSessionDescriptor *sessions,
                                   size_t &session_count) {
    if (!sessions || session_count == 0) {
        session_count = 0;
        return;
    }

    for (size_t i = 0; i < session_count; ++i) {
        edf_report_session_refresh_bounds(sessions[i]);
    }

    for (size_t i = 0; i < session_count; ++i) {
        if (!edf_session_has_report_annotation(sessions[i]) ||
            edf_session_has_report_numeric(sessions[i])) {
            continue;
        }

        size_t best = session_count;
        int64_t best_distance = INT64_MAX;
        for (size_t j = 0; j < session_count; ++j) {
            if (i == j || !edf_session_has_report_numeric(sessions[j]) ||
                !edf_session_annotation_matches_numeric(sessions[j],
                                                        sessions[i])) {
                continue;
            }

            const int64_t distance =
                llabs(session_match_start_ms(sessions[j]) -
                      session_match_start_ms(sessions[i]));
            if (distance < best_distance) {
                best = j;
                best_distance = distance;
            }
        }

        if (best < session_count &&
            merge_annotation_session(sessions[best], sessions[i])) {
            edf_report_session_init(sessions[i]);
        }
    }

    size_t write = 0;
    for (size_t i = 0; i < session_count; ++i) {
        if (!edf_report_session_reportable(sessions[i])) continue;
        if (write != i) sessions[write] = sessions[i];
        write++;
    }

    for (size_t i = write; i < session_count; ++i) {
        edf_report_session_init(sessions[i]);
    }
    session_count = write;
}

}  // namespace aircannect
