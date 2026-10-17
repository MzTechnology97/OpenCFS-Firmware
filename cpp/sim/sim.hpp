// SPDX-License-Identifier: Apache-2.0
// Deterministic host simulator of the CFS filament path. Time advances only in
// Clock::sleep_ms, one physics step per millisecond.
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>

#include "cfs/hal.hpp"

namespace cfs::sim {

struct Geometry {
    float hub_sensor_mm = 300.0f;   // inlet → hub path sensor
    float hub_engage_mm = 280.0f;   // from here the hub drives the filament
    float buffer_empty_mm = 1800.0f;
    float buffer_full_mm = 1830.0f;
    float toolhead_mm = 1900.0f;    // hard stop (extruder gears)
    float feeder_mm_s = 60.0f;      // at speed 255
    float hub_mm_s = 150.0f;        // at speed 255
    float rfid_window_mm = 15.0f;   // tag visible over this much filament
};

struct SlotModel {
    bool loaded = false;       // spool inserted (filament past the inlet)
    float tip_mm = 0.0f;       // filament tip position along the path
    float spool_circ_mm = 420.76f;  // circumference at the winding radius
    float tag_phase_mm = 100.0f;
    float unwound_mm = 0.0f;
    bool has_tag = true;
    uint8_t uid = 0x10;
    char record[40] = {};
};

class World : public Clock,
              public FeederDriver,
              public HubDriver,
              public Odometer,
              public PathSensors,
              public Environment,
              public Indicators,
              public RfidReader,
              public Settings,
              public Transport,
              public System {
public:
    World();

    Hardware hardware();

    // Clock
    uint32_t now_ms() override { return now_; }
    void sleep_ms(uint32_t ms) override;

    // FeederDriver
    void run(int slot, Dir dir, uint8_t speed) override;
    void stop(int slot) override { feeder_speed_[slot] = 0; }
    bool blocked(int slot) override { return feeder_blocked[slot]; }

    // HubDriver
    void run(Dir dir, uint8_t speed) override {
        hub_dir_ = dir;
        hub_speed_ = speed;
        if (speed) {
            motion_since_idle_check_ = true;
            ++motor_commands;
        }
    }
    void stop() override { hub_speed_ = 0; }
    bool blocked() override { return hub_blocked; }
    bool overload() override { return hub_overload; }
    Dir current_dir() override { return hub_dir_; }
    uint8_t current_speed() override { return hub_speed_; }
    // FeederDriver and HubDriver share this one override in the simulator.
    bool outputs_idle() override {
        const bool moved = motion_since_idle_check_;
        motion_since_idle_check_ = false;
        for (uint8_t s : feeder_speed_) {
            if (s) return false;
        }
        return !moved && hub_speed_ == 0;
    }
    bool motion_since_idle_check_ = false;
    uint32_t motor_commands = 0;  // feeder/hub run() calls with a non-zero speed

    // Odometer
    float position_mm() override { return odom_; }
    void reset() override { odom_ = 0.0f; }

    // PathSensors
    bool inlet_present(int slot) override { return slots[slot].loaded && slots[slot].tip_mm >= 0.0f; }
    bool hub_present(int slot) override { return slots[slot].loaded && slots[slot].tip_mm >= g.hub_sensor_mm; }
    BufferState buffer() override;

    // Environment
    int8_t temperature_c() override { return 29; }
    uint8_t humidity_pct() override { return 38; }

    // Indicators
    void set(int slot, uint8_t mode) override { indicator[slot] = mode; }

    // RfidReader
    Detect detect(int slot) override;
    bool read_uid(int slot, TagUid& uid) override;
    bool read_record(int slot, TagRecord& rec) override;
    const tags::TagReadout* last_tag(int slot) override { return tag_readout[slot]; }
    const tags::TagReadout* tag_readout[4] = {};  // what command 0x60 reports

    // Settings
    uint8_t mode() override { return mode_; }
    void set_mode(uint8_t m) override { mode_ = m; }

    // Transport
    void write(const uint8_t* data, size_t len) override {
        tx.insert(tx.end(), data, data + len);
        if (echo) inject(now_, std::vector<uint8_t>(data, data + len));  // the box hears itself
    }
    bool echo = false;
    bool read_byte(uint8_t& b) override;
    // Queue host bytes, delivered from `at_ms` on.
    void inject(uint32_t at_ms, const std::vector<uint8_t>& bytes);

    // System
    void reset_after_ms(uint32_t ms) override { reset_at = now_ + ms; }
    void serial_number(char out[19]) override { std::memcpy(out, "SIMCFS0000000000001", 19); }
    void unique_id(uint8_t out[12]) override { std::memcpy(out, uid, 12); }
    void set_aux_output(bool on) override { aux_output = on; }

    // Test controls
    void insert(int slot, float tip_mm = 50.0f);
    void remove(int slot);
    void consume(float mm_per_s) { extrude_mm_s_ = mm_per_s; }
    // Frames the box sent, split on the F7 sync (simple: valid as long as payloads avoid F7).
    std::vector<std::vector<uint8_t>> sent_frames();

    Geometry g;
    SlotModel slots[kSlotCount];
    bool hub_blocked = false;
    bool hub_overload = false;
    bool feeder_blocked[kSlotCount] = {};
    bool aux_output = false;
    uint8_t uid[12] = {0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C};
    bool jam_before_hub = false;  // filament cannot reach the hub sensor
    uint8_t indicator[kSlotCount] = {};
    std::vector<uint8_t> tx;
    uint32_t reset_at = 0;
    // Called every simulated millisecond (interrupt emulation in tests).
    std::function<void()> tick_hook;

private:
    void step_1ms();
    int engaged_slot() const;

    uint32_t now_ = 0;
    float odom_ = 0.0f;
    uint8_t feeder_speed_[kSlotCount] = {};
    Dir feeder_dir_[kSlotCount] = {};
    uint8_t hub_speed_ = 0;
    Dir hub_dir_ = Dir::Forward;
    float extrude_mm_s_ = 0.0f;
    float consumed_mm_ = 0.0f;
    uint8_t mode_ = 0;
    std::deque<std::pair<uint32_t, uint8_t>> rx_;
};

}  // namespace cfs::sim
