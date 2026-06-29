#pragma once

#include <stddef.h>
#include <stdint.h>

#include "report_night_index.h"

namespace aircannect {
namespace ReportNightIndexStore {

bool load(ReportIndexedNight *out,
          size_t capacity,
          size_t &count,
          uint32_t &content_crc);
bool content_crc(const ReportIndexedNight *nights,
                 size_t count,
                 uint32_t &content_crc);
bool save(const ReportIndexedNight *nights,
          size_t count,
          uint32_t &content_crc);

}  // namespace ReportNightIndexStore
}  // namespace aircannect
