// SPDX-License-Identifier: Apache-2.0
// Stock-compatible parameter page ("database", docs/10-board-port.md section 1).
// The stock firmware keeps a 16-byte record at the start of the last flash page
// (0x0807F800):
//   0x00 u32  CRC-16/XMODEM of bytes 0x08..0x0F (upper 16 bits zero)
//   0x04 u32  payload length = 8
//   0x08 u32  key 5: firmware version marker (stock 1.5.3 writes 0x00010503)
//   0x0C u8   key 6: box mode (0 = standard), read by 0x0C, written by 0x50
//   0x0D..0x0F padding (covered by the CRC)
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfs::params {

constexpr size_t kRecordSize = 16;
constexpr uint32_t kPayloadLen = 8;
constexpr uint32_t kStockVersion = 0x00010503;

struct Record {
    uint32_t version = 0;
    uint8_t mode = 0;
    uint8_t pad[3] = {};
};

// CRC-16/XMODEM: poly 0x1021, init 0, MSB first, no final xor (stock 0x08011CE8).
uint16_t crc16_xmodem(const uint8_t* data, size_t len);

// True when the stored CRC matches. The length word is not checked (stock does not).
bool decode(const uint8_t raw[kRecordSize], Record& out);
void encode(const Record& rec, uint8_t raw[kRecordSize]);

// Stock load policy (0x08011EBA): a bad CRC resets to {version, mode 0}; a valid
// record with another version keeps the mode and only rewrites the version.
// Returns true when the record must be written back.
bool load(const uint8_t raw[kRecordSize], uint32_t version, Record& out);

}  // namespace cfs::params
