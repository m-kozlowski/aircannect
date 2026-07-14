#include "background_operation_control.h"

namespace aircannect {

const char *background_operation_stop_error(BackgroundOperationStop reason) {
    switch (reason) {
        case BackgroundOperationStop::None: return "";
        case BackgroundOperationStop::Aborted: return "preempted";
        case BackgroundOperationStop::Deadline: return "operation_timeout";
    }
    return "operation_stopped";
}

}  // namespace aircannect
