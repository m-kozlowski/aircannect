#pragma once

#include <stdint.h>

namespace aircannect {

enum class PlxCentralAdmission : uint8_t {
    Reject,
    KnownPeer,
    PairingWindow,
    AdoptExistingBond,
};

PlxCentralAdmission plx_central_admission(bool saved_peer_present,
                                          bool saved_peer_matches,
                                          bool pairing_active,
                                          bool connection_bonded);

}  // namespace aircannect
