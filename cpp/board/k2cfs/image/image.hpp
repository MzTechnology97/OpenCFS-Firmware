// SPDX-License-Identifier: Apache-2.0
// OpenCFS image for the K2 CFS board: start-up, clock, vector table and entry
// (docs/13-application.md section 4). EXPERIMENTAL: not qualified on hardware.
#pragma once

#include <cstdint>

namespace cfs::image {

// Application base used by the Creality loader, and the end of the application
// area: the last flash page holds the parameters (docs/08, docs/10).
constexpr uint32_t kAppBase = 0x08003000;
constexpr uint32_t kParamPage = 0x0807F800;

void clock_init();

// RS-485 TX (PA2) as a GPIO output held high, so the bus stays free (docs/10
// section 2: the transceiver has no direction pin and follows TX).
void release_bus();

// Motors braked, hub PWM held off, interrupts off, then wait forever. Used by
// every fault and unexpected interrupt. Needs no RAM content and little stack.
[[noreturn]] void fault_safe_state();

}  // namespace cfs::image
