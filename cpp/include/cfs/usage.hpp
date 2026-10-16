// SPDX-License-Identifier: Apache-2.0
// Filament usage accounting per slot (docs/05-preload-rfid.md section 6).
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/hal.hpp"

namespace cfs {

class UsageManager {
public:
    enum class Mode : uint8_t { Idle, Printing, Retracting };

    static constexpr float kCheckpointMm = 10.0f;   // printing: account every 10 mm
    static constexpr float kIdleMaxDeltaMm = 500.0f;
    static constexpr uint32_t kRetractSettleMs = 30000;
    static constexpr uint32_t kRetractAllowanceMm = 15;

    UsageManager(Hardware& hw, Box& box) : hw_(hw), box_(box) {}

    // A spool estimate is known: start accounting from it. `record` is the
    // 40-character tag record (len field gives the spool length in meters).
    void begin(int slot, uint8_t start_pct, const char* record);
    void print_start(int slot);
    void print_end(int slot);
    void retract(int slot);
    // Spool removed from the slot: forget everything about it (0x03 reports 0xFF).
    void clear(int slot);
    // Spool ran out while printing: report 0 % until it is removed.
    void mark_empty(int slot);
    // Periodic task (stock: every pass of the preload thread).
    void tick();

    // Stock gates live accounting on the secure-element hardware; here it is a switch.
    bool live_percent = true;

    uint32_t used_mm(int slot) const { return rec_[slot].used_mm; }
    uint32_t total_mm(int slot) const { return rec_[slot].total_mm; }
    Mode mode(int slot) const { return rec_[slot].mode; }

private:
    struct Record {
        bool valid = false;
        Mode mode = Mode::Idle;
        uint8_t start_pct = 0xFF;
        uint32_t used_mm = 0;
        uint32_t total_mm = 0;
        float last_pos = 0.0f;
        uint32_t since_ms = 0;
    };

    // Fed length, growing toward the toolhead (the odometer itself decreases).
    float position() { return -hw_.odometer.position_mm(); }
    void update_percent(int slot);

    Hardware& hw_;
    Box& box_;
    Record rec_[kSlotCount];
    int active_ = -1;
};

}  // namespace cfs
