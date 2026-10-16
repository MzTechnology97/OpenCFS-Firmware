// SPDX-License-Identifier: Apache-2.0
#include "cfs/creality_tag.hpp"

#include "cfs/aes128.hpp"

namespace cfs::creality {

void derive_key_a(const uint8_t kdf_key[16], const uint8_t uid4[4], uint8_t key_a[6]) {
    uint8_t block[16];
    for (int i = 0; i < 16; ++i) block[i] = uid4[i % 4];
    aes128::encrypt_block(kdf_key, block);
    for (int i = 0; i < 6; ++i) key_a[i] = block[i];
}

void decrypt_block(const uint8_t record_key[16], uint8_t block[16]) {
    aes128::decrypt_block(record_key, block);
}

}  // namespace cfs::creality
