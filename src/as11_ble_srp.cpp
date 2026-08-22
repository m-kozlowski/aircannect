#include "as11_ble_srp.h"

#include <esp_random.h>
#include <mbedtls/bignum.h>
#include <mbedtls/sha256.h>
#include <string.h>

#include "hex_util.h"

namespace aircannect {
namespace {

static constexpr const char *SRP_MODULUS_HEX =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050"
    "A37329CBB4A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50"
    "E8083969EDB767B0CF6095179A163AB3661A05FBD5FAAAE82918A9962F0B93B"
    "855F97993EC975EEAA80D740ADBF4FF747359D041D5C33EA71D281E446B1477"
    "3BCA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87"
    "748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB3786160279004E5"
    "7AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9D"
    "BFBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E"
    "4AFF73";

class Sha256 {
public:
    Sha256() {
        mbedtls_sha256_init(&context_);
        valid_ = mbedtls_sha256_starts(&context_, 0) == 0;
    }

    ~Sha256() { mbedtls_sha256_free(&context_); }

    bool add(const void *data, size_t length) {
        if (!valid_ || (!data && length != 0)) return false;
        if (length == 0) return true;

        valid_ = mbedtls_sha256_update(
                     &context_, static_cast<const uint8_t *>(data), length) ==
                 0;
        return valid_;
    }

    bool finish(uint8_t out[AS11_BLE_SRP_PROOF_BYTES]) {
        if (!valid_ || !out) return false;
        valid_ = mbedtls_sha256_finish(&context_, out) == 0;
        return valid_;
    }

private:
    mbedtls_sha256_context context_;
    bool valid_ = false;
};

class Mpi {
public:
    Mpi() { mbedtls_mpi_init(&value); }
    ~Mpi() { mbedtls_mpi_free(&value); }

