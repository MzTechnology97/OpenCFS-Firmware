// SPDX-License-Identifier: Apache-2.0
#include "cfs/feeder.hpp"

namespace cfs {

void Feeder::stop_all() {
    hw_.hub.stop();
    hw_.feeders.stop_all();
}

bool Feeder::wait_hub_sensor(int slot, bool present, uint32_t timeout_ms, int samples) {
    int left = samples;
    for (uint32_t t = 0; t < timeout_ms; t += 10) {
        left = (hw_.sensors.hub_present(slot) == present) ? left - 1 : samples;
        if (left == 0) {
            return true;
        }
        sleep(10);
    }
    return false;
}

bool Feeder::wait_buffer(BufferState want, uint32_t timeout_ms) {
    int left = 10;
    for (uint32_t t = 0; t < timeout_ms; ++t) {
        left = (hw_.sensors.buffer() == want) ? left - 1 : 10;
        if (left == 0) {
            return true;
        }
        sleep(1);
    }
    return false;
}

bool Feeder::inlet_confirmed(int slot) {
    uint32_t run = 0;
    for (uint32_t i = 0; i < cfg_.timing.inlet_check_attempts; ++i) {
        sleep(1);
        run = hw_.sensors.inlet_present(slot) ? run + 1 : 0;
        if (run >= cfg_.timing.inlet_check_samples) {
            return true;
        }
    }
    return false;
}

Status Feeder::clear_other_slots(int slot) {
    for (int s = 0; s < kSlotCount; ++s) {
        if (s != slot && hw_.sensors.hub_present(s)) {
            Status st = retract_slot(s);
            if (st != Status::Ok) {
                box_.select_slot(s);
                return st;
            }
        }
    }
    return Status::Ok;
}

Status Feeder::precheck(int slot) {
    if (box_.state() != BoxState::Idle) {
        return Status::Busy;
    }
    box_.enter(BoxState::Reload);
    if (!inlet_confirmed(slot)) {
        return Status::SlotEmpty;
    }
    return clear_other_slots(slot);
}

Status Feeder::feed_until_hub_sensor(int slot) {
    MotionScope m(box_);
    hw_.feeders.run(slot, Dir::Forward, cfg_.speeds.feeder_forward);
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_insert);
    if (wait_hub_sensor(slot, true, cfg_.timing.hub_sensor_timeout_ms, 10)) {
        return Status::Ok;
    }
    stop_all();
    return Status::Stage0SensorTimeout;
}

Status Feeder::push_through_hub(int slot) {
    MotionScope m(box_);
    const float start = odom();
    for (uint32_t t = 0; t < cfg_.timing.hub_outside_timeout_ms; t += cfg_.timing.sample_ms) {
        if (start - odom() >= cfg_.timing.hub_outside_mm) {
            hw_.hub.stop();
            hw_.feeders.stop(slot);
            return Status::Ok;
        }
        sleep(cfg_.timing.sample_ms);
    }
    stop_all();
    return Status::Stage0OdometerTimeout;
}

void Feeder::transition_push() {
    MotionScope m(box_);
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_transition);
    sleep(cfg_.timing.hub_transition_wait_ms);
    hw_.hub.stop();
}

Status Feeder::stage0_slot_to_hub(int slot) {
    Status st = precheck(slot);
    if (st == Status::Ok) st = feed_until_hub_sensor(slot);
    if (st == Status::Ok) st = push_through_hub(slot);
    if (st == Status::Ok) transition_push();
    box_.finish(st);
    return st;
}

Status Feeder::stage1_hub_until_stop() {
    MotionScope m(box_);
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_transition);
    const float start = odom();
    float last = start;
    uint32_t still = 0;
    for (uint32_t i = 0; i < cfg_.timing.feed_total_samples; ++i) {
        sleep(cfg_.timing.sample_ms);
        const float now = odom();
        if (start - now > cfg_.timing.overtravel_mm) {
            hw_.hub.stop();
            return Status::Overtravel;
        }
        still = (last - now < cfg_.timing.min_move_mm) ? still + 1 : 0;
        last = now;
        if (still >= cfg_.timing.stall_samples) {
            break;  // the filament reached a stop
        }
    }
    hw_.hub.stop();
    return Status::Ok;
}

Status Feeder::stage2_hub_start_checked() {
    {
        MotionScope m(box_);
        hw_.hub.run(Dir::Forward, cfg_.speeds.hub_forward);
        const float start = odom();
        bool moved = false;
        for (uint32_t t = 0; t < cfg_.timing.recovery_timeout_ms && !moved; t += 1000) {
            sleep(1000);
            moved = start - odom() >= 1.0f;
        }
        if (!moved) {
            hw_.hub.stop();
            return Status::FeedTimeout;
        }
    }
    return stage1_hub_until_stop();
}

