// SPDX-License-Identifier: Apache-2.0
// Blocked-motor supervision and reverse detooth (docs/06-motors-sensors.md section 4).
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/hal.hpp"

namespace cfs {

class MotorSupervisor {
public:
    static constexpr uint32_t kPeriodMs = 100;
    static constexpr uint32_t kStopAfterMs = 10000;
    static constexpr uint32_t kReenableMs = 11100;
    static constexpr uint32_t kHubKickMs = 60;
    static constexpr uint32_t kFeederPulseDelayMs = 200;
    static constexpr uint32_t kFeederPulseMs = 30;

    MotorSupervisor(Hardware& hw, Box& box, const Config& cfg) : hw_(hw), box_(box), cfg_(cfg) {}

    // Call every kPeriodMs (stock: RT soft timer). May block for a detooth (~300 ms).
    void tick();
    // false: never start a corrective move (detooth), only stop. Motion-locked
    // builds use it so that supervision can never move a motor.
    void set_corrective(bool on) { corrective_ = on; }

    // Bit 4 = hub, bits 0..3 = feeders, as captured when supervision stopped everything.
    uint8_t stopped_mask() const { return stopped_mask_; }
    bool overload_seen() const { return overload_; }
    uint32_t detooth_count() const { return detooth_count_; }

private:
    uint8_t blocked_mask();
    int active_slot();
    void detooth(int slot);

    Hardware& hw_;
    Box& box_;
    const Config& cfg_;  // supervision.reverse_detooth / detooth_after_ms
    uint32_t blocked_ms_ = 0;
    bool detoothing_ = false;
    bool corrective_ = true;
    uint32_t reenable_at_ = 0;
    uint8_t stopped_mask_ = 0;
    bool overload_ = false;
    uint32_t detooth_count_ = 0;
};

}  // namespace cfs
