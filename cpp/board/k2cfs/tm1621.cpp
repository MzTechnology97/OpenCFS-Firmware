// SPDX-License-Identifier: Apache-2.0
#include "tm1621.hpp"

#include <initializer_list>

namespace cfs::board {

using gd32::PinMode;

namespace {
// Segment words for 0..9 (stock table at 0x08029C0C).
constexpr uint16_t kDigits[10] = {0x1323, 0x0300, 0x1132, 0x1330, 0x0311,
                                  0x1231, 0x1233, 0x1300, 0x1333, 0x1331};
constexpr uint16_t kMinus = 0x0010;
constexpr uint16_t kAlwaysOn = 0x2000;  // set on every digit word by stock
constexpr uint32_t kWrLowUs = 3;
constexpr uint32_t kWrHighUs = 2;
}  // namespace

// One bit per WR pulse: DATA, then WR low for 3 µs (stock 0x0800A1BA).
void Tm1621::bits_msb(uint8_t value, int count) {
    for (int i = 0; i < count; ++i) {
        pin_write(pins::kLcdData, (value & 0x80) != 0);
        value = static_cast<uint8_t>(value << 1);
        pin_write(pins::kLcdWr, false);
        delay_us(kWrLowUs);
        pin_write(pins::kLcdWr, true);
        delay_us(kWrHighUs);
    }
}

void Tm1621::bits_lsb(uint16_t value, int count) {
    for (int i = 0; i < count; ++i) {
        pin_write(pins::kLcdData, (value & 1) != 0);
        value = static_cast<uint16_t>(value >> 1);
        pin_write(pins::kLcdWr, false);
        delay_us(kWrLowUs);
        pin_write(pins::kLcdWr, true);
        delay_us(kWrHighUs);
    }
}

// "100" + 0 + command[7:0] (stock 0x0800A2AE sends 1000 then 8 bits).
void Tm1621::command(uint8_t cmd) {
    pin_write(pins::kLcdCs, false);
    bits_msb(0x80, 4);
    bits_msb(cmd, 8);
    pin_write(pins::kLcdCs, true);
}

// "101" + 6-bit address (MSB first) + 16 data bits (LSB first), 4 nibbles from
// address pos * 4 (stock 0x0800A268 via 0x0800A370).
void Tm1621::write_digit(unsigned pos, uint16_t segments) {
    const uint8_t addr = static_cast<uint8_t>(((pos * 4u) & 0x3Fu) << 2);
    pin_write(pins::kLcdCs, false);
    bits_msb(0xA0, 3);
    bits_msb(addr, 6);
    bits_lsb(static_cast<uint16_t>(segments | kAlwaysOn), 16);
    pin_write(pins::kLcdCs, true);
}

void Tm1621::init() {
    pin_write(pins::kLcdCs, true);
    pin_write(pins::kLcdWr, true);
    pin_write(pins::kLcdRd, true);
    pin_mode(pins::kLcdCs, PinMode::OutputPP);
    pin_mode(pins::kLcdWr, PinMode::OutputPP);
    pin_mode(pins::kLcdRd, PinMode::OutputPP);
    pin_mode(pins::kLcdData, PinMode::OutputPP);
    pin_write(pins::kLcdBacklight, false);
    pin_mode(pins::kLcdBacklight, PinMode::OutputPP);
    delay_us(2000);
    for (uint8_t c : {uint8_t{0x40}, uint8_t{0x30}, uint8_t{0x00}, uint8_t{0x0A}, uint8_t{0x02},
                      uint8_t{0x06}}) {
        command(c);
    }
    delay_us(2000);
    backlight(true);
    show(0, 0, 0);
}

void Tm1621::show(int temp_c, unsigned hum_pct, unsigned box_index) {
    if (hum_pct > 99) hum_pct = 99;
    write_digit(0, kDigits[hum_pct / 10]);
    write_digit(1, kDigits[hum_pct % 10]);
    if (temp_c < 0) {
        if (temp_c < -9) temp_c = -9;
        write_digit(2, kMinus);
        write_digit(3, kDigits[-temp_c % 10]);
    } else {
        if (temp_c > 99) temp_c = 99;
        write_digit(2, kDigits[temp_c / 10]);
        write_digit(3, kDigits[temp_c % 10]);
    }
    write_digit(4, kDigits[box_index > 4 ? 0 : box_index]);
}

}  // namespace cfs::board
