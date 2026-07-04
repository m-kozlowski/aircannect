#include "spool_client_status.h"

namespace aircannect {

const char *spool_client_state_name(SpoolClientState state) {
    switch (state) {
        case SpoolClientState::Idle: return "idle";
        case SpoolClientState::Starting: return "starting";
        case SpoolClientState::Pulling: return "pulling";
        case SpoolClientState::WaitingFragments: return "fragments";
        case SpoolClientState::Complete: return "complete";
        case SpoolClientState::Error: return "error";
    }
    return "unknown";
}

}  // namespace aircannect
