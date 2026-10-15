// SPDX-License-Identifier: Apache-2.0
#include "cfs/usage.hpp"

namespace cfs {

namespace {

// Tag `len` field: 4 ASCII digits at offset 24, in meters ("0330" = 330 m).
uint32_t record_length_mm(const char* record) {
    uint32_t meters = 0;
    for (int i = 24; i < 28; ++i) {
        const char c = record[i];
        if (c < '0' || c > '9') return 0;
        meters = meters * 10 + static_cast<uint32_t>(c - '0');
    }
    return meters * 1000;
}

}  // namespace

void UsageManager::begin(int slot, uint8_t start_pct, const char* record) {
    Record& r = rec_[slot];
    r = Record{};
    r.valid = start_pct <= 100;
    r.start_pct = start_pct;
    r.total_mm = record ? record_length_mm(record) : 0;
    r.last_pos = position();
}

void UsageManager::print_start(int slot) {
    Record& r = rec_[slot];
    r.last_pos = position();
    r.mode = Mode::Printing;
    active_ = slot;
}

void UsageManager::print_end(int slot) {
    Record& r = rec_[slot];
    const float now = position();
    const float delta = now - r.last_pos;
    r.last_pos = now;
    r.mode = Mode::Idle;
    if (delta > 0.0f && delta < kIdleMaxDeltaMm) {
        r.used_mm += static_cast<uint32_t>(delta);
        update_percent(slot);
    }
    active_ = slot;
}

void UsageManager::retract(int slot) {
    Record& r = rec_[slot];
    r.last_pos = position();
    r.mode = Mode::Retracting;
    r.used_mm += kRetractAllowanceMm;
    r.since_ms = hw_.clock.now_ms();
    active_ = slot;
}

void UsageManager::update_percent(int slot) {
    Record& r = rec_[slot];
    if (!live_percent || !r.valid || r.total_mm == 0) return;
    const uint32_t used_pct = (r.used_mm * 100u) / r.total_mm;
    uint8_t pct = used_pct < r.start_pct ? static_cast<uint8_t>(r.start_pct - used_pct) : 0;
    if (pct == 0) pct = 1;  // stock never reports 0 for a tracked spool
    box_.rfid[slot].remaining_pct = pct;
}

void UsageManager::tick() {
    if (active_ < 0) return;
    Record& r = rec_[active_];
    switch (r.mode) {
    case Mode::Idle:
        return;
    case Mode::Retracting:
        if (hw_.clock.now_ms() - r.since_ms > kRetractSettleMs) print_end(active_);
        return;
    case Mode::Printing: {
        const float delta = position() - r.last_pos;
        if (delta < kCheckpointMm) return;
        const uint32_t whole = static_cast<uint32_t>(delta);
        r.last_pos += static_cast<float>(whole);
        r.used_mm += whole;
        update_percent(active_);
        return;
    }
    }
}

}  // namespace cfs
