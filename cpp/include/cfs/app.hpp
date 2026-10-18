// SPDX-License-Identifier: Apache-2.0
// The OpenCFS application: one cooperative main loop that replaces the stock
// RT-Thread threads and soft timers (docs/13-application.md).
//
//   stock                          OpenCFS
//   rs485 thread (dispatcher)      loop_once(): Dispatcher::poll()
//   preload thread, 1 ms step      loop_once(): Preload::step()
//   print_check thread             loop_once(): PrintTracker::service()
//   presd soft timer, 20 ms        duties(): Preload::sample()
//   motor_blocked timer, 100 ms    duties(): MotorSupervisor::tick()
//   buffer EXTI lines              duties(): buffer edge -> PrintTracker
//   usage manager                  duties(): UsageManager::tick()
//
// Long routines (load, unload, RFID scan, refill) block loop_once() exactly as
// they block their stock thread. While they sleep, the board calls background()
// from its clock idle hook, so supervision, filtering and buffer edges keep
// running, and the host still gets answers to commands that never move a motor.
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/dispatcher.hpp"
#include "cfs/feeder.hpp"
#include "cfs/hal.hpp"
#include "cfs/preload.hpp"
#include "cfs/print_tracker.hpp"
#include "cfs/supervisor.hpp"
#include "cfs/usage.hpp"

namespace cfs {

class App {
public:
    static constexpr uint32_t kSampleMs = 20;      // stock `presd`
    static constexpr uint32_t kSuperviseMs = MotorSupervisor::kPeriodMs;

    App(Hardware& hw, uint8_t address);

    void init();       // after the board is up: filters, read-on-insert
    void loop_once();  // one main-loop pass; may block inside a long routine
    void background(); // from the clock idle hook while a routine sleeps

    Config cfg;
    Box box;
    Feeder feeder;
    PrintTracker print;
    RfidScanner scanner;
    Preload preload;
    UsageManager usage;
    MotorSupervisor supervisor;
    Dispatcher disp;

private:
    void duties();

    Hardware& hw_;
    bool in_dispatch_ = false;  // the dispatcher itself is blocked in a command
    bool in_duties_ = false;    // a duty (detooth) is sleeping
    uint32_t next_sample_ = 0;
    uint32_t next_supervise_ = 0;
    BufferState last_buffer_ = BufferState::Between;
};

}  // namespace cfs
