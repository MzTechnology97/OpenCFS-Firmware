// SPDX-License-Identifier: Apache-2.0
// TagCrypto backed by the Creality keys of your own stock firmware image.
// tools/re/extract_tag_keys.py writes local/creality_keys.hpp, which is
// git-ignored and never published. Without it the build still works: Creality
// MIFARE tags then report a UID but no record.
#pragma once

#include "cfs/creality_tag.hpp"
#include "fm17622.hpp"

#if __has_include("local/creality_keys.hpp")
#include "local/creality_keys.hpp"
#define CFS_HAVE_CREALITY_KEYS 1
#else
#define CFS_HAVE_CREALITY_KEYS 0
#endif

namespace cfs::board {

class CrealityTagCrypto : public TagCrypto {
public:
    static constexpr bool kHaveKeys = CFS_HAVE_CREALITY_KEYS != 0;

    bool key_a(const uint8_t uid4[4], uint8_t key[6]) override {
#if CFS_HAVE_CREALITY_KEYS
        creality::derive_key_a(local::kCrealityKeyAKdf, uid4, key);
        return true;
#else
        (void)uid4;
        (void)key;
        return false;
#endif
    }

    bool decrypt_block(uint8_t block[16]) override {
#if CFS_HAVE_CREALITY_KEYS
        creality::decrypt_block(local::kCrealityRecordKey, block);
        return true;
#else
        (void)block;
        return false;
#endif
    }
};

}  // namespace cfs::board
