#include "as11_ble_crypto.h"

#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "hex_util.h"

namespace aircannect {

namespace {

bool decode_hex_bytes(const char *text,
                      uint8_t *out,
                      size_t out_size,
                      size_t &decoded_length) {
    if (!text) return false;
    return hex_decode(text, strlen(text), out, out_size, decoded_length);
}

bool sha256_parts(const uint8_t *first,
                  size_t first_length,
                  const uint8_t *second,
                  size_t second_length,
                  uint8_t out[AS11_BLE_KEY_BYTES]) {
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    int result = mbedtls_sha256_starts(&context, 0);
    if (result == 0 && first_length != 0) {
        result = mbedtls_sha256_update(&context, first, first_length);
    }
    if (result == 0 && second_length != 0) {
        result = mbedtls_sha256_update(&context, second, second_length);
    }
    if (result == 0) result = mbedtls_sha256_finish(&context, out);
    mbedtls_sha256_free(&context);
    return result == 0;
}

bool all_zero(const uint8_t *data, size_t length) {
    uint8_t combined = 0;
    for (size_t i = 0; i < length; ++i) combined |= data[i];
    return combined == 0;
}

}  // namespace

bool As11BleSessionCrypto::set_master_key_hex(const char *master_key_hex) {
    clear();

    size_t decoded = 0;
    if (!master_key_hex || strlen(master_key_hex) != AS11_BLE_KEY_HEX_BYTES ||
        !decode_hex_bytes(master_key_hex, master_key_, sizeof(master_key_),
                          decoded) ||
        decoded != sizeof(master_key_) || all_zero(master_key_, decoded)) {
        clear();
        return false;
    }
    master_key_ready_ = true;
    return true;
}

bool As11BleSessionCrypto::integrity_response_hex(
    const char *challenge_hex,
    char out[AS11_BLE_KEY_HEX_BYTES + 1]) const {
    if (!master_key_ready_ || !out) return false;

    uint8_t challenge[AS11_BLE_KEY_BYTES] = {};
    size_t challenge_length = 0;
    if (!decode_hex_bytes(challenge_hex, challenge, sizeof(challenge),
                          challenge_length) ||
        challenge_length == 0) {
        return false;
    }

    const mbedtls_md_info_t *sha256 =
        mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t response[AS11_BLE_KEY_BYTES] = {};
    if (!sha256 ||
        mbedtls_md_hmac(sha256, master_key_, sizeof(master_key_), challenge,
                        challenge_length, response) != 0) {
        return false;
    }
    return hex_encode(response, sizeof(response), out,
                      AS11_BLE_KEY_HEX_BYTES + 1, HexCase::Upper);
}

bool As11BleSessionCrypto::derive_session_key(const char *nonce_hex) {
    clear_session();
    if (!master_key_ready_) return false;

    uint8_t nonce[AS11_BLE_KEY_BYTES] = {};
    size_t nonce_length = 0;
    if (!decode_hex_bytes(nonce_hex, nonce, sizeof(nonce), nonce_length) ||
        nonce_length == 0 ||
        !sha256_parts(master_key_, sizeof(master_key_), nonce, nonce_length,
                      session_key_)) {
        clear_session();
        return false;
    }
    session_key_ready_ = true;
    return true;
}

std::unique_ptr<LargeByteBuffer> As11BleSessionCrypto::encrypt(
    RpcPayloadView plaintext) const {
    if (!session_key_ready_ || plaintext.size() > UINT16_MAX) return {};

    const size_t framed_length = 2 + plaintext.size();
    const size_t padded_length = (framed_length + 15u) & ~size_t(15u);
    const size_t encrypted_length = 16 + padded_length;
    if (encrypted_length > AS11_BLE_ENCRYPTED_REQUEST_MAX_BYTES) return {};

    std::unique_ptr<LargeByteBuffer> out =
        LargeByteBuffer::allocate(encrypted_length);
    if (!out) return {};

    uint8_t *iv = out->data();
    uint8_t *ciphertext = out->data() + 16;
    esp_fill_random(iv, 16);

    memset(ciphertext, 0, padded_length);
    ciphertext[0] = static_cast<uint8_t>(plaintext.size());
    ciphertext[1] = static_cast<uint8_t>(plaintext.size() >> 8);
    if (!plaintext.empty()) {
        memcpy(ciphertext + 2, plaintext.data(), plaintext.size());
    }

    uint8_t working_iv[16];
    memcpy(working_iv, iv, sizeof(working_iv));

    mbedtls_aes_context context;
    mbedtls_aes_init(&context);
    int result = mbedtls_aes_setkey_enc(&context, session_key_, 256);
    if (result == 0) {
        result = mbedtls_aes_crypt_cbc(
            &context, MBEDTLS_AES_ENCRYPT, padded_length, working_iv,
            ciphertext, ciphertext);
    }
    mbedtls_aes_free(&context);
    return result == 0 ? std::move(out) : nullptr;
}

RpcPayloadRef As11BleSessionCrypto::decrypt(const uint8_t *encrypted,
                                            size_t encrypted_length,
                                            const char *&error) const {
    error = "";
    if (!session_key_ready_) {
        error = "session_key_missing";
        return {};
    }
    if (!encrypted || encrypted_length < 32 ||
        ((encrypted_length - 16) & 15u) != 0) {
        error = "encrypted_length_invalid";
        return {};
    }

    const size_t plaintext_capacity = encrypted_length - 16;
    std::unique_ptr<LargeByteBuffer> plaintext =
        LargeByteBuffer::allocate(plaintext_capacity);
    if (!plaintext) {
        error = "decrypt_allocation_failed";
        return {};
    }

    uint8_t working_iv[16];
    memcpy(working_iv, encrypted, sizeof(working_iv));

    mbedtls_aes_context context;
    mbedtls_aes_init(&context);
    int result = mbedtls_aes_setkey_dec(&context, session_key_, 256);
    if (result == 0) {
        result = mbedtls_aes_crypt_cbc(
            &context, MBEDTLS_AES_DECRYPT, plaintext_capacity, working_iv,
            encrypted + 16, plaintext->data());
    }
    mbedtls_aes_free(&context);
    if (result != 0) {
        error = "decrypt_failed";
        return {};
    }

    const size_t payload_length =
        static_cast<size_t>(plaintext->data()[0]) |
        static_cast<size_t>(plaintext->data()[1]) << 8;
    if (payload_length > plaintext_capacity - 2) {
        error = "decrypted_length_invalid";
        return {};
    }
    for (size_t i = 2 + payload_length; i < plaintext_capacity; ++i) {
        if (plaintext->data()[i] != 0) {
            error = "decrypted_padding_invalid";
            return {};
        }
    }
    if (payload_length == 0) {
        error = "decrypted_payload_empty";
        return {};
    }

    RpcPayloadRef payload = LargeByteBuffer::copy_and_freeze(
        plaintext->data() + 2, payload_length);
    if (!payload) error = "decrypt_payload_allocation_failed";
    return payload;
}

void As11BleSessionCrypto::clear_session() {
    memset(session_key_, 0, sizeof(session_key_));
    session_key_ready_ = false;
}

void As11BleSessionCrypto::clear() {
    clear_session();
    memset(master_key_, 0, sizeof(master_key_));
    master_key_ready_ = false;
}

}  // namespace aircannect
