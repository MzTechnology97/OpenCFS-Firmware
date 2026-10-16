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

// Motors braked, hub PWM held off, interrupts off, then wait forever. Used by
// every fault and unexpected interrupt. Needs no RAM content and little stack.
[[noreturn]] void fault_safe_state();

}  // namespace cfs::image
