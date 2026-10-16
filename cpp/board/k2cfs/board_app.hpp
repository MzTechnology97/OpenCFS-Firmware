// SPDX-License-Identifier: Apache-2.0
// The OpenCFS application on the K2 CFS board (docs/13-application.md). This
// file only wires Board and cfs::App; the vector table, reset handler and entry
// that call it are in image/ (EXPERIMENTAL, not qualified on hardware).
#pragma once

#include <cstdint>

#include "board_hal.hpp"
#include "cfs/app.hpp"

namespace cfs::board {

class BoardApp {
public:
    static constexpr uint32_t kEnvironmentMs = 1000;  // stock `lcd` thread period

    explicit BoardApp(uint8_t address);

    void init();       // board peripherals, clock idle hook, application
    void loop_once();  // one main-loop pass

    // Interrupt entry points for the image's vector table.
    void on_systick() { board_.clock.on_systick(); }
    void on_usart1_irq() { board_.on_usart1_irq(); }
    void on_exti10_15_irq() { board_.on_exti_fg(); }

    App& app() { return app_; }

private:
    static void idle();     // clock idle hook while a routine sleeps
    void service_board();   // Board::tick_1ms() once per elapsed millisecond

    static BoardApp* active_;
    Board board_;
    Hardware hw_;
    App app_;
    uint32_t board_ms_ = 0;
    uint32_t next_env_ms_ = 0;
    bool in_service_ = false;
};

}  // namespace cfs::board
