// SPDX-License-Identifier: Apache-2.0
// The tag decoders of the board (docs/spec/11 section 4), built from the
// user's git-ignored local headers:
//   local/creality_keys.hpp  tools/re/extract_tag_keys.py from your stock image
//   local/vendor_keys.hpp    copy vendor_keys.example.hpp and fill it in
// A decoder whose keys are missing is compiled in its disabled form and never
// matches. Nothing here writes to a tag.
#pragma once

#include "cfs/tags/decoders.hpp"

#if __has_include("local/creality_keys.hpp")
#include "local/creality_keys.hpp"
#define CFS_HAVE_CREALITY_KEYS 1
#else
#define CFS_HAVE_CREALITY_KEYS 0
#endif

#if __has_include("local/vendor_keys.hpp")
#include "local/vendor_keys.hpp"
#define CFS_HAVE_VENDOR_KEYS 1
#else
#define CFS_HAVE_VENDOR_KEYS 0
#endif

namespace cfs::board {

class VendorDecoders {
public:
    static constexpr size_t kCount = 4;

    VendorDecoders()
#if CFS_HAVE_CREALITY_KEYS
        : creality_(local::kCrealityKeyAKdf, local::kCrealityRecordKey),
#else
        : creality_(nullptr, nullptr),
#endif
#if CFS_HAVE_VENDOR_KEYS
          bambu_(local::kHaveBambuSalt ? local::kBambuSalt : nullptr, sizeof local::kBambuSalt),
          key_list_(local::kKeyList, sizeof local::kKeyList / sizeof local::kKeyList[0]),
#else
          bambu_(nullptr, 0),
          key_list_(nullptr, 0),
#endif
          list_{&creality_, &bambu_, &key_list_, &type2_} {
    }

    tags::TagDecoder* const* list() const { return list_; }

private:
    tags::CrealityDecoder creality_;
    tags::BambuDecoder bambu_;
    tags::KeyListDecoder key_list_;
    tags::Type2Decoder type2_;
    tags::TagDecoder* list_[kCount];
};

}  // namespace cfs::board