Status Feeder::stage4_feed_to_buffer(Stage4Link& link) {
    MotionScope m(box_);
    box_.enter(BoxState::Reload);
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_transition);
    const float start = odom();
    float last = start;
    uint32_t still = 0;
    for (uint32_t i = 0; i < cfg_.timing.feed_total_samples; ++i) {
        sleep(cfg_.timing.sample_ms);
        const float now = odom();
        switch (link.poll()) {
        case Stage4Link::Poll::Progress:
            link.reply_progress(now);
            break;
        case Stage4Link::Poll::Malformed:
            link.reply_malformed();
            break;
        case Stage4Link::Poll::Stop: {
            const Status st = wait_buffer(BufferState::Full, cfg_.timing.buffer_full_timeout_ms)
                                  ? Status::Ok
                                  : Status::BufferNotFull;
            hw_.hub.stop();
            box_.finish(st);
            link.reply_stop(st);
            return Status::Ok;
        }
        case Stage4Link::Poll::None:
            break;
        }
        if (start - now > cfg_.timing.overtravel_mm) {
            hw_.hub.stop();
            return Status::Overtravel;
        }
        still = (last - now < cfg_.timing.min_move_mm) ? still + 1 : 0;
        last = now;
        if (still >= cfg_.timing.stall_samples) {
            hw_.hub.stop();
            return Status::OdometerStalled;
        }
    }
    hw_.hub.stop();
    return Status::FeedTimeout;
}

Status Feeder::stage6_buffer_full() {
    if (wait_buffer(BufferState::Full, 15)) {
        return Status::Ok;
    }
    MotionScope m(box_);
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_forward);
    const bool full = wait_buffer(BufferState::Full, cfg_.timing.buffer_full_timeout_ms);
    hw_.hub.stop();
    return full ? Status::Ok : Status::BufferNotFull;
}

Status Feeder::stage7_extruder_push(float mm) {
    MotionScope m(box_);
    const float start = odom();
    hw_.hub.run(Dir::Forward, cfg_.speeds.hub_extruder_push);
    for (uint32_t i = 0; i < cfg_.timing.stage7_samples; ++i) {
        sleep(cfg_.timing.sample_ms);
        if (start - odom() >= mm) {
            hw_.hub.stop();
            return Status::Ok;
        }
    }
    hw_.hub.stop();
    return Status::Stage7NoMotion;
}

Status Feeder::empty_buffer(int slot) {
    MotionScope m(box_);
    hw_.feeders.run(slot, Dir::Reverse, cfg_.speeds.feeder_reverse);
    hw_.hub.run(Dir::Reverse, cfg_.speeds.hub_reverse);
    const bool empty = wait_buffer(BufferState::Empty, cfg_.timing.unload_buffer_timeout_ms);
    hw_.hub.stop();
    hw_.feeders.stop(slot);
    return empty ? Status::Ok : Status::UnloadBufferTimeout;
}

Status Feeder::pull_back_through_hub(int slot) {
    MotionScope m(box_);
    hw_.hub.run(Dir::Reverse, cfg_.speeds.hub_reverse);
    hw_.feeders.run(slot, Dir::Reverse, cfg_.speeds.feeder_reverse);
    float last = odom();
    uint32_t clear = 0;
    uint32_t still = 0;
    for (uint32_t i = 0; i < cfg_.timing.pull_back_samples; ++i) {
        clear = hw_.sensors.hub_present(slot) ? 0 : clear + 1;
        if (clear >= 5) {
            return Status::Ok;
        }
        const float now = odom();
        still = (now - last < cfg_.timing.min_move_mm) ? still + 1 : 0;
        last = now;
        if (still && hw_.hub.blocked()) {
            stop_all();
            return Status::UnloadMotorBlocked;
        }
        sleep(cfg_.timing.sample_ms);
    }
    stop_all();
    return Status::UnloadOdometerTimeout;
}

Status Feeder::clear_hub_sensor(int slot) {
    MotionScope m(box_);
    if (hw_.sensors.hub_present(slot) &&
        !wait_hub_sensor(slot, false, cfg_.timing.hub_clear_timeout_ms, 10)) {
        stop_all();
        return Status::UnloadHubPeTimeout;
    }
    sleep(300);
    hw_.hub.stop();
    hw_.feeders.stop(slot);
    return Status::Ok;
}

Status Feeder::retract_slot(int slot) {
    if (!hw_.sensors.inlet_present(slot)) {
        return Status::UnloadInletClear;
    }
    Status st = pull_back_through_hub(slot);
    if (st == Status::Ok) {
        st = clear_hub_sensor(slot);
    }
    return st;
}

Status Feeder::unload(int slot, uint8_t phase) {
    if (box_.state() != BoxState::Idle && box_.state() != BoxState::Preload) {
        return Status::Busy;
    }
    bool any = false;
    for (int s = 0; s < kSlotCount; ++s) {
        any = any || hw_.sensors.inlet_present(s);
    }
    if (slot >= 0 && !hw_.sensors.inlet_present(slot)) {
        box_.fail(Status::UnloadNoFilament);
        return Status::UnloadNoFilament;
    }
    if (slot < 0 && !any) {
        box_.fail(Status::UnloadAllEmpty);
        return Status::UnloadAllEmpty;
    }
    if (phase > 1) {
        return Status::InvalidParam;
    }
    box_.enter(BoxState::Reload);
    Status st = Status::Ok;
    for (int s = 0; s < kSlotCount && st == Status::Ok; ++s) {
        if (slot >= 0 ? s != slot : !hw_.sensors.hub_present(s)) {
            continue;
        }
        st = phase == 0 ? empty_buffer(s) : retract_slot(s);
    }
    box_.finish(st);
    return st;
}

Status Feeder::feed_to_hub_and_settle(int slot) {
    Status st = precheck(slot);
    if (st == Status::Ok) st = feed_until_hub_sensor(slot);
    if (st == Status::Ok) st = push_through_hub(slot);
    if (st == Status::Ok) st = stage1_hub_until_stop();
    box_.finish(st);
    return st;
}

}  // namespace cfs
