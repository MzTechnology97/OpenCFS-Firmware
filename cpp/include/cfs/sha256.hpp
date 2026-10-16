// SPDX-License-Identifier: Apache-2.0
// SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and HKDF-SHA256 (RFC 5869),
// small and allocation-free. Used by the Bambu tag decoder's key derivation.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfs::sha256 {

constexpr size_t kDigest = 32;
constexpr size_t kBlock = 64;

class Hasher {
public:
    Hasher() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t len);
    void finish(uint8_t out[kDigest]);

private:
    void compress(const uint8_t block[kBlock]);
    uint32_t h_[8];
    uint8_t buf_[kBlock];
    size_t fill_ = 0;
    uint64_t bits_ = 0;
};

void digest(const uint8_t* data, size_t len, uint8_t out[kDigest]);
void hmac(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
          uint8_t out[kDigest]);
// out_len <= 255 * 32.
bool hkdf(const uint8_t* salt, size_t salt_len, const uint8_t* ikm, size_t ikm_len,
          const uint8_t* info, size_t info_len, uint8_t* out, size_t out_len);

}  // namespace cfs::sha256
