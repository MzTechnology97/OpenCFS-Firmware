// SPDX-License-Identifier: Apache-2.0
#include "sim.hpp"

#include <cmath>

namespace cfs::sim {

World::World() {
    for (int s = 0; s < kSlotCount; ++s) {
        slots[s].uid = static_cast<uint8_t>(0x10 + s);
        // date, supplier, batch, material, color, length (m), serial, reserve
        std::memcpy(slots[s].record, "AB124" "0276" "A2" "101001" "0FFFFFF" "0330" "000001" "000000", 40);
    }
}

Hardware World::hardware() {
    return Hardware{*this, *this, *this, *this, *this, *this, *this, *this, *this, *this, *this};
}

void World::sleep_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; ++i) {
        step_1ms();
    }
}

void World::run(int slot, Dir dir, uint8_t speed) {
    feeder_speed_[slot] = speed;
    feeder_dir_[slot] = dir;
    if (speed) {
        motion_since_idle_check_ = true;
        ++motor_commands;
    }
}

int World::engaged_slot() const {
    for (int s = 0; s < kSlotCount; ++s) {
        if (slots[s].loaded && slots[s].tip_mm >= g.hub_engage_mm) return s;
    }
    return -1;
}

BufferState World::buffer() {
    const int s = engaged_slot();
    const float fill = s < 0 ? 0.0f : slots[s].tip_mm - consumed_mm_;
    if (fill >= g.buffer_full_mm) return BufferState::Full;
    if (fill < g.buffer_empty_mm) return BufferState::Empty;
    return BufferState::Between;
}

void World::step_1ms() {
    ++now_;
    const int hub_slot = engaged_slot();
    if (extrude_mm_s_ > 0.0f && hub_slot >= 0 &&
        slots[hub_slot].tip_mm - consumed_mm_ > g.buffer_empty_mm - 50.0f) {
        consumed_mm_ += extrude_mm_s_ / 1000.0f;  // the extruder pulls slack out of the buffer
    }
    for (int s = 0; s < kSlotCount; ++s) {
        SlotModel& m = slots[s];
        if (!m.loaded) continue;
        float v = 0.0f;
        const bool engaged = s == hub_slot;
        if (engaged && hub_blocked) {
            v = 0.0f;  // a blocked hub holds the filament
        } else if (engaged && hub_speed_) {
            v = (hub_dir_ == Dir::Forward ? 1.0f : -1.0f) * g.hub_mm_s * hub_speed_ / 255.0f;
        } else if (feeder_speed_[s]) {
            v = (feeder_dir_[s] == Dir::Forward ? 1.0f : -1.0f) * g.feeder_mm_s * feeder_speed_[s] / 255.0f;
        }
        float next = m.tip_mm + v / 1000.0f;
        if (jam_before_hub && next > g.hub_sensor_mm - 5.0f) next = g.hub_sensor_mm - 5.0f;
        const float stop = g.toolhead_mm + (engaged ? consumed_mm_ : 0.0f);
        if (next > stop) next = stop;
        if (next < 1.0f) next = 1.0f;  // the spool keeps the tip at the inlet
        const float dp = next - m.tip_mm;
        if (next >= g.hub_engage_mm || m.tip_mm >= g.hub_engage_mm) odom_ -= dp;
        m.unwound_mm += dp;
        m.tip_mm = next;
    }
    if (tick_hook) tick_hook();
}

RfidReader::Detect World::detect(int slot) {
    const SlotModel& m = slots[slot];
    if (!m.loaded || !m.has_tag) return Detect::None;
    float a = std::fmod(m.unwound_mm + m.tag_phase_mm, m.spool_circ_mm);
    if (a < 0) a += m.spool_circ_mm;
    return a < g.rfid_window_mm ? Detect::One : Detect::None;
}

bool World::read_uid(int slot, TagUid& uid) {
    if (detect(slot) != Detect::One) return false;
    uid.len = 4;
    uid.bytes[0] = slots[slot].uid;
    return true;
}

bool World::read_record(int slot, TagRecord& rec) {
    if (detect(slot) != Detect::One) return false;
    std::memcpy(rec.text, slots[slot].record, 40);
    return true;
}

bool World::read_byte(uint8_t& b) {
    if (rx_.empty() || rx_.front().first > now_) return false;
    b = rx_.front().second;
    rx_.pop_front();
    return true;
}

void World::inject(uint32_t at_ms, const std::vector<uint8_t>& bytes) {
    for (uint8_t b : bytes) rx_.emplace_back(at_ms, b);
}

void World::insert(int slot, float tip_mm) {
    slots[slot].loaded = true;
    slots[slot].tip_mm = tip_mm;
}

void World::remove(int slot) {
    slots[slot].loaded = false;
    slots[slot].tip_mm = 0.0f;
}

std::vector<std::vector<uint8_t>> World::sent_frames() {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    while (i + 2 < tx.size()) {
        if (tx[i] != 0xF7) {
            ++i;
            continue;
        }
        const size_t n = static_cast<size_t>(tx[i + 2]) + 3;
        if (i + n > tx.size()) break;
        out.emplace_back(tx.begin() + static_cast<long>(i), tx.begin() + static_cast<long>(i + n));
        i += n;
    }
    return out;
}

}  // namespace cfs::sim
