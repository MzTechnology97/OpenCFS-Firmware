// SPDX-License-Identifier: Apache-2.0
// RS-485 auto-addressing A0..A3 (docs/03-rs485-protocol.md section 6).
#pragma once

#include <cstdint>

#include "cfs/frame.hpp"
#include "cfs/hal.hpp"

namespace cfs {

enum class AutoCmd : uint8_t { Assign = 0xA0, Discover = 0xA1, Online = 0xA2, Table = 0xA3 };

class AutoAddress {
public:
    static constexpr uint8_t kUnassigned = 0xFE;
    static constexpr uint8_t kBroadcast = 0xFF;
    static constexpr uint32_t kDiscoverWindowMs = 1000;  // random reply delay 0..1000 ms
    static constexpr int kAssignRepeats = 3;

    AutoAddress(Hardware& hw, uint8_t initial_address);

    uint8_t address() const { return address_; }
    // True for A0..A3 frames this module must look at.
    static bool is_auto_command(uint8_t cmd) { return cmd >= 0xA0 && cmd <= 0xA3; }
    void handle(const Frame& f);
    // Sends replies whose time has come; call from the RS-485 loop.
    void poll();

private:
    struct Pending {
        bool used = false;
        uint8_t cmd = 0;
        uint8_t repeats = 0;
        uint32_t due_ms = 0;
    };

    void schedule(uint8_t cmd, uint32_t delay_ms, uint8_t repeats);
    uint32_t next_random();

    Hardware& hw_;
    uint8_t address_;
    uint8_t uid_[12] = {};
    uint32_t rng_;
    Pending pending_[4];
};

}  // namespace cfs
