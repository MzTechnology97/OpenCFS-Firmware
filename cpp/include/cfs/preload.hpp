// SPDX-License-Identifier: Apache-2.0
// Insertion detection, pretension and RFID margin estimation
// (docs/05-preload-rfid.md).
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/feeder.hpp"
#include "cfs/hal.hpp"
#include "cfs/spool.hpp"
#include "cfs/usage.hpp"

namespace cfs {

// Level filter: the output follows the input after `samples` consecutive
// differing reads.
class DebounceFilter {
public:
    explicit DebounceFilter(uint8_t samples = 6, bool initial = false)
        : samples_(samples), level_(initial) {}
    bool update(bool raw) {
        if (raw == level_) {
            count_ = 0;
        } else if (++count_ >= samples_) {
            level_ = raw;
            count_ = 0;
        }
        return level_;
    }
    bool level() const { return level_; }

private:
    uint8_t samples_;
    uint8_t count_ = 0;
    bool level_;
};

class RfidScanner {
public:
    struct Result {
        Status status = Status::Ok;
        bool record_read = false;
        float per_rev_mm = -1.0f;
        SpoolEstimate estimate;
    };

    RfidScanner(Hardware& hw, Box& box, Feeder& feeder, const Config& cfg)
        : hw_(hw), box_(box), feeder_(feeder), cfg_(cfg) {}

    // Locates the slot's tag, measures one spool revolution, reads the record,
    // parks the filament and stores the estimate in box.rfid[slot].
    Result scan(int slot);

    static int neighbor(int slot) { return slot ^ 1; }
    void set_usage(UsageManager* u) { usage_ = u; }

private:
    void clear_neighbor(int slot);
    bool locate(int slot);
    bool measure(int slot, float& per_rev_mm);
    bool read_record(int slot);
    void nudge_neighbor_away(int slot);

    Hardware& hw_;
    Box& box_;
    Feeder& feeder_;
    const Config& cfg_;
    UsageManager* usage_ = nullptr;
};

// Thread `preload`: call step() every few ms and sample() every 20 ms.
class Preload {
public:
    Preload(Hardware& hw, Box& box, Feeder& feeder, RfidScanner& scanner, const Config& cfg)
        : hw_(hw), box_(box), feeder_(feeder), scanner_(scanner), cfg_(cfg) {}

    void init();
    // 20 ms sensor timer: filters inputs and raises insert/remove events.
    void sample();
    // One step of the preload state machine (may block during operations).
    void step();

    bool inlet(int slot) const { return inlet_[slot].level(); }
    // Removal of a spool resets its usage record.
    void set_usage(UsageManager* u) { usage_ = u; }

private:
    enum class State : uint8_t { Wait, Pretension, CheckHub, Read };

    bool allowed() const;
    void enqueue(int slot);
    void dequeue(int slot);
    int pop();
    bool any_hub_loaded() const;
    void pretension(int slot);
    void finish(int slot);

    Hardware& hw_;
    Box& box_;
    Feeder& feeder_;
    RfidScanner& scanner_;
    const Config& cfg_;

    DebounceFilter inlet_[kSlotCount];
    DebounceFilter hub_[kSlotCount];
    bool present_[kSlotCount] = {};
    int8_t queue_[kSlotCount] = {-1, -1, -1, -1};
    State state_ = State::Wait;
    int slot_ = -1;
    UsageManager* usage_ = nullptr;
};

}  // namespace cfs
