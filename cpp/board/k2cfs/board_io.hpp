// SPDX-License-Identifier: Apache-2.0
// Low-level I/O for the K2 CFS controller board (GD32F303), pin map from
// docs/spec/10-board-port.md. Register access only; no RTOS dependency.
#pragma once

#include <cstddef>
#include <cstdint>

#include "gd32f30x_regs.hpp"

namespace cfs::board {

// Clock assumptions (to be confirmed on hardware): HCLK 120 MHz, APB1 60 MHz
// (timer clock 120 MHz), APB2 120 MHz. The application must configure the PLL
// accordingly or adjust these constants.
constexpr uint32_t kHclkHz = 120000000;
constexpr uint32_t kApb1Hz = 60000000;
constexpr uint32_t kTimerClkHz = 120000000;

// RT-Thread style logical pin: port * 16 + pin (0x00 = PA0).
struct Pin {
    uint8_t id;
    uint32_t port() const { return gd32::GPIOA + 0x400u * (id / 16u); }
    uint32_t mask() const { return 1u << (id % 16u); }
};

constexpr Pin P(char port, uint8_t pin) { return Pin{static_cast<uint8_t>((port - 'A') * 16 + pin)}; }

namespace pins {
// RS-485 (USART1).
constexpr Pin kRs485Tx = P('A', 2), kRs485Rx = P('A', 3);
// Feeders: IN1, IN2 per slot A..D.
constexpr Pin kFeederIn1[4] = {P('D', 3), P('D', 1), P('D', 15), P('D', 13)};
constexpr Pin kFeederIn2[4] = {P('D', 2), P('D', 0), P('D', 14), P('D', 12)};
constexpr uint8_t kFeederAdcChannel[4] = {15, 10, 1, 0};
constexpr Pin kFeederAdcPin[4] = {P('C', 5), P('C', 0), P('A', 1), P('A', 0)};
// Hub.
constexpr Pin kHubPwm = P('E', 13), kHubDir = P('E', 3), kHubBrake = P('E', 4), kHubFg = P('E', 11);
// Path sensors (low = filament).
constexpr Pin kInlet[4] = {P('D', 7), P('D', 6), P('D', 5), P('D', 4)};
constexpr Pin kHubSensor[4] = {P('E', 10), P('E', 9), P('E', 8), P('E', 7)};
constexpr Pin kBufferFull = P('D', 8), kBufferEmpty = P('D', 9);
// Odometer MT6826 on SPI1.
constexpr Pin kOdoCs = P('B', 12), kOdoSck = P('B', 13), kOdoMiso = P('B', 14), kOdoMosi = P('B', 15);
// Software I2C buses.
constexpr Pin kAhtScl = P('C', 11), kAhtSda = P('C', 12);
constexpr Pin kEepromScl = P('E', 5), kEepromSda = P('E', 6);
constexpr Pin kRfid0Scl = P('B', 10), kRfid0Sda = P('B', 11);
constexpr Pin kRfid1Scl = P('B', 8), kRfid1Sda = P('B', 9);
// Front display (HT1621-compatible) and backlight.
constexpr Pin kLcdCs = P('C', 1), kLcdRd = P('C', 2), kLcdWr = P('C', 3), kLcdData = P('C', 4);
constexpr Pin kLcdBacklight = P('B', 0);
// Slot LEDs.
constexpr Pin kLedA[4] = {P('B', 7), P('B', 4), P('B', 5), P('B', 6)};
constexpr Pin kLedB[4] = {P('C', 6), P('C', 7), P('C', 8), P('C', 9)};
// Hardware-version straps (both low = no FM15L013).
constexpr Pin kHwStrap0 = P('D', 10), kHwStrap1 = P('D', 11);
}  // namespace pins

// ---- GPIO --------------------------------------------------------------------
void gpio_clocks_on();
void pin_mode(Pin p, gd32::PinMode m);
void pin_pull(Pin p, bool up);  // with PinMode::InputPull
inline void pin_write(Pin p, bool high) {
    gd32::reg(p.port() + (high ? gd32::GPIO_BOP : gd32::GPIO_BC)) = p.mask();
}
inline bool pin_read(Pin p) { return (gd32::reg(p.port() + gd32::GPIO_ISTAT) & p.mask()) != 0; }

// ---- busy-wait microsecond delay (SysTick must run at HCLK) --------------------
void delay_us(uint32_t us);

// ---- PWM on TIMER0 / TIMER3 ------------------------------------------------------
class PwmTimer {
public:
    PwmTimer(uint32_t base, bool advanced) : base_(base), advanced_(advanced) {}
    // Edge-aligned PWM mode 0 at `hz` with `steps` counts per period.
    void init(uint32_t hz, uint16_t steps);
    void enable_channel(int ch);
    void set_compare(int ch, uint16_t value);
    uint16_t steps() const { return steps_; }

private:
    uint32_t base_;
    bool advanced_;
    uint16_t steps_ = 0;
};

// ---- USART1 (RS-485) ---------------------------------------------------------------
class Usart {
public:
    explicit Usart(uint32_t base) : base_(base) {}
    void init(uint32_t baud, uint32_t pclk_hz);
    void write_blocking(const uint8_t* data, size_t len);
    // Call from the USART IRQ handler; stores received bytes in the ring.
    void on_irq();
    bool read(uint8_t& b);

private:
    uint32_t base_;
    uint8_t rx_[512] = {};
    volatile uint16_t head_ = 0;
    volatile uint16_t tail_ = 0;
};

// ---- ADC0 one-shot conversions -------------------------------------------------------
class Adc {
public:
    explicit Adc(uint32_t base) : base_(base) {}
    void init();
    uint16_t read(uint8_t channel);  // 12-bit result
    static float volts(uint16_t raw) { return raw * 3.3f / 4096.0f; }

private:
    uint32_t base_;
};

// ---- SPI1 master (mode 3, 8 bit) -------------------------------------------------------
class Spi {
public:
    explicit Spi(uint32_t base) : base_(base) {}
    void init(uint32_t prescaler_bits);  // PSC field value 0..7 (fPCLK / 2^(n+1))
    uint8_t transfer(uint8_t out);

private:
    uint32_t base_;
};

// ---- software I2C (open drain, ~100 kHz) ------------------------------------------------
class SoftI2c {
public:
    SoftI2c(Pin scl, Pin sda) : scl_(scl), sda_(sda) {}
    void init();
    bool write(uint8_t addr7, const uint8_t* data, size_t len);
    bool read(uint8_t addr7, uint8_t* data, size_t len);

private:
    void start();
    void stop();
    bool send(uint8_t b);
    uint8_t recv(bool ack);
    Pin scl_, sda_;
};

}  // namespace cfs::board
