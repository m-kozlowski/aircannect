#include "report_artifact_key.h"

#include <limits.h>

#include "report_range_tile.h"

namespace aircannect {

namespace {

ReportArtifactKey base_key(SleepDayId sleep_day,
                           SourceRevision source_revision,
                           ReportArtifactKind kind) {
    ReportArtifactKey key;
    key.sleep_day = sleep_day;
    key.source_revision = source_revision;
    key.kind = kind;
    return key;
}

}  // namespace

ReportArtifactKey ReportArtifactKey::result(
    SleepDayId sleep_day, SourceRevision source_revision) {
    return base_key(sleep_day, source_revision, ReportArtifactKind::Result);
}

ReportArtifactKey ReportArtifactKey::overview(
    SleepDayId sleep_day, SourceRevision source_revision) {
    return base_key(sleep_day, source_revision, ReportArtifactKind::Overview);
}

ReportArtifactKey ReportArtifactKey::range_tile(
    SleepDayId sleep_day,
    SourceRevision source_revision,
    int64_t range_start_ms,
    int64_t range_end_ms) {
    ReportArtifactKey key =
        base_key(sleep_day, source_revision, ReportArtifactKind::RangeTile);
    key.range_start_ms = range_start_ms;
    key.range_end_ms = range_end_ms;
    return key;
}

bool ReportArtifactKey::valid() const {
    if (!sleep_day.valid() || !source_revision.valid()) return false;

    switch (kind) {
        case ReportArtifactKind::Result:
        case ReportArtifactKind::Overview:
            return range_start_ms == 0 && range_end_ms == 0;
        case ReportArtifactKind::RangeTile:
            return range_start_ms > 0 &&
                   range_start_ms % REPORT_RANGE_TILE_MS == 0 &&
                   range_start_ms <= INT64_MAX - REPORT_RANGE_TILE_MS &&
                   range_end_ms == range_start_ms + REPORT_RANGE_TILE_MS;
    }
    return false;
}

}  // namespace aircannect
