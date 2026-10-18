// SPDX-License-Identifier: Apache-2.0
#include "cfs/app.hpp"

namespace cfs {

App::App(Hardware& hw, uint8_t address)
    : feeder(hw, box, cfg),
      print(hw, box, cfg),
      scanner(hw, box, feeder, cfg),
      preload(hw, box, feeder, scanner, cfg),
      usage(hw, box),
      supervisor(hw, box, cfg),
      disp(hw, box, cfg, feeder, print, address),
      hw_(hw) {}

void App::init() {
    disp.set_scanner(&scanner);
    disp.set_preload(&preload);
    disp.set_usage(&usage);
    scanner.set_usage(&usage);
    preload.set_usage(&usage);
    preload.init();
    // Stock main thread: switch_mode(database key 6), then read-on-insert on. The
    // aging/test modes are not implemented: every mode runs the standard logic.
    box.read_on_insert = true;
    last_buffer_ = hw_.sensors.buffer();
    const uint32_t now = hw_.clock.now_ms();
    next_sample_ = now + kSampleMs;
    next_supervise_ = now + kSuperviseMs;
}

void App::duties() {
    if (in_duties_) return;  // a duty is sleeping (reverse detooth)
    in_duties_ = true;
    const uint32_t now = hw_.clock.now_ms();
    if (static_cast<int32_t>(now - next_sample_) >= 0) {
        next_sample_ = now + kSampleMs;
        preload.sample();
    }
    // Buffer switches: the stock EXTI lines, armed only while printing.
    const BufferState b = hw_.sensors.buffer();
    if (b != last_buffer_) {
        if (b == BufferState::Empty) print.on_buffer_empty();
        if (b == BufferState::Full) print.on_buffer_full();
        last_buffer_ = b;
    }
    usage.tick();
    if (static_cast<int32_t>(now - next_supervise_) >= 0) {
        next_supervise_ = now + kSuperviseMs;
        supervisor.tick();  // may sleep ~300 ms for a detooth
    }
    in_duties_ = false;
}

void App::set_motion_locked(bool locked) {
    motion_locked_ = locked;
    disp.set_motion_locked(locked);
    supervisor.set_corrective(!locked);
}

void App::loop_once() {
    duties();
    in_dispatch_ = true;  // long commands (load, unload) run inline, like stock
    disp.poll();
    in_dispatch_ = false;
    duties();
    if (!motion_locked_) preload.step();  // pretension and RFID scan block here, like stock
    print.service();  // buffer refill session while printing
}

void App::background() {
    duties();
    // A blocked dispatcher owns the link (stage-4 inline reader); otherwise answer
    // the commands that cannot start motion.
    if (!in_dispatch_ && !in_duties_) {
        in_dispatch_ = true;
        disp.poll_background();
        in_dispatch_ = false;
    }
}

}  // namespace cfs
