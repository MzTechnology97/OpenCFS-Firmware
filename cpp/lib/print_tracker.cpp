// SPDX-License-Identifier: Apache-2.0
#include "cfs/print_tracker.hpp"

namespace cfs {

Status PrintTracker::start(int slot) {
    if (box_.state() == BoxState::Preload || box_.state() == BoxState::Reload) {
        return Status::Busy;
    }
    if (slot < 0 || slot >= kSlotCount) {
        return Status::InvalidParam;
    }
    box_.select_slot(slot);
    box_.enter(BoxState::Print);
    armed_ = true;
    retriggers_ = 0;
    if (hw_.sensors.buffer() == BufferState::Empty) {
        on_buffer_empty();
    } else if (hw_.sensors.buffer() == BufferState::Full) {
        hw_.hub.stop();
    }
    return Status::Ok;
}

void PrintTracker::stop() {
    armed_ = false;
    session_ = false;
    retriggers_ = 0;
    hw_.hub.stop();
    if (box_.state() == BoxState::Print) {
        box_.set_idle();
    }
}

void PrintTracker::on_buffer_empty() {
    if (!armed_) return;
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_print_refill);
    session_ = true;
    retrigger_ = true;
    ++retriggers_;
}

void PrintTracker::on_buffer_full() {
    if (!armed_) return;
    session_ = false;
    retriggers_ = 0;
    hw_.hub.stop();
}

void PrintTracker::service() {
    if (!session_) {
        return;
    }
    MotionScope m(box_);
    const Timing& t = cfg_.timing;
    const int slot = box_.current_slot();
    const uint32_t tick = 100;
    int32_t left = static_cast<int32_t>(t.refill_window_ms / tick);
    float first = hw_.odometer.position_mm();
    float now = first;
    retrigger_ = false;
    enum { Waiting, Assisting, Assisted } phase = Waiting;
    uint32_t assist_ticks = 0;

    while (session_) {
        --left;
        if (retrigger_) {  // a new empty edge restarts the measurement
            retrigger_ = false;
            first = now = hw_.odometer.position_mm();
            left += 2;  // stock: the countdown grows by one tick per retrigger
        }
        if (phase == Waiting && left < static_cast<int32_t>((t.refill_window_ms - t.refill_assist_after_ms) / tick)) {
            phase = Assisting;
            hw_.feeders.run(slot, Dir::Forward, cfg_.speeds.feeder_forward);
            hw_.hub.run(Dir::Forward, cfg_.speeds.hub_print_assist);
        }
        if (left <= 0) {
            const float moved = first - now;
            if (moved < t.refill_min_mm) {
                box_.fail(Status::BufferRefillNoMotion);
                armed_ = false;
            } else if (retriggers_ > t.refill_max_retriggers || moved >= t.refill_max_mm) {
                box_.fail(Status::BufferRefillStalled);
                retriggers_ = 0;
                armed_ = false;
            }
            hw_.hub.stop();
            break;
        }
        if (phase == Assisting && ++assist_ticks >= t.refill_assist_ms / tick) {
            phase = Assisted;
            hw_.feeders.stop(slot);
        }
        hw_.clock.sleep_ms(tick);
        now = hw_.odometer.position_mm();
    }
    if (phase == Assisting) {
        hw_.feeders.stop(slot);
    }
    session_ = false;
}

Status PrintTracker::runout_check() {
    if (!hw_.sensors.inlet_present(box_.current_slot())) {
        return Status::Runout;
    }
    return Status::Ok;
}

}  // namespace cfs
