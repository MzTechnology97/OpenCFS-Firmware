// SPDX-License-Identifier: Apache-2.0
// Creality tag cryptography as the stock reader applies it (0x080260A6,
// 0x08026136; docs/05-preload-rfid.md section 7). The keys are parameters:
// OpenCFS does not ship them.
#pragma once

#include <cstdint>

namespace cfs::creality {

// Key A = first 6 bytes of AES-128(kdf_key, uid4 repeated four times).
void derive_key_a(const uint8_t kdf_key[16], const uint8_t uid4[4], uint8_t key_a[6]);

// One record block (4, 5 or 6) = AES-128 decryption of the stored block.
void decrypt_block(const uint8_t record_key[16], uint8_t block[16]);

}  // namespace cfs::creality
