// SPDX-License-Identifier: Apache-2.0
// Shared box state: operating state, error, current slot, pending slot events.
#pragma once

#include <cstdint>

#include "cfs/protocol.hpp"

namespace cfs {

struct SlotRfid {
    bool has_record = false;
    char record[40] = {};
    uint8_t remaining_pct = 0xFF;  // area estimate, 0xFF = unknown
};

class Box {
public:
    BoxState state() const { return state_; }
    uint8_t error() const { return error_; }
    int current_slot() const { return current_slot_; }
    void select_slot(int s) { current_slot_ = s; }

    // Motion that is running right now, independent of the reported state.
    // The stock firmware can report IDLE while filament moves; this flag does not.
    bool motion_active() const { return motion_depth_ > 0; }

    void enter(BoxState s) { state_ = s; }
    void set_idle() { state_ = BoxState::Idle; }
    // Stock rule: INVALID (1) and BUSY (3) never latch an error.
    void fail(Status s) {
        if (s == Status::InvalidParam || s == Status::Busy) {
            return;
        }
        error_ = to_u8(s);
        state_ = BoxState::Error;
    }
    void finish(Status s) {
        if (s == Status::Ok) {
            set_idle();
        } else {
            fail(s);
        }
    }
    void clear_error() {
        error_ = 0;
        if (state_ == BoxState::Error) state_ = BoxState::Idle;
    }

    // Slot events reported through BOX_STATE status 0x30.
    void post_event(int slot, SlotEvent e) {
        events_[slot] = e;
        events_pending_ = true;
    }
    bool take_events(uint8_t out[kSlotCount]) {
        if (!events_pending_) return false;
        for (int i = 0; i < kSlotCount; ++i) {
            out[i] = static_cast<uint8_t>(events_[i]);
            events_[i] = SlotEvent::None;
        }
        events_pending_ = false;
        return true;
    }

    void begin_motion() { ++motion_depth_; }
    void end_motion() {
        if (motion_depth_) --motion_depth_;
    }

    // Preload (pretension + RFID read on insertion), command 0x0D sub 0/1.
    bool read_on_insert = true;
    uint8_t one_shot_event = 0;  // BOX_STATE byte 2 (0x80 | code when fresh)
    uint8_t box_index = 0;       // set through 0xEB/0x51 (front display)
    uint8_t option_0f = 0;
    uint8_t display_values[2] = {};
    // Module init results reported by 0x15, in stock order:
    // EEPROM, temperature/humidity sensor, odometer, buffer, RFID reader 0, RFID reader 1.
    bool module_ok[6] = {true, true, true, true, true, true};
    SlotRfid rfid[kSlotCount];

private:
    BoxState state_ = BoxState::Idle;
    uint8_t error_ = 0;
    int current_slot_ = 0;
    SlotEvent events_[kSlotCount] = {};
    bool events_pending_ = false;
    int motion_depth_ = 0;
};

// RAII marker for code that drives motors.
class MotionScope {
public:
    explicit MotionScope(Box& b) : box_(b) { box_.begin_motion(); }
    ~MotionScope() { box_.end_motion(); }
    MotionScope(const MotionScope&) = delete;
    MotionScope& operator=(const MotionScope&) = delete;

private:
    Box& box_;
};

}  // namespace cfs
