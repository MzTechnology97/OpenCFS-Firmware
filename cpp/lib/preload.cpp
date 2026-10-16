// SPDX-License-Identifier: Apache-2.0
#include "cfs/preload.hpp"

namespace cfs {

// ---------------------------------------------------------------- RfidScanner

void RfidScanner::nudge_neighbor_away(int slot) {
    const int n = neighbor(slot);
    if (!hw_.sensors.inlet_present(n)) {
        return;
    }
    MotionScope m(box_);
    hw_.feeders.run(n, Dir::Reverse, cfg_.speeds.feeder_reverse);
    hw_.clock.sleep_ms(cfg_.timing.rfid_neighbor_move_ms);
    hw_.feeders.stop(n);
    hw_.clock.sleep_ms(cfg_.timing.rfid_neighbor_settle_ms);
}

bool RfidScanner::locate(int slot) {
    for (uint32_t attempt = 0; attempt < cfg_.timing.rfid_locate_attempts; ++attempt) {
        const auto d = hw_.rfid.detect(slot);
        if (d == RfidReader::Detect::One) {
            return true;
        }
        if (d == RfidReader::Detect::Multiple) {
            nudge_neighbor_away(slot);
            continue;
        }
        // No tag in the field: rotate the spool a little with the hub.
        MotionScope m(box_);
        hw_.hub.run(Dir::Forward, cfg_.speeds.rfid_seek);
        hw_.clock.sleep_ms(cfg_.timing.rfid_seek_move_ms);
        hw_.hub.stop();
        hw_.clock.sleep_ms(cfg_.timing.rfid_seek_settle_ms);
    }
    return hw_.rfid.detect(slot) == RfidReader::Detect::One;
}

bool RfidScanner::measure(int slot, float& per_rev_mm) {
    MotionScope m(box_);
    hw_.hub.run(Dir::Forward, cfg_.speeds.rfid_measure);
    const uint32_t t0 = hw_.clock.now_ms();
    const float origin = hw_.odometer.position_mm();
    bool in_field = hw_.rfid.detect(slot) == RfidReader::Detect::One;
    uint32_t field_since = t0;
    int passes = 0;
    float first = 0.0f;
    TagUid uid_first;
    while (hw_.clock.now_ms() - t0 < 20000) {
        const bool seen = hw_.rfid.detect(slot) == RfidReader::Detect::One;
        const uint32_t now = hw_.clock.now_ms();
        if (seen && !in_field) {
            field_since = now;
            TagUid uid;
            hw_.rfid.read_uid(slot, uid);
            if (passes == 0) {
                uid_first = uid;
                first = hw_.odometer.position_mm();
                passes = 1;
            } else if (uid == uid_first) {
                per_rev_mm = first - hw_.odometer.position_mm();
                if (per_rev_mm < 0) per_rev_mm = -per_rev_mm;
                hw_.hub.stop();
                return true;
            } else {
                break;  // a different tag: neighbor spool
            }
        }
        if (seen && in_field && now - field_since > 1000) {
            break;  // tag stuck in the field
        }
        in_field = seen;
        const float travel = origin - hw_.odometer.position_mm();
        if (travel > 1250.0f || travel < -1250.0f) {
            break;
        }
        hw_.clock.sleep_ms(5);
    }
    hw_.hub.stop();
    return false;
}

bool RfidScanner::read_record(int slot) {
    hw_.clock.sleep_ms(500);
    for (int attempt = 0; attempt < 3; ++attempt) {
        TagRecord rec;
        if (hw_.rfid.read_record(slot, rec)) {
            for (size_t i = 0; i < kTagRecordLen; ++i) {
                box_.rfid[slot].record[i] = rec.text[i];
            }
            box_.rfid[slot].has_record = true;
            return true;
        }
        MotionScope m(box_);
        hw_.feeders.run(slot, Dir::Reverse, cfg_.speeds.feeder_reverse);
        hw_.clock.sleep_ms(cfg_.timing.rfid_retry_reverse_ms);
        hw_.feeders.stop(slot);
        hw_.hub.run(Dir::Forward, cfg_.speeds.rfid_reposition);
        for (int t = 0; t < 3000 && hw_.rfid.detect(slot) != RfidReader::Detect::One; ++t) {
            hw_.clock.sleep_ms(1);
        }
        hw_.hub.stop();
    }
    return false;
}

RfidScanner::Result RfidScanner::scan(int slot) {
    Result r;
    box_.rfid[slot] = SlotRfid{};
    // The hub can only rotate the spool once the filament is engaged in it. A tag
    // that is not found yet is fine: the measurement below rotates the spool anyway.
    if (!locate(slot)) {
        r.status = feeder_.feed_until_hub_sensor(slot);
        if (r.status == Status::Ok) r.status = feeder_.push_through_hub(slot);
        if (r.status != Status::Ok) {
            feeder_.stop_all();
            if (hw_.sensors.hub_present(slot)) feeder_.retract_slot(slot);
            return r;
        }
        locate(slot);
    }
    const bool measured = measure(slot, r.per_rev_mm);
    r.record_read = read_record(slot);
    feeder_.stop_all();
    if (hw_.sensors.hub_present(slot)) {
        feeder_.retract_slot(slot);
    }
    if (measured) {
        r.estimate = estimate_remaining(r.per_rev_mm, cfg_.spool);
        box_.rfid[slot].remaining_pct = r.estimate.area_pct;
        if (usage_) {
            usage_->begin(slot, r.estimate.area_pct,
                          box_.rfid[slot].has_record ? box_.rfid[slot].record : nullptr);
        }
    }
    return r;
}

// -------------------------------------------------------------------- Preload

void Preload::init() {
    for (int s = 0; s < kSlotCount; ++s) {
        inlet_[s] = DebounceFilter(6, hw_.sensors.inlet_present(s));
        hub_[s] = DebounceFilter(128, hw_.sensors.hub_present(s));
        present_[s] = inlet_[s].level();
    }
}

bool Preload::allowed() const {
    return box_.state() == BoxState::Idle || box_.state() == BoxState::Preload;
}

void Preload::enqueue(int slot) {
    for (auto& q : queue_) {
        if (q == slot) return;
        if (q < 0) {
            q = static_cast<int8_t>(slot);
            return;
        }
    }
}

void Preload::dequeue(int slot) {
    int w = 0;
    for (int r = 0; r < kSlotCount; ++r) {
        if (queue_[r] >= 0 && queue_[r] != slot) queue_[w++] = queue_[r];
    }
    while (w < kSlotCount) queue_[w++] = -1;
}

int Preload::pop() {
    const int s = queue_[0];
    if (s >= 0) dequeue(s);
    return s;
}

bool Preload::any_hub_loaded() const {
    for (int s = 0; s < kSlotCount; ++s) {
        if (hw_.sensors.hub_present(s)) return true;
    }
    return false;
}

void Preload::sample() {
    for (int s = 0; s < kSlotCount; ++s) {
        const bool now = inlet_[s].update(hw_.sensors.inlet_present(s));
        hub_[s].update(hw_.sensors.hub_present(s));
        if (now == present_[s]) continue;
        present_[s] = now;
        if (box_.motion_active()) continue;  // our own motion moved the filament
        if (now) {
            box_.post_event(s, SlotEvent::Inserted);
            if (allowed()) enqueue(s);
        } else {
            box_.post_event(s, SlotEvent::Removed);
            dequeue(s);
            if (usage_) usage_->clear(s);
        }
    }
}

void Preload::pretension(int slot) {
    MotionScope m(box_);
    hw_.feeders.run(slot, Dir::Forward, cfg_.speeds.feeder_forward);
    for (int t = 0; t < 10000 && !hw_.sensors.hub_present(slot); ++t) hw_.clock.sleep_ms(1);
    hw_.feeders.stop(slot);
    hw_.feeders.run(slot, Dir::Reverse, cfg_.speeds.feeder_reverse);
    for (int t = 0; t < 10000 && hw_.sensors.hub_present(slot); ++t) hw_.clock.sleep_ms(1);
    hw_.feeders.stop(slot);
}

void Preload::finish(int slot) {
    box_.post_event(slot, hw_.sensors.inlet_present(slot) ? SlotEvent::Redetected : SlotEvent::Removed);
    if (box_.state() == BoxState::Preload) box_.set_idle();
    state_ = State::Wait;
    slot_ = -1;
    hw_.clock.sleep_ms(300);
}

void Preload::step() {
    switch (state_) {
    case State::Wait: {
        if (!allowed()) {
            for (auto& q : queue_) q = -1;
            return;
        }
        if (queue_[0] < 0 || any_hub_loaded()) return;
        slot_ = pop();
        state_ = State::Pretension;
        return;
    }
    case State::Pretension:
        // Pretension always runs; the RFID part only when preload is enabled.
        if (box_.read_on_insert) box_.enter(BoxState::Preload);
        pretension(slot_);
        if (!box_.read_on_insert) {
            finish(slot_);
            return;
        }
        state_ = State::CheckHub;
        return;
    case State::CheckHub:
        if (any_hub_loaded()) {
            finish(slot_);
            return;
        }
        state_ = State::Read;
        return;
    case State::Read:
        if (hw_.sensors.inlet_present(slot_)) {
            scanner_.scan(slot_);
        }
        finish(slot_);
        return;
    }
}

}  // namespace cfs
