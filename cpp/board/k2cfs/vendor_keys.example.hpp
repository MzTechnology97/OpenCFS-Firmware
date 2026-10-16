// SPDX-License-Identifier: Apache-2.0
// Template for local/vendor_keys.hpp (git-ignored). Copy it there and fill in
// the values you hold. Never commit the filled copy: manufacturer secrets stay
// on your own computer.
#pragma once

#include <cstdint>

#include "cfs/tags/decoders.hpp"

namespace cfs::board::local {

// Bambu Lab: the HKDF-SHA256 salt of the tag Key A derivation
// (docs/spec/11 section 4). Set kHaveBambuSalt once the array holds it.
constexpr bool kHaveBambuSalt = false;
constexpr uint8_t kBambuSalt[16] = {};

// Snapmaker U1: the HKDF-SHA256 salt of the per-sector Key A derivation
// (25 ASCII bytes, no terminator; docs/spec/11 section 4).
constexpr bool kHaveSnapmakerSalt = false;
constexpr uint8_t kSnapmakerSalt[25] = {};

// MIFARE Classic keys tried in order on tags no other decoder read. The
// factory-default transport key (also used by QIDI Box tags) is the usual entry.
constexpr tags::KeyEntry kKeyList[] = {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, tags::KeyListDecoder::kHintQidi},
};

}  // namespace cfs::board::local
