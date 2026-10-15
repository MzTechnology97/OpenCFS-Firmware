// SPDX-License-Identifier: Apache-2.0
// Load / unload / feed operations of the filament path (docs/04-feeder.md).
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/hal.hpp"

namespace cfs {

// Lets the stage-4 feed loop talk to the host while it owns the RS-485 thread.
class Stage4Link {
public:
    enum class Poll : uint8_t { None, Progress, Stop, Malformed };
    virtual ~Stage4Link() = default;
    virtual Poll poll() = 0;
    virtual void reply_progress(float odometer_mm) = 0;
    virtual void reply_malformed() = 0;
    // Reply to the stage-6 frame that ended the loop.
    virtual void reply_stop(Status s) = 0;
};

class Feeder {
public:
    Feeder(Hardware& hw, Box& box, const Config& cfg) : hw_(hw), box_(box), cfg_(cfg) {}

    // LOAD stages (command 0x10).
    Status stage0_slot_to_hub(int slot);
    Status stage1_hub_until_stop();
    Status stage2_hub_start_checked();
    // Returns Ok when the host ended the loop with stage 6 (already answered).
    Status stage4_feed_to_buffer(Stage4Link& link);
    Status stage6_buffer_full();
    Status stage7_extruder_push(float mm);

    // UNLOAD (command 0x11). slot = 0..3, or -1 for every slot loaded in the hub.
    Status unload(int slot, uint8_t phase);

    // Command 0x13: precheck, slot to hub, then hub feed until the filament stops.
    Status feed_to_hub_and_settle(int slot);

    // Building blocks, public for the preload/RFID logic.
    Status precheck(int slot);
    Status feed_until_hub_sensor(int slot);
    Status push_through_hub(int slot);
    Status retract_slot(int slot);
    Status empty_buffer(int slot);
    void stop_all();

private:
    float odom() { return hw_.odometer.position_mm(); }
    void sleep(uint32_t ms) { hw_.clock.sleep_ms(ms); }
    bool wait_hub_sensor(int slot, bool present, uint32_t timeout_ms, int samples);
    bool wait_buffer(BufferState want, uint32_t timeout_ms);
    bool inlet_confirmed(int slot);
    Status clear_other_slots(int slot);
    Status pull_back_through_hub(int slot);
    Status clear_hub_sensor(int slot);
    void transition_push();

    Hardware& hw_;
    Box& box_;
    const Config& cfg_;
};

}  // namespace cfs
