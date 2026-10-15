// SPDX-License-Identifier: Apache-2.0
#include "cfs/supervisor.hpp"

namespace cfs {

uint8_t MotorSupervisor::blocked_mask() {
    uint8_t m = hw_.hub.blocked() ? 0x10 : 0;
    for (int s = 0; s < kSlotCount; ++s) {
        if (hw_.feeders.blocked(s)) m |= static_cast<uint8_t>(1u << s);
    }
    return m;
}

int MotorSupervisor::active_slot() {
    for (int s = 0; s < kSlotCount; ++s) {
        if (hw_.sensors.hub_present(s)) return s;
    }
    return -1;
}

void MotorSupervisor::detooth(int slot) {
    MotionScope m(box_);
    ++detooth_count_;
    const Dir hub_dir = hw_.hub.current_dir();
    const uint8_t hub_speed = hw_.hub.current_speed();
    // Kick the hub briefly against its current direction to release the gear...
    hw_.hub.stop();
    hw_.hub.run(hub_dir == Dir::Forward ? Dir::Reverse : Dir::Forward, 255);
    hw_.clock.sleep_ms(kHubKickMs);
    hw_.hub.stop();
    // ...then pulse the slot feeder in the hub's direction.
    hw_.clock.sleep_ms(kFeederPulseDelayMs);
    hw_.feeders.run(slot, hub_dir, 255);
    hw_.clock.sleep_ms(kFeederPulseMs);
    hw_.feeders.stop(slot);
    // Restore the previous hub command.
    if (hub_speed) hw_.hub.run(hub_dir, hub_speed);
}

void MotorSupervisor::tick() {
    const uint32_t now = hw_.clock.now_ms();
    if (detoothing_ && static_cast<int32_t>(now - reenable_at_) >= 0) {
        detoothing_ = false;
    }

    const uint8_t mask = blocked_mask();
    if (mask == 0) {
        blocked_ms_ = 0;
    } else {
        blocked_ms_ += kPeriodMs;
        if (blocked_ms_ > kDetoothAfterMs && detooth_enabled && !detoothing_) {
            detoothing_ = true;
            reenable_at_ = now + kReenableMs;
            const int slot = active_slot();
            if ((mask & 0x10) && slot >= 0) detooth(slot);
        }
        if (blocked_ms_ > kStopAfterMs) {
            stopped_mask_ = mask;
            hw_.hub.stop();
            hw_.feeders.stop_all();
        }
    }

    if (hw_.hub.overload()) {
        overload_ = true;
        if (detooth_enabled && !detoothing_) {
            const int slot = active_slot();
            detoothing_ = true;
            reenable_at_ = now + kReenableMs;
            if (slot >= 0) detooth(slot);
        }
    }
}

}  // namespace cfs
