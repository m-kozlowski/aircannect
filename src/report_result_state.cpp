#include "report_manager.h"

#include <string.h>

#include "debug_log.h"
#include "report_night_index.h"
#include "report_result_json.h"

namespace aircannect {

ReportResultStatus ReportManager::result_status() const {
    return result_build_.status();
}

}  // namespace aircannect
