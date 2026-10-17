// SPDX-License-Identifier: Apache-2.0
#include "cfs/auto_address.hpp"

namespace cfs {

AutoAddress::AutoAddress(Hardware& hw, uint8_t initial_address)
    : hw_(hw), address_(initial_address) {
    hw_.system.unique_id(uid_);
    // Seed from the unique ID so boxes on one bus pick different delays.
    rng_ = (static_cast<uint32_t>(crc8(uid_, sizeof uid_)) << 16) | 0x9E37u;
}

uint32_t AutoAddress::next_random() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void AutoAddress::schedule(uint8_t cmd, uint32_t delay_ms, uint8_t repeats) {
    for (Pending& p : pending_) {
        if (!p.used || p.cmd == cmd) {
            p = Pending{true, cmd, repeats, hw_.clock.now_ms() + delay_ms};
            return;
        }
    }
}

void AutoAddress::handle(const Frame& f) {
    const bool to_me = f.addr == address_;
    const bool broadcast = f.addr == kUnassigned || f.addr == kBroadcast;
    if (!is_auto_command(f.cmd) || !(to_me || broadcast)) {
        return;
    }
    // Every reply is [device type, mode, uid[12]]; no request has that length.
    // Ignoring it keeps a box from answering its own echo or another box.
    if (f.len == kReplyLen) return;
    switch (static_cast<AutoCmd>(f.cmd)) {
    case AutoCmd::Discover:
        // Only boxes without an address answer, after a random back-off.
        if (address_ == kUnassigned && f.len >= 2 && f.payload[0] != kBroadcast &&
            f.payload[1] >= kUnassigned) {
            schedule(f.cmd, next_random() % (kDiscoverWindowMs + 1), 1);
        }
        break;
    case AutoCmd::Assign: {
        if (f.len < 13) break;
        bool match = true;
        for (int i = 0; i < 12; ++i) {
            match = match && f.payload[1 + i] == uid_[i];
        }
        if (match) {
            address_ = f.payload[0];
            schedule(f.cmd, 0, kAssignRepeats);
        }
        break;
    }
    case AutoCmd::Online:
    case AutoCmd::Table:
        if (to_me) schedule(f.cmd, 0, 1);
        break;
    }
}

void AutoAddress::poll() {
    for (Pending& p : pending_) {
        if (!p.used || static_cast<int32_t>(hw_.clock.now_ms() - p.due_ms) < 0) {
            continue;
        }
        uint8_t payload[14] = {1, 0};  // device type CFS, mode application
        for (int i = 0; i < 12; ++i) payload[2 + i] = uid_[i];
        uint8_t buf[20];
        const size_t n = encode(address_, 0x00, p.cmd, payload, sizeof payload, buf, sizeof buf);
        for (uint8_t r = 0; r < p.repeats; ++r) {
            hw_.link.write(buf, n);
        }
        p.used = false;
    }
}

}  // namespace cfs
