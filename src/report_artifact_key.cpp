#include "report_artifact_key.h"

namespace aircannect {

ReportArtifactKey ReportArtifactKey::result(
    SleepDayId sleep_day, SourceRevision source_revision) {
    ReportArtifactKey key;
    key.sleep_day = sleep_day;
    key.source_revision = source_revision;
    return key;
}

}  // namespace aircannect
