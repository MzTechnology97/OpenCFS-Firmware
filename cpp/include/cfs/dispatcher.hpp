// SPDX-License-Identifier: Apache-2.0
// RS-485 command dispatcher (thread `rs485`, docs/03-rs485-protocol.md).
#pragma once

#include <cstdint>

#include "cfs/auto_address.hpp"
#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/feeder.hpp"
#include "cfs/frame.hpp"
#include "cfs/hal.hpp"
#include "cfs/preload.hpp"
#include "cfs/print_tracker.hpp"
#include "cfs/runtime_config.hpp"
#include "cfs/usage.hpp"

namespace cfs {

class Dispatcher {
public:
    Dispatcher(Hardware& hw, Box& box, Config& cfg, Feeder& feeder, PrintTracker& print,
               uint8_t address)
        : hw_(hw), box_(box), cfg_(cfg), feeder_(feeder), print_(print), auto_(hw, address),
          rx_(address), address_(address), interlock_(hw, box), runtime_(cfg, interlock_) {}

    // Reads every available byte and executes complete frames.
    void poll();
    // Executes one decoded frame (also used by tests).
    void handle(const Frame& f);

    // Current RS-485 address: the initial one, or the one assigned through A0.
    uint8_t address() const { return address_; }
    // Enables forced RFID reads (0x0D sub 2/3); they run synchronously like stock.
    void set_scanner(RfidScanner* s) { scanner_ = s; }
    // Enables usage accounting hooks (print start/end, retract).
    void set_usage(UsageManager* u) { usage_ = u; }

private:
    class LoadLink;

    void reply(uint8_t cmd, Status st, const uint8_t* data = nullptr, size_t len = 0);
    void reply_raw(uint8_t cmd, uint8_t status, const uint8_t* data, size_t len);
    bool check_len(const Frame& f, uint8_t payload_len);
    // Slot mask → slot index (0..3); -1 = "all" (mask 0, only when allowed).
    bool slot_from_mask(uint8_t mask, bool allow_zero, int& slot);
    bool busy() const;

    void cmd_rfid_records(uint8_t mask);
    void cmd_rfid_remaining(uint8_t mask);
    void cmd_box_state();
    void cmd_load(int slot, uint8_t stage, uint8_t arg);

    Hardware& hw_;
    Box& box_;
    Config& cfg_;
    Feeder& feeder_;
    PrintTracker& print_;
    RfidScanner* scanner_ = nullptr;
    UsageManager* usage_ = nullptr;
    AutoAddress auto_;
    Receiver rx_;
    uint8_t address_;
    uint8_t reply_addr_ = 0;
    bool silent_ = false;
    Status stage4_result_ = Status::Ok;
    bool loading_ = false;
    int loading_slot_ = -1;
    MotionInterlock interlock_;
    RuntimeConfig runtime_;
};

}  // namespace cfs
