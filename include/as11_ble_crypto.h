#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "rpc_payload.h"

namespace aircannect {

static constexpr size_t AS11_BLE_KEY_BYTES = 32;
static constexpr size_t AS11_BLE_KEY_HEX_BYTES = AS11_BLE_KEY_BYTES * 2;
static constexpr size_t AS11_BLE_ENCRYPTED_REQUEST_MAX_BYTES = 7650;

class As11BleSessionCrypto {
public:
    bool set_master_key_hex(const char *master_key_hex);
    bool integrity_response_hex(const char *challenge_hex,
                                char out[AS11_BLE_KEY_HEX_BYTES + 1]) const;
    bool derive_session_key(const char *nonce_hex);

    std::unique_ptr<LargeByteBuffer> encrypt(RpcPayloadView plaintext) const;
    RpcPayloadRef decrypt(const uint8_t *encrypted,
                          size_t encrypted_length,
                          const char *&error) const;

    bool has_master_key() const { return master_key_ready_; }
    bool ready() const { return session_key_ready_; }
    void clear_session();
    void clear();

private:
    uint8_t master_key_[AS11_BLE_KEY_BYTES] = {};
    uint8_t session_key_[AS11_BLE_KEY_BYTES] = {};
    bool master_key_ready_ = false;
    bool session_key_ready_ = false;
};

}  // namespace aircannect
