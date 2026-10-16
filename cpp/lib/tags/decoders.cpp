// SPDX-License-Identifier: Apache-2.0
#include "cfs/tags/decoders.hpp"

#include <cstring>

#include "cfs/creality_tag.hpp"
#include "cfs/sha256.hpp"

namespace cfs::tags {

namespace {
uint16_t u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// "RRGGBB" (optionally after '#') -> 0xRRGGBBFF; false if malformed.
bool parse_rgb(const char* s, size_t len, uint32_t& rgba) {
    if (len && s[0] == '#') {
        ++s;
        --len;
    }
    if (len < 6) return false;
    uint32_t v = 0;
    for (int i = 0; i < 6; ++i) {
        const int n = hex_nibble(s[i]);
        if (n < 0) return false;
        v = v << 4 | static_cast<uint32_t>(n);
    }
    rgba = v << 8 | 0xFF;
    return true;
}

uint16_t parse_uint(const char* s, size_t len) {
    uint32_t v = 0;
    for (size_t i = 0; i < len && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + static_cast<uint32_t>(s[i] - '0');
        if (v > 0xFFFF) return 0;
    }
    return static_cast<uint16_t>(v);
}
}  // namespace

// ---- Creality ---------------------------------------------------------------------
bool CrealityDecoder::matches(const CardInfo& card) const {
    return kdf_key_ && record_key_ && card.classic_1k() && card.uid_len == 4;
}

bool CrealityDecoder::read(TagSession& session, TagReadout& out) {
    uint8_t key[6];
    creality::derive_key_a(kdf_key_, out.card.uid, key);
    if (!session.auth_a(4, key)) return false;
    out.flags |= kFlagAuthenticated;
    size_t pos = 0;
    for (uint8_t b = 4; b <= 6; ++b) {
        uint8_t data[16];
        if (!session.read16(b, data)) return false;
        creality::decrypt_block(record_key_, data);
        out.add_raw(b, data);
        for (size_t i = 0; i < 16 && pos < kTagRecordLen; ++i) out.creality.text[pos++] = static_cast<char>(data[i]);
    }
    out.has_creality = true;
    const char* t = out.creality.text;  // layout: docs/05-preload-rfid.md section 7
    out.set_material(t + 11, 6);        // Creality material id
    parse_rgb(t + 18, 6, out.rgba);     // color: prefix character, then RRGGBB
    out.length_m = parse_uint(t + 24, 4);
    return true;
}

// ---- Bambu ------------------------------------------------------------------------
bool BambuDecoder::matches(const CardInfo& card) const {
    return salt_ && salt_len_ && card.classic_1k() && card.uid_len == 4;
}

bool BambuDecoder::derive_keys(const uint8_t uid4[4], uint8_t keys[16][6]) const {
    static const uint8_t kInfo[] = {'R', 'F', 'I', 'D', '-', 'A', 0};
    uint8_t okm[16 * 6];
    if (!sha256::hkdf(salt_, salt_len_, uid4, 4, kInfo, sizeof kInfo, okm, sizeof okm)) return false;
    for (int s = 0; s < 16; ++s)
        for (int i = 0; i < 6; ++i) keys[s][i] = okm[6 * s + i];
    return true;
}

bool BambuDecoder::read(TagSession& session, TagReadout& out) {
    uint8_t keys[16][6];
    if (!derive_keys(out.card.uid, keys)) return false;
    static const uint8_t kBlocks[] = {1, 2, 4, 5, 6, 14};
    uint8_t data[sizeof kBlocks][16];
    int authed_sector = -1;
    for (size_t i = 0; i < sizeof kBlocks; ++i) {
        const uint8_t b = kBlocks[i];
        const int sector = b / 4;
        if (sector != authed_sector) {
            if (!session.auth_a(b, keys[sector])) return false;
            authed_sector = sector;
        }
        if (!session.read16(b, data[i])) return false;
    }
    float diameter;
    std::memcpy(&diameter, data[3] + 8, sizeof diameter);  // block 5, float LE
    if (!(diameter >= 0.5f && diameter <= 4.0f)) return false;
    out.flags |= kFlagAuthenticated;
    for (size_t i = 0; i < sizeof kBlocks; ++i) out.add_raw(kBlocks[i], data[i]);
    out.set_material(reinterpret_cast<const char*>(data[1]), 16);  // block 2: filament type
    const uint8_t* b5 = data[3];
    const uint8_t* b6 = data[4];
    out.rgba = static_cast<uint32_t>(b5[0]) << 24 | static_cast<uint32_t>(b5[1]) << 16 |
               static_cast<uint32_t>(b5[2]) << 8 | b5[3];
    out.weight_g = u16le(b5 + 4);
    out.diameter_centi_mm = static_cast<uint16_t>(diameter * 100.0f + 0.5f);
    out.nozzle_max_c = u16le(b6 + 8);
    out.nozzle_min_c = u16le(b6 + 10);
    out.length_m = u16le(data[5] + 4);  // block 14
    return true;
}

// ---- configured key list ---------------------------------------------------------------
bool KeyListDecoder::read(TagSession& session, TagReadout& out) {
    for (size_t e = 0; e < count_; ++e) {
        if (e > 0 && !session.reactivate()) return false;
        if (!session.auth_a(4, entries_[e].key)) continue;
        out.flags |= kFlagAuthenticated;
        for (uint8_t b = 4; b <= 6; ++b) {
            uint8_t data[16];
            if (!session.read16(b, data)) return false;
            out.add_raw(b, data);
        }
        out.vendor_hint = entries_[e].vendor_hint;
        return true;
    }
    return false;
}

// ---- NFC Type 2 / NDEF ------------------------------------------------------------------
bool json_string(const char* json, size_t len, const char* key, char* out, size_t out_len) {
    const size_t klen = std::strlen(key);
    for (size_t i = 0; i + klen + 2 < len; ++i) {
        if (json[i] != '"' || std::strncmp(json + i + 1, key, klen) != 0 || json[i + 1 + klen] != '"') continue;
        size_t p = i + klen + 2;
        while (p < len && (json[p] == ' ' || json[p] == ':')) ++p;
        const bool quoted = p < len && json[p] == '"';
        if (quoted) ++p;
        size_t n = 0;
        while (p < len && n + 1 < out_len) {
            const char c = json[p];
            if (quoted ? c == '"' : (c == ',' || c == '}' || c == ' ')) break;
            out[n++] = c;
            ++p;
        }
        out[n] = '\0';
        return n > 0;
    }
    return false;
}

bool parse_ndef_json(const uint8_t* mem, size_t len, TagReadout& out) {
    size_t p = 0;
    while (p < len) {
        const uint8_t t = mem[p++];
        if (t == 0x00) continue;           // NULL TLV
        if (t == 0xFE || p >= len) break;  // terminator
        size_t l = mem[p++];
        if (l == 0xFF) {
            if (p + 2 > len) break;
            l = static_cast<size_t>(mem[p] << 8 | mem[p + 1]);
            p += 2;
        }
        if (t != 0x03) {  // lock / memory control TLVs
            p += l;
            continue;
        }
        if (p + l > len) {
            out.flags |= kFlagTruncated;
            l = len - p;
        }
        out.flags |= kFlagNdef;
        // First NDEF record only: header, type length, payload length, [id length].
        const uint8_t* r = mem + p;
        if (l < 3) return false;
        const uint8_t hdr = r[0];
        const bool sr = hdr & 0x10, il = hdr & 0x08;
        const uint8_t tnf = hdr & 0x07;
        size_t q = 1;
        const size_t type_len = r[q++];
        size_t payload_len;
        if (sr) {
            payload_len = r[q++];
        } else {
            if (q + 4 > l) return false;
            payload_len = static_cast<size_t>(r[q]) << 24 | static_cast<size_t>(r[q + 1]) << 16 |
                          static_cast<size_t>(r[q + 2]) << 8 | r[q + 3];
            q += 4;
        }
        const size_t id_len = il ? r[q++] : 0;
        const char* type = reinterpret_cast<const char*>(r + q);
        q += type_len + id_len;
        if (q > l) return false;
        if (payload_len > l - q) {
            out.flags |= kFlagTruncated;
            payload_len = l - q;
        }
        static const char kJson[] = "application/json";
        if (tnf != 0x02 || type_len != sizeof kJson - 1 || std::strncmp(type, kJson, type_len) != 0) return true;
        const char* json = reinterpret_cast<const char*>(r + q);
        char v[24];
        if (json_string(json, payload_len, "type", v, sizeof v)) out.set_material(v, std::strlen(v));
        if (json_string(json, payload_len, "color_hex", v, sizeof v)) parse_rgb(v, std::strlen(v), out.rgba);
        if (json_string(json, payload_len, "min_temp", v, sizeof v)) out.nozzle_min_c = parse_uint(v, std::strlen(v));
        if (json_string(json, payload_len, "max_temp", v, sizeof v)) out.nozzle_max_c = parse_uint(v, std::strlen(v));
        return true;
    }
    return false;
}

namespace {
// Bytes needed to hold the first NDEF TLV, from the TLV headers read so far;
// 0 while unknown.
size_t ndef_bytes_needed(const uint8_t* mem, size_t got) {
    size_t p = 0;
    while (p < got) {
        const uint8_t t = mem[p];
        if (t == 0x00) {
            ++p;
            continue;
        }
        if (t == 0xFE) return p + 1;
        if (p + 2 > got) return 0;
        size_t l = mem[p + 1], hdr = 2;
        if (l == 0xFF) {
            if (p + 4 > got) return 0;
            l = static_cast<size_t>(mem[p + 2] << 8 | mem[p + 3]);
            hdr = 4;
        }
        if (t == 0x03) return p + hdr + l;
        p += hdr + l;
    }
    return 0;
}
}  // namespace

bool Type2Decoder::read(TagSession& session, TagReadout& out) {
    uint8_t mem[kMaxBytes];
    size_t got = 0;
    for (uint8_t page = 4; got < kMaxBytes; page = static_cast<uint8_t>(page + 4)) {
        if (!session.read16(page, mem + got)) break;  // end of user memory
        out.add_raw(page, mem + got);
        got += 16;
        const size_t need = ndef_bytes_needed(mem, got);
        if (got >= 32 && need && got >= need) break;  // whole NDEF message read
    }
    if (got < 32) return false;  // stock needs pages 4..11 for its record
    static const char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < kTagRecordLen / 2; ++i) {  // stock record: pages 4..8 as hex
        out.creality.text[2 * i] = kHex[mem[i] >> 4];
        out.creality.text[2 * i + 1] = kHex[mem[i] & 0x0F];
    }
    out.has_creality = true;
    parse_ndef_json(mem, got, out);
    return true;
}

}  // namespace cfs::tags
