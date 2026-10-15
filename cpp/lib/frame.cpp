// SPDX-License-Identifier: Apache-2.0
#include "cfs/frame.hpp"

namespace cfs {

uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
        }
    }
    return crc;
}

size_t encode(uint8_t addr, uint8_t header, uint8_t cmd, const uint8_t* payload, size_t len,
              uint8_t* out, size_t out_size) {
    if (len > kMaxPayload || out_size < len + 6) {
        return 0;
    }
    out[0] = kSync;
    out[1] = addr;
    out[2] = static_cast<uint8_t>(len + 3);
    out[3] = header;
    out[4] = cmd;
    for (size_t i = 0; i < len; ++i) {
        out[5 + i] = payload[i];
    }
    out[5 + len] = crc8(out + 2, len + 3);
    return len + 6;
}

bool Receiver::accepts(uint8_t addr) const {
    return addr == own_ || addr == kAddrSilentBroadcast || addr == kAddrBoxBroadcast;
}

void Receiver::poll_timeout(uint32_t now_ms) {
    if (state_ != State::Sync && now_ms - last_ms_ >= kInterByteTimeoutMs) {
        state_ = State::Sync;
    }
}

Receiver::Result Receiver::push(uint8_t byte, uint32_t now_ms, Frame& out) {
    poll_timeout(now_ms);
    last_ms_ = now_ms;
    switch (state_) {
    case State::Sync:
        if (byte == kSync) {
            state_ = State::Addr;
        }
        return Result::None;
    case State::Addr:
        if (!accepts(byte)) {
            state_ = State::Sync;
            return Result::None;
        }
        cur_.addr = byte;
        state_ = State::Len;
        return Result::None;
    case State::Len:
        if (byte < 3) {
            state_ = State::Sync;
            return Result::None;
        }
        wire_len_ = byte;
        cur_.len = static_cast<uint8_t>(byte - 3);
        state_ = State::Header;
        return Result::None;
    case State::Header:
        cur_.header = byte;
        state_ = State::Cmd;
        return Result::None;
    case State::Cmd:
        cur_.cmd = byte;
        got_ = 0;
        state_ = cur_.len ? State::Payload : State::Crc;
        return Result::None;
    case State::Payload:
        cur_.payload[got_++] = byte;
        if (got_ == cur_.len) {
            state_ = State::Crc;
        }
        return Result::None;
    case State::Crc: {
        state_ = State::Sync;
        uint8_t buf[kMaxPayload + 3];
        buf[0] = wire_len_;
        buf[1] = cur_.header;
        buf[2] = cur_.cmd;
        for (uint8_t i = 0; i < cur_.len; ++i) {
            buf[3 + i] = cur_.payload[i];
        }
        if (crc8(buf, cur_.len + 3u) != byte) {
            return Result::BadCrc;
        }
        out = cur_;
        return Result::Frame;
    }
    }
    return Result::None;
}

}  // namespace cfs
