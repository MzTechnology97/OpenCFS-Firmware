// SPDX-License-Identifier: Apache-2.0
// Front display: TM1621B (HT1621-compatible) segment driver on a bit-banged 3-wire
// bus, as the stock `lcd` thread drives it (0x0800A164..0x0800A4A8, docs/spec/06
// section 5). NOT qualified on hardware.
#pragma once

#include <cstdint>

#include "board_io.hpp"

namespace cfs::board {

class Tm1621 {
public:
    // Stock init: bus idle, commands 0x40 0x30 0x00 0x0A 0x02 0x06, backlight on,
    // then 0 °C / 0 % / box 0.
    void init();
    // Stock layout: digits 0-1 humidity, 2-3 temperature (-9..99, minus sign in
    // digit 2), digit 4 the box index (0..4, anything else shows 0).
    void show(int temp_c, unsigned hum_pct, unsigned box_index);
    void backlight(bool on) { pin_write(pins::kLcdBacklight, on); }

private:
    void bits_msb(uint8_t value, int count);
    void bits_lsb(uint16_t value, int count);
    void command(uint8_t cmd);
    void write_digit(unsigned pos, uint16_t segments);
};

}  // namespace cfs::board
