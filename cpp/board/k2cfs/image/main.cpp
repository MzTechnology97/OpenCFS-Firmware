// SPDX-License-Identifier: Apache-2.0
// Entry of the OpenCFS image: one BoardApp, the cooperative loop, and the three
// interrupt handlers it needs (docs/13-application.md section 4).
#include <cstdint>
#include <new>

#include "board_app.hpp"
#include "image.hpp"

namespace {

// Unassigned address: auto-addressing from the host assigns one, as with stock.
constexpr uint8_t kUnassignedAddress = 0xFE;

// Constructed in main_loop() rather than as a global, so nothing runs before
// the clock is up and no destructor is ever registered.
alignas(cfs::board::BoardApp) uint8_t g_storage[sizeof(cfs::board::BoardApp)];
cfs::board::BoardApp* g_app = nullptr;

}  // namespace

extern "C" void SysTick_Handler() {
    if (g_app) g_app->on_systick();
}

extern "C" void USART1_IRQHandler() {
    if (g_app) g_app->on_usart1_irq();
}

extern "C" void EXTI10_15_IRQHandler() {
    if (g_app) g_app->on_exti10_15_irq();
}

namespace cfs::image {

void main_loop() {
    g_app = new (g_storage) board::BoardApp(kUnassignedAddress);
    g_app->init();
    for (;;) g_app->loop_once();
}

}  // namespace cfs::image
