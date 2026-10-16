// SPDX-License-Identifier: Apache-2.0
// Frame codec and byte-stream receiver for the CFS RS-485 link.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cfs/protocol.hpp"

namespace cfs {

// CRC-8, polynomial 0x07, init 0, MSB first, no final XOR.
uint8_t crc8(const uint8_t* data, size_t len);

struct Frame {
    uint8_t addr = 0;
    uint8_t header = 0;  // request header or reply status
    uint8_t cmd = 0;
    uint8_t len = 0;     // payload length
    uint8_t payload[kMaxPayload] = {};

    // `len` field as it appears on the wire.
    uint8_t wire_len() const { return static_cast<uint8_t>(len + 3); }
};

// Writes F7|addr|len+3|header|cmd|payload|crc into out. Returns bytes written,
// 0 if out is too small or the payload too long.
size_t encode(uint8_t addr, uint8_t header, uint8_t cmd, const uint8_t* payload, size_t len,
              uint8_t* out, size_t out_size);

// Byte-at-a-time receiver with address filtering and inter-byte timeout.
class Receiver {
public:
    enum class Result { None, Frame, BadCrc };

    explicit Receiver(uint8_t own_address = 1) : own_(own_address) {}
    void set_address(uint8_t a) { own_ = a; }

    // Feeds one byte received at `now_ms`. A complete frame is copied to `out`.
    Result push(uint8_t byte, uint32_t now_ms, Frame& out);
    // Discards a partial frame when the line has been idle for kInterByteTimeoutMs.
    void poll_timeout(uint32_t now_ms);
    void reset() { state_ = State::Sync; }
    bool idle() const { return state_ == State::Sync; }

private:
    enum class State { Sync, Addr, Len, Header, Cmd, Payload, Crc };
    bool accepts(uint8_t addr) const;

    uint8_t own_;
    State state_ = State::Sync;
    uint8_t wire_len_ = 0;
    uint8_t got_ = 0;
    uint32_t last_ms_ = 0;
    Frame cur_;
};

}  // namespace cfs
