#include "plx_pairing_policy.h"

namespace aircannect {

PlxCentralAdmission plx_central_admission(bool saved_peer_present,
                                          bool saved_peer_matches,
                                          bool pairing_active,
                                          bool connection_bonded,
                                          bool role_enabled) {
    if (!role_enabled) return PlxCentralAdmission::Reject;
    if (saved_peer_matches) return PlxCentralAdmission::KnownPeer;
    if (pairing_active) return PlxCentralAdmission::PairingWindow;
    if (!saved_peer_present && connection_bonded) {
        return PlxCentralAdmission::AdoptExistingBond;
    }
    return PlxCentralAdmission::Reject;
}

}  // namespace aircannect
