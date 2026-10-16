// SPDX-License-Identifier: Apache-2.0
// Tag decoders (docs/11-multi-vendor-tags.md section 4). Vendor secrets are
// constructor parameters from the user's git-ignored local headers; a decoder
// built without them never matches.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cfs/tags/tag.hpp"

namespace cfs::tags {

// Creality MIFARE Classic 1K: Key A from the UID (K1), blocks 4..6 decrypted (K2).
class CrealityDecoder : public TagDecoder {
public:
    CrealityDecoder(const uint8_t* kdf_key, const uint8_t* record_key)
        : kdf_key_(kdf_key), record_key_(record_key) {}
    DecoderId id() const override { return DecoderId::Creality; }
    bool matches(const CardInfo& card) const override;
    bool read(TagSession& session, TagReadout& out) override;

private:
    const uint8_t* kdf_key_;
    const uint8_t* record_key_;
};

// Bambu Lab MIFARE Classic 1K: 16 Key A = HKDF-SHA256(salt, UID, "RFID-A\0", 96).
class BambuDecoder : public TagDecoder {
public:
    BambuDecoder(const uint8_t* salt, size_t salt_len) : salt_(salt), salt_len_(salt_len) {}
    DecoderId id() const override { return DecoderId::Bambu; }
    bool matches(const CardInfo& card) const override;
    bool read(TagSession& session, TagReadout& out) override;
    // Exposed for tests: the 16 sector keys for a 4-byte UID.
    bool derive_keys(const uint8_t uid4[4], uint8_t keys[16][6]) const;

private:
    const uint8_t* salt_;
    size_t salt_len_;
};

// MIFARE Classic with user-configured keys (factory default, QIDI, ...): raw
// blocks 4..6, plus a vendor hint for the host.
struct KeyEntry {
    uint8_t key[6];
    uint8_t vendor_hint;  // 0 generic, 1 QIDI
};

class KeyListDecoder : public TagDecoder {
public:
    static constexpr uint8_t kHintQidi = 1;
    KeyListDecoder(const KeyEntry* entries, size_t count) : entries_(entries), count_(count) {}
    DecoderId id() const override { return DecoderId::KeyList; }
    bool matches(const CardInfo& card) const override { return count_ > 0 && card.classic(); }
    bool read(TagSession& session, TagReadout& out) override;

private:
    const KeyEntry* entries_;
    size_t count_;
};

// NFC Type 2 (NTAG21x / Ultralight): user memory from page 4, NDEF TLV, OpenSpool
// JSON fields. Also builds the stock 40-hex-character record of pages 4..8.
class Type2Decoder : public TagDecoder {
public:
    static constexpr size_t kMaxBytes = 256;  // pages 4..67, room for OpenSpool JSON
    DecoderId id() const override { return DecoderId::Type2; }
    bool matches(const CardInfo& card) const override { return card.type2(); }
    bool read(TagSession& session, TagReadout& out) override;
};

// NFC Forum Type 4 (ISO-DEP): RATS, then the NDEF application, the capability
// container and the NDEF file with READ BINARY. Read-only: no UPDATE BINARY.
class Type4Decoder : public TagDecoder {
public:
    static constexpr size_t kMaxNdef = 192;
    static constexpr size_t kChunk = 48;  // fits the 64-byte reader FIFO with PCB, SW and CRC
    DecoderId id() const override { return DecoderId::Type4; }
    bool matches(const CardInfo& card) const override { return card.iso_dep(); }
    bool read(TagSession& session, TagReadout& out) override;

private:
    // One APDU in an I-block; returns the response data without SW (must be 90 00).
    bool apdu(TagSession& s, const uint8_t* cmd, size_t len, uint8_t* resp, size_t cap, size_t& resp_len);
    uint8_t pcb_ = 0x02;
};

// Helpers shared with tests.
// Type 2 memory: TLVs, then the first NDEF message.
bool parse_ndef_json(const uint8_t* mem, size_t len, TagReadout& out);
// One NDEF message (Type 4 file content after NLEN): OpenSpool JSON fields.
bool parse_ndef_message(const uint8_t* msg, size_t len, TagReadout& out);
bool json_string(const char* json, size_t len, const char* key, char* out, size_t out_len);

}  // namespace cfs::tags
