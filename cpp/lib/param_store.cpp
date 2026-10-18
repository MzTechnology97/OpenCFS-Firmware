// SPDX-License-Identifier: Apache-2.0
#include "cfs/param_store.hpp"

namespace cfs::params {

namespace {
uint32_t get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
}  // namespace

uint16_t crc16_xmodem(const uint8_t* data, size_t len) {
    uint16_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = static_cast<uint16_t>(crc ^ (data[i] << 8));
        for (int b = 0; b < 8; ++b)
            crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

bool decode(const uint8_t raw[kRecordSize], Record& out) {
    if (get32(raw) != crc16_xmodem(raw + 8, kPayloadLen)) return false;
    out.version = get32(raw + 8);
    out.mode = raw[12];
    for (int i = 0; i < 3; ++i) out.pad[i] = raw[13 + i];
    return true;
}

void encode(const Record& rec, uint8_t raw[kRecordSize]) {
    put32(raw + 8, rec.version);
    raw[12] = rec.mode;
    for (int i = 0; i < 3; ++i) raw[13 + i] = rec.pad[i];
    put32(raw, crc16_xmodem(raw + 8, kPayloadLen));
    put32(raw + 4, kPayloadLen);
}

bool load(const uint8_t raw[kRecordSize], uint32_t version, Record& out) {
    if (!decode(raw, out)) {
        out = Record{};
        out.version = version;
        return true;
    }
    if (out.version != version) {
        out.version = version;
        return true;
    }
    return false;
}

}  // namespace cfs::params
