#pragma once

#include <stdint.h>
#include <string>

namespace aircannect {

enum class SpoolClientState : uint8_t {
    Idle,
    Starting,
    Pulling,
    WaitingFragments,
    Complete,
    Error,
};

struct SpoolClientStatus {
    SpoolClientState state = SpoolClientState::Idle;
    std::string spool_type;
    std::string error;
    uint16_t current_round = 0;
    uint32_t active_spool_id = 0;
    uint32_t fragments = 0;
    uint32_t bytes = 0;
    uint32_t round_fragments = 0;
    uint32_t round_bytes = 0;
    uint32_t elapsed_ms = 0;
    uint32_t idle_ms = 0;
};

const char *spool_client_state_name(SpoolClientState state);

}  // namespace aircannect
