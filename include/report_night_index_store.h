#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_night_index_snapshot.h"

namespace aircannect {
namespace ReportNightIndexStore {

bool load(ReportNightIndexSnapshotRef &out, uint32_t &content_crc);
bool save(const ReportNightIndexSnapshot &snapshot,
          uint32_t &content_crc);

}  // namespace ReportNightIndexStore
}  // namespace aircannect
