#pragma once

#include "oximetry_hub.h"
#include "report_oximetry.h"

namespace aircannect::OximetryCsv {

// Producer calls do not access the filesystem.
void observe(const OximetrySample &sample, int64_t utc_ms);
void publish_catalog(std::shared_ptr<const ReportOximetryCatalog> catalog);
void pause(bool paused);
bool drained();

}  // namespace aircannect::OximetryCsv
