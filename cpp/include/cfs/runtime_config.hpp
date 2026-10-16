// SPDX-License-Identifier: Apache-2.0
// Runtime Config API v2, wire-compatible with K2-OpenHost's box_cfs_runtime.py
// (command 0x57, sub-commands 0x10..0x14, 28 parameters, RAM only), with an MCU
// motion interlock (docs/12-runtime-config.md).
#pragma once

#include <cstddef>
#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/hal.hpp"

namespace cfs {

// SET/RESET are accepted only while the box is provably at rest: IDLE state, no
// motion scope open, every commanded motor output off/braked (read back by the
// board drivers) and no FG pulse, continuously for kSettleMs. Everything runs in
// the main loop, and no interrupt starts a motor, so checking and applying a value
// cannot race with a motor start.
class MotionInterlock {
public:
    static constexpr uint32_t kSettleMs = 500;
    MotionInterlock(Hardware& hw, const Box& box) : hw_(hw), box_(box) {}
    void poll();          // call every main-loop pass
    bool at_rest();       // true once at rest for kSettleMs

private:
    bool idle_now();
    Hardware& hw_;
    const Box& box_;
    bool started_ = false;
    uint32_t busy_at_ = 0;
};

class RuntimeConfig {
public:
    static constexpr uint8_t kVersion = 2;
    static constexpr uint8_t kCount = 28;
    static constexpr uint8_t kDescriptorRevision = 1;
    // bit 0 GET, bit 1 SET, bit 3 DESCRIBE, bit 5 MCU motion-interlocked SET.
    static constexpr uint8_t kFeatures = 0x2B;

    enum : uint8_t { kSubInfo = 0x10, kSubGet = 0x11, kSubSet = 0x12, kSubReset = 0x13, kSubDescribe = 0x14 };
    enum : uint8_t { kStatusOk = 0, kStatusBadRequest = 1, kStatusBusy = 7 };
    enum : uint8_t { kTypeBool = 0, kTypeU8 = 1, kTypeU16 = 2 };
    enum : uint8_t {
        kFlagWritable = 1u << 0, kFlagIdleOnly = 1u << 1, kFlagAdvanced = 1u << 2,
        kFlagRfid = 1u << 3, kFlagSafety = 1u << 4,
    };

    struct Param {
        uint8_t type;
        uint8_t flags;
        uint16_t def, min, max;
        uint16_t (*get)(const Config&);
        void (*set)(Config&, uint16_t);  // nullptr: read-only in OpenCFS
    };
    static const Param kParams[kCount];

    RuntimeConfig(Config& cfg, MotionInterlock& interlock) : cfg_(cfg), interlock_(interlock) {}

    // Handles one 0x57 request payload. Returns the status; `out`/`out_len` is the
    // reply payload (empty on errors, as box_cfs_runtime.py requires).
    uint8_t handle(const uint8_t* req, size_t len, uint8_t* out, size_t& out_len);

private:
    Config& cfg_;
    MotionInterlock& interlock_;
};

}  // namespace cfs
