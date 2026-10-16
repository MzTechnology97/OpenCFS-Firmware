// SPDX-License-Identifier: Apache-2.0
// Multi-vendor tag reading (docs/11-multi-vendor-tags.md). Read-only by design:
// a TagSession has no write primitive.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cfs/hal.hpp"

namespace cfs::tags {

struct CardInfo {
    uint8_t atqa[2] = {};
    uint8_t uid[10] = {};
    uint8_t uid_len = 0;  // 4, 7 or 10
    uint8_t sak = 0;

    bool classic() const { return sak == 0x08 || sak == 0x09 || sak == 0x18 || sak == 0x88; }
    bool classic_1k() const { return atqa[0] == 0x04 && atqa[1] == 0x00 && sak == 0x08; }
    // NFC Type 2 (NTAG21x, Ultralight): SAK 0x00.
    bool type2() const { return sak == 0x00; }
};

// One activated tag in the reader field.
class TagSession {
public:
    virtual ~TagSession() = default;
    virtual const CardInfo& card() const = 0;
    // Select the tag again (after a failed authentication) without a new revolution.
    virtual bool reactivate() = 0;
    // MIFARE Classic Key A authentication of the sector holding `block`.
    virtual bool auth_a(uint8_t block, const uint8_t key[6]) = 0;
    // READ 0x30: one Classic block, or four Type 2 pages starting at `index`.
    virtual bool read16(uint8_t index, uint8_t out[16]) = 0;
};

enum class DecoderId : uint8_t { None = 0, Creality = 1, Bambu = 2, KeyList = 3, Type2 = 4 };

enum : uint8_t { kFlagAuthenticated = 1u << 0, kFlagNdef = 1u << 1, kFlagTruncated = 1u << 2 };

struct TagReadout {
    static constexpr size_t kMaxRaw = 6;
    struct Raw {
        uint8_t index;
        uint8_t data[16];
    };

    DecoderId decoder = DecoderId::None;
    uint8_t flags = 0;
    CardInfo card;
    char material[16] = {};  // ASCII, not terminated when full
    uint32_t rgba = 0;
    uint16_t weight_g = 0;
    uint16_t length_m = 0;
    uint16_t diameter_centi_mm = 0;
    uint16_t nozzle_min_c = 0;
    uint16_t nozzle_max_c = 0;
    uint8_t vendor_hint = 0;
    bool has_creality = false;
    TagRecord creality;  // 40-character stock record, Creality tags only
    Raw raw[kMaxRaw] = {};
    uint8_t raw_count = 0;

    void add_raw(uint8_t index, const uint8_t data[16]);
    void set_material(const char* text, size_t len);
};

// Command 0x60 reply payload (docs/11-multi-vendor-tags.md section 5).
constexpr size_t kTagInfoHeader = 49;
size_t encode_tag_info(uint8_t slot, const TagReadout& r, uint8_t* out, size_t cap);

class TagDecoder {
public:
    virtual ~TagDecoder() = default;
    virtual DecoderId id() const = 0;
    // False when the decoder is not built in (missing local keys) or the card
    // technology does not fit.
    virtual bool matches(const CardInfo& card) const = 0;
    virtual bool read(TagSession& session, TagReadout& out) = 0;
};

// Runs the decoders in order inside one field window, the last decoder that read
// a UID first (8-entry RAM cache).
class TagPipeline {
public:
    TagPipeline(TagDecoder* const* decoders, size_t count) : decoders_(decoders), count_(count) {}
    bool read(TagSession& session, TagReadout& out);

private:
    struct Route {
        uint8_t uid[10];
        uint8_t uid_len;
        uint8_t decoder;  // index + 1, 0 = empty
    };
    int cached(const CardInfo& card) const;
    void remember(const CardInfo& card, size_t index);

    TagDecoder* const* decoders_;
    size_t count_;
    Route routes_[8] = {};
    uint8_t next_route_ = 0;
};

}  // namespace cfs::tags
