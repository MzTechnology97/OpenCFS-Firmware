// SPDX-License-Identifier: Apache-2.0
#include "cfs/tags/decoders.hpp"

#include <cstring>

#include <initializer_list>

#include "cfs/creality_tag.hpp"
#include "cfs/sha256.hpp"

namespace cfs::tags {

namespace {
uint16_t u16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
uint16_t u16be(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t u24be(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 16 | static_cast<uint32_t>(p[1]) << 8 | p[2]; }
uint32_t u32be(const uint8_t* p) { return static_cast<uint32_t>(p[0]) << 24 | u24be(p + 1); }

// Trimmed printable length of a fixed-size text field.
size_t text_len(const uint8_t* p, size_t n) {
    size_t len = 0;
    for (size_t i = 0; i < n && p[i] >= 0x20 && p[i] < 0x7F; ++i) len = i + 1;
    while (len && p[len - 1] == ' ') --len;
    return len;
}

// material = a, or "a<sep>b" when b is not empty.
void set_material2(TagReadout& out, const uint8_t* a, size_t na, const uint8_t* b, size_t nb, char sep) {
    char buf[sizeof out.material];
    size_t n = 0;
    for (size_t i = 0, la = text_len(a, na); i < la && n < sizeof buf; ++i) buf[n++] = static_cast<char>(a[i]);
    const size_t lb = text_len(b, nb);
    if (lb && n < sizeof buf) buf[n++] = sep;
    for (size_t i = 0; i < lb && n < sizeof buf; ++i) buf[n++] = static_cast<char>(b[i]);
    out.set_material(buf, n);
}

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

// ---- Snapmaker U1 ---------------------------------------------------------------------
bool SnapmakerDecoder::derive_key(const uint8_t uid4[4], int sector, uint8_t key[6]) const {
    char info[12] = {'k', 'e', 'y', '_', 'a', '_'};
    size_t n = 6;
    if (sector >= 10) info[n++] = static_cast<char>('0' + sector / 10);
    info[n++] = static_cast<char>('0' + sector % 10);
    return sha256::hkdf(salt_, salt_len_, uid4, 4, reinterpret_cast<const uint8_t*>(info), n, key, 6);
}

bool SnapmakerDecoder::read(TagSession& session, TagReadout& out) {
    static const uint8_t kBlocks[] = {4, 5, 6, 8, 9, 10};  // sector 0 (vendor names) is not needed
    uint8_t data[sizeof kBlocks][16];
    int authed = -1;
    for (size_t i = 0; i < sizeof kBlocks; ++i) {
        const int sector = kBlocks[i] / 4;
        if (sector != authed) {
            uint8_t key[6];
            if (!derive_key(out.card.uid, sector, key) || !session.auth_a(kBlocks[i], key)) return false;
            authed = sector;
        }
        if (!session.read16(kBlocks[i], data[i])) return false;
    }
    static const char* const kMain[] = {nullptr, "PLA", "PETG", "ABS", "TPU", "PVA"};
    static const char* const kSub[] = {nullptr, "Basic", "Matte", "SnapSpeed"};
    const uint8_t* b4 = data[0];
    const unsigned main_type = u16le(b4 + 2), sub_type = u16le(b4 + 4);
    if (main_type == 0 || main_type > 5 || sub_type == 0 || sub_type > 3) return false;  // as the reference parser
    out.flags |= kFlagAuthenticated;
    out.vendor_hint = kHintSnapmaker;
    for (size_t i = 0; i < sizeof kBlocks; ++i) out.add_raw(kBlocks[i], data[i]);
    set_material2(out, reinterpret_cast<const uint8_t*>(kMain[main_type]), std::strlen(kMain[main_type]),
                  reinterpret_cast<const uint8_t*>(kSub[sub_type]), std::strlen(kSub[sub_type]), ' ');
    const uint8_t* b5 = data[1];
    out.rgba = static_cast<uint32_t>(b5[0]) << 24 | static_cast<uint32_t>(b5[1]) << 16 |
               static_cast<uint32_t>(b5[2]) << 8 | static_cast<uint8_t>(0xFF - b4[9]);
    const uint8_t* b8 = data[3];
    const uint8_t* b9 = data[4];
    out.diameter_centi_mm = u16le(b8);  // unit not documented [?]
    out.weight_g = u16le(b8 + 2);
    out.length_m = u16le(b8 + 4);
    out.nozzle_max_c = u16le(b9 + 4);
    out.nozzle_min_c = u16le(b9 + 6);
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

bool parse_ndef_message(const uint8_t* r, size_t l, TagReadout& out) {
    static const char kJson[] = "application/json";
    static const char kOt3d[] = "application/opentag3d";
    size_t q = 0;
    while (q + 3 <= l) {  // every record: header, type length, payload length, [id length]
        const uint8_t hdr = r[q++];
        const bool sr = hdr & 0x10, il = hdr & 0x08, me = hdr & 0x40;
        const uint8_t tnf = hdr & 0x07;
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
        size_t id_len = 0;
        if (il) {
            if (q >= l) return false;
            id_len = r[q++];
        }
        if (q + type_len + id_len > l) return false;
        const char* type = reinterpret_cast<const char*>(r + q);
        q += type_len + id_len;
        if (payload_len > l - q) {
            out.flags |= kFlagTruncated;
            payload_len = l - q;
        }
        const uint8_t* payload = r + q;
        if (tnf == 0x02 && type_len == sizeof kJson - 1 && std::strncmp(type, kJson, type_len) == 0) {
            const char* json = reinterpret_cast<const char*>(payload);
            char v[24];
            if (json_string(json, payload_len, "type", v, sizeof v)) out.set_material(v, std::strlen(v));
            if (json_string(json, payload_len, "color_hex", v, sizeof v)) parse_rgb(v, std::strlen(v), out.rgba);
            if (json_string(json, payload_len, "min_temp", v, sizeof v)) out.nozzle_min_c = parse_uint(v, std::strlen(v));
            if (json_string(json, payload_len, "max_temp", v, sizeof v)) out.nozzle_max_c = parse_uint(v, std::strlen(v));
            out.vendor_hint = kHintOpenSpool;
            return true;
        }
        if (tnf == 0x02 && type_len == sizeof kOt3d - 1 && std::strncmp(type, kOt3d, type_len) == 0) {
            return parse_opentag3d(payload, payload_len, out);
        }
        q += payload_len;
        if (me) break;
    }
    return false;
}

// OpenTag3D core fields (spec v2, big endian, missing bytes read as 0).
bool parse_opentag3d(const uint8_t* payload, size_t len, TagReadout& out) {
    uint8_t p[0xE0] = {};
    for (size_t i = 0; i < len && i < sizeof p; ++i) p[i] = payload[i];
    set_material2(out, p + 0x02, 5, p + 0x07, 5, ' ');
    out.rgba = u32be(p + 0x3C);
    out.diameter_centi_mm = static_cast<uint16_t>(u16be(p + 0x8C) / 10);  // um -> 1/100 mm
    out.nozzle_min_c = static_cast<uint16_t>(p[0x91] * 5);
    out.nozzle_max_c = static_cast<uint16_t>(p[0x92] * 5);
    out.weight_g = u16be(p + 0x9E);
    out.length_m = u16be(p + 0xA2);
    out.vendor_hint = kHintOpenTag3D;
    return true;
}

// TigerTag (TigerTag-RFID-Guide section 2, big endian, offsets from page 4).
bool parse_tigertag(const uint8_t* m, size_t len, TagReadout& out) {
    if (len < 80) return false;
    const uint32_t id = u32be(m);
    constexpr uint32_t kTigerTag = 0x5BF59264, kTigerTagPlus = 0xBC0FCB97, kTigerTagInit = 0x6C41A2E1;
    if (id != kTigerTag && id != kTigerTagPlus && id != kTigerTagInit) return false;
    out.vendor_hint = kHintTigerTag;
    if (id == kTigerTagInit) return true;  // initialized but blank
    // The host maps the material ID to a name (database/id_material.json).
    char mat[16] = {'T', 'T'};
    size_t n = 2;
    char digits[6];
    size_t d = 0;
    unsigned v = u16be(m + 8);
    do {
        digits[d++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v && d < sizeof digits);
    while (d) mat[n++] = digits[--d];
    out.set_material(mat, n);
    if (m[13] == 0x38) out.diameter_centi_mm = 175;
    if (m[13] == 0xDD) out.diameter_centi_mm = 285;
    out.rgba = u32be(m + 16);
    const uint32_t measure = u24be(m + 20);
    switch (m[23]) {  // database/id_measure_unit.json
    case 21: out.weight_g = static_cast<uint16_t>(measure); break;         // g
    case 35: out.weight_g = static_cast<uint16_t>(measure * 1000); break;  // kg
    case 149: out.length_m = static_cast<uint16_t>(measure); break;        // m
    case 130: out.length_m = static_cast<uint16_t>(measure / 100); break;  // cm
    default: break;
    }
    out.nozzle_min_c = u16be(m + 24);
    out.nozzle_max_c = u16be(m + 26);
    return true;
}

// Anycubic ACE Pro (anycubic-nfc-filament format.md): magic 0x7B, version 0x64/0x65,
// little-endian values, offsets from page 4.
bool parse_anycubic(const uint8_t* m, size_t len, TagReadout& out) {
    if (len < 112 || m[0] != 0x7B || (m[2] != 0x64 && m[2] != 0x65)) return false;
    out.vendor_hint = kHintAnycubic;
    out.set_material(reinterpret_cast<const char*>(m + 44), text_len(m + 44, 20));  // page 0x0F
    out.rgba = static_cast<uint32_t>(m[67]) << 24 | static_cast<uint32_t>(m[66]) << 16 |
               static_cast<uint32_t>(m[65]) << 8 | m[64];  // page 0x14: A B G R
    out.nozzle_min_c = u16le(m + 80);                       // page 0x18
    uint16_t max = u16le(m + 82);
    for (size_t off : {static_cast<size_t>(90), static_cast<size_t>(98)}) {  // HS ranges 2 and 3
        const uint16_t v = u16le(m + off);
        if (v > max) max = v;
    }
    out.nozzle_max_c = max;
    out.diameter_centi_mm = u16le(m + 104);  // page 0x1E
    out.length_m = u16le(m + 106);
    out.weight_g = u16le(m + 108);           // page 0x1F
    return true;
}

// ELEGOO (ELEGOO-RFID-Tag-Guide): packed big-endian EPC-256 from page 4, header
// 0x36 and manufacturer EEEEEEEE. Not checked against a real tag.
bool parse_elegoo(const uint8_t* m, size_t len, TagReadout& out) {
    if (len < 32 || m[0] != 0x36 || m[1] != 0xEE || m[2] != 0xEE || m[3] != 0xEE || m[4] != 0xEE) return false;
    out.vendor_hint = kHintElegoo;
    set_material2(out, m + 7, 4, m + 11, 4, '-');
    out.rgba = u24be(m + 15) << 8 | 0xFF;
    out.diameter_centi_mm = u16be(m + 18);
    out.weight_g = u16be(m + 20);
    return true;
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
        parse_ndef_message(mem + p, l, out);
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
    if (!parse_tigertag(mem, got, out) && !parse_anycubic(mem, got, out) && !parse_elegoo(mem, got, out)) {
        parse_ndef_json(mem, got, out);
    }
    return true;
}

// ---- NFC Type 4 / ISO-DEP ---------------------------------------------------------------
bool Type4Decoder::apdu(TagSession& s, const uint8_t* cmd, size_t len, uint8_t* resp, size_t cap,
                        size_t& resp_len) {
    uint8_t tx[1 + 16];
    if (len > sizeof tx - 1) return false;
    tx[0] = pcb_;
    for (size_t i = 0; i < len; ++i) tx[1 + i] = cmd[i];
    uint8_t rx[kChunk + 8];
    size_t n = 0;
    if (!s.transceive(tx, 1 + len, rx, sizeof rx, n)) return false;
    for (int wtx = 0; wtx < 3 && n >= 2 && (rx[0] & 0xF7) == 0xF2; ++wtx) {  // S(WTX): echo it
        const uint8_t ack[2] = {rx[0], rx[1]};
        if (!s.transceive(ack, 2, rx, sizeof rx, n)) return false;
    }
    if (n < 3 || (rx[0] & 0xE2) != 0x02) return false;  // I-block with SW1 SW2
    pcb_ ^= 0x01;                                       // block number toggles
    if (rx[n - 2] != 0x90 || rx[n - 1] != 0x00) return false;
    resp_len = n - 3;
    if (resp_len > cap) return false;
    for (size_t i = 0; i < resp_len; ++i) resp[i] = rx[1 + i];
    return true;
}

bool Type4Decoder::read(TagSession& session, TagReadout& out) {
    uint8_t ats[24];
    size_t n = 0;
    const uint8_t rats[2] = {0xE0, 0x50};  // FSDI 5 (64 bytes), CID 0
    if (!session.transceive(rats, 2, ats, sizeof ats, n) || n < 1) return false;
    pcb_ = 0x02;
    uint8_t r[kChunk];
    size_t rn = 0;
    static const uint8_t kSelectApp[] = {0x00, 0xA4, 0x04, 0x00, 0x07, 0xD2, 0x76,
                                         0x00, 0x00, 0x85, 0x01, 0x01, 0x00};
    static const uint8_t kSelectCc[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, 0xE1, 0x03};
    static const uint8_t kReadCc[] = {0x00, 0xB0, 0x00, 0x00, 0x0F};
    if (!apdu(session, kSelectApp, sizeof kSelectApp, r, sizeof r, rn)) return false;
    if (!apdu(session, kSelectCc, sizeof kSelectCc, r, sizeof r, rn)) return false;
    if (!apdu(session, kReadCc, sizeof kReadCc, r, sizeof r, rn) || rn < 15 || r[7] != 0x04) return false;
    const uint8_t fid_hi = r[9], fid_lo = r[10];
    size_t mle = static_cast<size_t>(r[3] << 8 | r[4]);
    if (mle == 0 || mle > kChunk) mle = kChunk;
    const uint8_t select_ndef[] = {0x00, 0xA4, 0x00, 0x0C, 0x02, fid_hi, fid_lo};
    if (!apdu(session, select_ndef, sizeof select_ndef, r, sizeof r, rn)) return false;
    const uint8_t read_nlen[] = {0x00, 0xB0, 0x00, 0x00, 0x02};
    if (!apdu(session, read_nlen, sizeof read_nlen, r, sizeof r, rn) || rn < 2) return false;
    size_t nlen = static_cast<size_t>(r[0] << 8 | r[1]);
    if (nlen > kMaxNdef) {
        out.flags |= kFlagTruncated;
        nlen = kMaxNdef;
    }
    uint8_t msg[kMaxNdef];
    size_t got = 0;
    while (got < nlen) {
        const size_t off = 2 + got;
        const size_t want = nlen - got < mle ? nlen - got : mle;
        const uint8_t rd[] = {0x00, 0xB0, static_cast<uint8_t>(off >> 8), static_cast<uint8_t>(off),
                              static_cast<uint8_t>(want)};
        if (!apdu(session, rd, sizeof rd, msg + got, sizeof msg - got, rn) || rn == 0) return false;
        got += rn;
    }
    for (size_t i = 0; i + 16 <= got && out.raw_count < TagReadout::kMaxRaw; i += 16) {
        out.add_raw(static_cast<uint8_t>(i / 16), msg + i);
    }
    out.flags |= kFlagNdef;
    parse_ndef_message(msg, got, out);
    const uint8_t deselect = 0xC2;
    session.transceive(&deselect, 1, r, sizeof r, rn);
    return true;
}

}  // namespace cfs::tags
