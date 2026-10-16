// SPDX-License-Identifier: Apache-2.0
// Buffer refill while printing (thread `print_check`, docs/04-feeder.md section 7).
#pragma once

#include <cstdint>

#include "cfs/box.hpp"
#include "cfs/config.hpp"
#include "cfs/hal.hpp"

namespace cfs {

class PrintTracker {
public:
    PrintTracker(Hardware& hw, Box& box, const Config& cfg) : hw_(hw), box_(box), cfg_(cfg) {}

    // Command 0x04 mode 0 / mode 1.
    Status start(int slot);
    void stop();
    bool active() const { return armed_; }

    // Buffer switch interrupts (only while armed).
    void on_buffer_empty();
    void on_buffer_full();

    // Thread body: runs one refill session when one is pending (blocking).
    void service();

    // Runout check used by BOX_STATE while printing.
    Status runout_check();

private:
    Hardware& hw_;
    Box& box_;
    const Config& cfg_;
    bool armed_ = false;
    bool session_ = false;
    bool retrigger_ = false;
    uint16_t retriggers_ = 0;
};

}  // namespace cfs
