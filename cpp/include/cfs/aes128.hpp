// SPDX-License-Identifier: Apache-2.0
// Small AES-128 (FIPS-197) for single 16-byte blocks, as used by the stock tag
// reader (0x0802639A encrypt, 0x080263F8 decrypt). No tables beyond the S-boxes.
#pragma once

#include <cstdint>

namespace cfs::aes128 {

void encrypt_block(const uint8_t key[16], uint8_t block[16]);
void decrypt_block(const uint8_t key[16], uint8_t block[16]);

}  // namespace cfs::aes128
