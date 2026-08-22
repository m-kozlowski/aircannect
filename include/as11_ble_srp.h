#pragma once

#include <stddef.h>
#include <stdint.h>

namespace aircannect {

static constexpr size_t AS11_BLE_SRP_PAD_BYTES = 256;
static constexpr size_t AS11_BLE_SRP_PUBLIC_HEX_LENGTH =
    AS11_BLE_SRP_PAD_BYTES * 2;
static constexpr size_t AS11_BLE_SRP_PROOF_BYTES = 32;
static constexpr size_t AS11_BLE_SRP_PROOF_HEX_LENGTH =
    AS11_BLE_SRP_PROOF_BYTES * 2;

class As11BleSrpClient {
public:
    ~As11BleSrpClient();

    bool begin(
        char public_key_hex[AS11_BLE_SRP_PUBLIC_HEX_LENGTH + 1]);
    bool finish(const char *passkey,
                const char *server_public_key_hex,
                const char *salt_hex,
                char client_proof_hex[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1],
                char master_key_hex[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1]);
    bool verify_server(const char *server_proof_hex) const;
    void clear();

private:
    uint8_t private_key_[32] = {};
    uint8_t public_key_[AS11_BLE_SRP_PAD_BYTES] = {};
    uint8_t expected_server_proof_[AS11_BLE_SRP_PROOF_BYTES] = {};
    bool started_ = false;
    bool proof_ready_ = false;
};

bool as11_ble_pairing_passkey_valid(const char *passkey);

}  // namespace aircannect