    mbedtls_mpi value;
};

bool mpi_pad(const mbedtls_mpi &value,
             uint8_t out[AS11_BLE_SRP_PAD_BYTES]) {
    if (mbedtls_mpi_size(&value) > AS11_BLE_SRP_PAD_BYTES) return false;
    return mbedtls_mpi_write_binary(&value, out,
                                    AS11_BLE_SRP_PAD_BYTES) == 0;
}

bool sha256_one(const void *data,
                size_t length,
                uint8_t out[AS11_BLE_SRP_PROOF_BYTES]) {
    Sha256 hash;
    return hash.add(data, length) && hash.finish(out);
}

bool decode_hex(const char *text,
                uint8_t *out,
                size_t capacity,
                size_t &length) {
    if (!text) return false;
    return hex_decode(text, strlen(text), out, capacity, length);
}

bool read_padded_mpi(const uint8_t bytes[AS11_BLE_SRP_PAD_BYTES],
                     mbedtls_mpi &value) {
    return mbedtls_mpi_read_binary(&value, bytes,
                                   AS11_BLE_SRP_PAD_BYTES) == 0;
}

}  // namespace

As11BleSrpClient::~As11BleSrpClient() { clear(); }

bool As11BleSrpClient::begin(
    char public_key_hex[AS11_BLE_SRP_PUBLIC_HEX_LENGTH + 1]) {
    clear();
    if (!public_key_hex) return false;

    esp_fill_random(private_key_, sizeof(private_key_));

    Mpi modulus;
    Mpi generator;
    Mpi private_key;
    Mpi public_key;
    if (mbedtls_mpi_read_string(&modulus.value, 16, SRP_MODULUS_HEX) != 0 ||
        mbedtls_mpi_lset(&generator.value, 2) != 0 ||
        mbedtls_mpi_read_binary(&private_key.value, private_key_,
                                sizeof(private_key_)) != 0 ||
        mbedtls_mpi_exp_mod(&public_key.value, &generator.value,
                            &private_key.value, &modulus.value, nullptr) != 0 ||
        !mpi_pad(public_key.value, public_key_) ||
        !hex_encode(public_key_, sizeof(public_key_), public_key_hex,
                    AS11_BLE_SRP_PUBLIC_HEX_LENGTH + 1, HexCase::Upper)) {
        clear();
        return false;
    }

    started_ = true;
    return true;
}

bool As11BleSrpClient::finish(
    const char *passkey,
    const char *server_public_key_hex,
    const char *salt_hex,
    char client_proof_hex[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1],
    char master_key_hex[AS11_BLE_SRP_PROOF_HEX_LENGTH + 1]) {
    proof_ready_ = false;
    memset(expected_server_proof_, 0, sizeof(expected_server_proof_));
    if (!started_ || !as11_ble_pairing_passkey_valid(passkey) ||
        !server_public_key_hex || !salt_hex || !client_proof_hex ||
        !master_key_hex) {
        return false;
    }

    uint8_t salt[64] = {};
    size_t salt_length = 0;
    if (!decode_hex(salt_hex, salt, sizeof(salt), salt_length) ||
        salt_length == 0) {
        return false;
    }

    Mpi modulus;
    Mpi generator;
    Mpi private_key;
    Mpi public_key;
    Mpi server_public_key;
    Mpi remainder;
    if (mbedtls_mpi_read_string(&modulus.value, 16, SRP_MODULUS_HEX) != 0 ||
        mbedtls_mpi_lset(&generator.value, 2) != 0 ||
        mbedtls_mpi_read_binary(&private_key.value, private_key_,
                                sizeof(private_key_)) != 0 ||
        !read_padded_mpi(public_key_, public_key.value) ||
        mbedtls_mpi_read_string(&server_public_key.value, 16,
                                server_public_key_hex) != 0 ||
        mbedtls_mpi_mod_mpi(&remainder.value, &server_public_key.value,
                            &modulus.value) != 0 ||
        mbedtls_mpi_cmp_int(&remainder.value, 0) == 0) {
        return false;
    }

    uint8_t padded_modulus[AS11_BLE_SRP_PAD_BYTES] = {};
    uint8_t padded_generator[AS11_BLE_SRP_PAD_BYTES] = {};
    uint8_t padded_server_key[AS11_BLE_SRP_PAD_BYTES] = {};
    if (!mpi_pad(modulus.value, padded_modulus) ||
        !mpi_pad(generator.value, padded_generator) ||
        !mpi_pad(server_public_key.value, padded_server_key)) {
        return false;
    }

    uint8_t multiplier_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    Sha256 multiplier_digest;
    if (!multiplier_digest.add(padded_modulus, sizeof(padded_modulus)) ||
        !multiplier_digest.add(padded_generator,
                               sizeof(padded_generator)) ||
        !multiplier_digest.finish(multiplier_hash)) {
        return false;
    }

    uint8_t passkey_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    if (!sha256_one(passkey, strlen(passkey), passkey_hash)) return false;

    uint8_t private_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    Sha256 private_value;
    if (!private_value.add(salt, salt_length) ||
        !private_value.add(passkey_hash, sizeof(passkey_hash)) ||
        !private_value.finish(private_hash)) {
        return false;
    }

    uint8_t scrambling_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    Sha256 scrambling_digest;
    if (!scrambling_digest.add(public_key_, sizeof(public_key_)) ||
        !scrambling_digest.add(padded_server_key,
                               sizeof(padded_server_key)) ||
        !scrambling_digest.finish(scrambling_hash)) {
        return false;
    }

    Mpi multiplier;
    Mpi private_value_mpi;
    Mpi scrambling;
    if (mbedtls_mpi_read_binary(&multiplier.value, multiplier_hash,
                                sizeof(multiplier_hash)) != 0 ||
        mbedtls_mpi_read_binary(&private_value_mpi.value, private_hash,
                                sizeof(private_hash)) != 0 ||
        mbedtls_mpi_read_binary(&scrambling.value, scrambling_hash,
                                sizeof(scrambling_hash)) != 0 ||
        mbedtls_mpi_cmp_int(&scrambling.value, 0) == 0) {
        return false;
    }

    Mpi generator_private;
    Mpi multiplied_generator;
    Mpi base;
    Mpi exponent_product;
    Mpi exponent;
    Mpi shared_secret;
    if (mbedtls_mpi_exp_mod(&generator_private.value, &generator.value,
                            &private_value_mpi.value, &modulus.value,
                            nullptr) != 0 ||
        mbedtls_mpi_mul_mpi(&multiplied_generator.value, &multiplier.value,
                            &generator_private.value) != 0 ||
        mbedtls_mpi_sub_mpi(&base.value, &server_public_key.value,
                            &multiplied_generator.value) != 0 ||
        mbedtls_mpi_mod_mpi(&base.value, &base.value, &modulus.value) != 0 ||
        mbedtls_mpi_mul_mpi(&exponent_product.value, &scrambling.value,
                            &private_value_mpi.value) != 0 ||
        mbedtls_mpi_add_mpi(&exponent.value, &private_key.value,
                            &exponent_product.value) != 0 ||
        mbedtls_mpi_exp_mod(&shared_secret.value, &base.value,
                            &exponent.value, &modulus.value, nullptr) != 0) {
        return false;
    }

    uint8_t padded_secret[AS11_BLE_SRP_PAD_BYTES] = {};
    uint8_t master_key[AS11_BLE_SRP_PROOF_BYTES] = {};
    if (!mpi_pad(shared_secret.value, padded_secret) ||
        !sha256_one(padded_secret, sizeof(padded_secret), master_key)) {
        return false;
    }

    uint8_t modulus_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    uint8_t generator_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    uint8_t xor_hash[AS11_BLE_SRP_PROOF_BYTES] = {};
    if (!sha256_one(padded_modulus, sizeof(padded_modulus), modulus_hash) ||
        !sha256_one(padded_generator, sizeof(padded_generator),
                    generator_hash)) {
        return false;
    }
    for (size_t i = 0; i < sizeof(xor_hash); ++i) {
        xor_hash[i] = modulus_hash[i] ^ generator_hash[i];
    }

    uint8_t client_proof[AS11_BLE_SRP_PROOF_BYTES] = {};
    Sha256 proof;
    if (!proof.add(xor_hash, sizeof(xor_hash)) ||
        !proof.add(salt, salt_length) ||
        !proof.add(public_key_, sizeof(public_key_)) ||
        !proof.add(padded_server_key, sizeof(padded_server_key)) ||
        !proof.add(master_key, sizeof(master_key)) ||
        !proof.finish(client_proof)) {
        return false;
    }

    Sha256 server_proof;
    if (!server_proof.add(public_key_, sizeof(public_key_)) ||
        !server_proof.add(client_proof, sizeof(client_proof)) ||
        !server_proof.add(master_key, sizeof(master_key)) ||
        !server_proof.finish(expected_server_proof_) ||
        !hex_encode(client_proof, sizeof(client_proof), client_proof_hex,
                    AS11_BLE_SRP_PROOF_HEX_LENGTH + 1, HexCase::Upper) ||
        !hex_encode(master_key, sizeof(master_key), master_key_hex,
                    AS11_BLE_SRP_PROOF_HEX_LENGTH + 1, HexCase::Upper)) {
        memset(expected_server_proof_, 0,
               sizeof(expected_server_proof_));
        return false;
    }

    proof_ready_ = true;
    return true;
}

bool As11BleSrpClient::verify_server(const char *server_proof_hex) const {
    if (!proof_ready_ || !server_proof_hex) return false;

    uint8_t proof[AS11_BLE_SRP_PROOF_BYTES] = {};
    size_t proof_length = 0;
    if (!decode_hex(server_proof_hex, proof, sizeof(proof), proof_length) ||
        proof_length != sizeof(proof)) {
        return false;
    }

    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(proof); ++i) {
        difference |= proof[i] ^ expected_server_proof_[i];
    }
    return difference == 0;
}

void As11BleSrpClient::clear() {
    memset(private_key_, 0, sizeof(private_key_));
    memset(public_key_, 0, sizeof(public_key_));
    memset(expected_server_proof_, 0, sizeof(expected_server_proof_));
    started_ = false;
    proof_ready_ = false;
}

bool as11_ble_pairing_passkey_valid(const char *passkey) {
    if (!passkey || strlen(passkey) != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (passkey[i] < '0' || passkey[i] > '9') return false;
    }
    return true;
}

}  // namespace aircannect
