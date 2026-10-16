// SPDX-License-Identifier: Apache-2.0
#include "board_io.hpp"

namespace cfs::board {

using namespace cfs::gd32;

void gpio_clocks_on() {
    reg(RCU_APB2EN) |= APB2_AF | APB2_PA | APB2_PB | APB2_PC | APB2_PD | APB2_PE;
}

void pin_mode(Pin p, PinMode m) {
    const uint32_t pin = p.id % 16u;
    const uint32_t ctl = p.port() + (pin < 8 ? GPIO_CTL0 : GPIO_CTL1);
    const uint32_t shift = (pin % 8u) * 4u;
    reg(ctl) = (reg(ctl) & ~(0xFu << shift)) | (static_cast<uint32_t>(m) << shift);
}

void pin_pull(Pin p, bool up) { pin_write(p, up); }

void delay_us(uint32_t us) {
    // SysTick counts down at HCLK; spin over the elapsed ticks.
    const uint32_t load = reg(SYST_LOAD) + 1;
    uint32_t last = reg(SYST_VAL);
    uint64_t left = static_cast<uint64_t>(us) * (kHclkHz / 1000000u);
    while (left) {
        const uint32_t now = reg(SYST_VAL);
        const uint32_t elapsed = now <= last ? last - now : last + load - now;
        left = elapsed >= left ? 0 : left - elapsed;
        last = now;
    }
}

// ---- PWM --------------------------------------------------------------------------
void PwmTimer::init(uint32_t hz, uint16_t steps) {
    steps_ = steps;
    reg(base_ + TIM_CTL0) = 0;
    const uint32_t psc = kTimerClkHz / (static_cast<uint32_t>(hz) * steps);
    reg(base_ + TIM_PSC) = psc ? psc - 1 : 0;
    reg(base_ + TIM_CAR) = steps - 1u;
    reg(base_ + TIM_SWEVG) = 1;  // load PSC/CAR
    if (advanced_) reg(base_ + TIM_CCHP) |= TIM_CCHP_POEN;
    reg(base_ + TIM_CTL0) = TIM_CTL0_ARSE | TIM_CTL0_CEN;
}

void PwmTimer::enable_channel(int ch) {
    // PWM mode 0 (CHxCOMCTL = 110) with compare preload, output enabled, active high.
    const uint32_t ctl = base_ + (ch < 2 ? TIM_CHCTL0 : TIM_CHCTL1);
    const uint32_t shift = (ch % 2) ? 8u : 0u;
    reg(ctl) = (reg(ctl) & ~(0xFFu << shift)) | ((0x6u << 4 | 0x8u) << shift);
    reg(base_ + TIM_CHCTL2) |= 1u << (4 * ch);
}

void PwmTimer::set_compare(int ch, uint16_t value) {
    reg(base_ + TIM_CH0CV + 4u * static_cast<uint32_t>(ch)) = value;
}

uint16_t PwmTimer::compare(int ch) const {
    return static_cast<uint16_t>(reg(base_ + TIM_CH0CV + 4u * static_cast<uint32_t>(ch)));
}

// ---- USART ------------------------------------------------------------------------
void Usart::init(uint32_t baud, uint32_t pclk_hz) {
    reg(base_ + USART_CTL0) = 0;
    reg(base_ + USART_BAUD) = (pclk_hz + baud / 2) / baud;  // 16x oversampling: mantissa.fraction
    reg(base_ + USART_CTL0) = USART_CTL0_UEN | USART_CTL0_TEN | USART_CTL0_REN | USART_CTL0_RBNEIE;
}

void Usart::write_blocking(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        while (!(reg(base_ + USART_STAT0) & USART_STAT0_TBE)) {
        }
        reg(base_ + USART_DATA) = data[i];
    }
    while (!(reg(base_ + USART_STAT0) & USART_STAT0_TC)) {
    }
}

void Usart::on_irq() {
    // An overrun raises the interrupt (RBNEIE) without RBNE and stays set until
    // STAT0 then DATA are read: without this the interrupt would fire forever.
    // With RBNE also set, the loop below reads DATA and clears it.
    const uint32_t st = reg(base_ + USART_STAT0);
    if ((st & USART_STAT0_ERRORS) && !(st & USART_STAT0_RBNE)) {
        const uint32_t discard = reg(base_ + USART_DATA);
        (void)discard;
    }
    while (reg(base_ + USART_STAT0) & USART_STAT0_RBNE) {
        const uint8_t b = static_cast<uint8_t>(reg(base_ + USART_DATA));
        const uint16_t next = static_cast<uint16_t>((head_ + 1u) % sizeof rx_);
        if (next != tail_) {  // drop on overflow
            rx_[head_] = b;
            head_ = next;
        }
    }
}

bool Usart::read(uint8_t& b) {
    if (tail_ == head_) return false;
    b = rx_[tail_];
    tail_ = static_cast<uint16_t>((tail_ + 1u) % sizeof rx_);
    return true;
}

// ---- ADC --------------------------------------------------------------------------
void Adc::init() {
    reg(RCU_APB2EN) |= enable_;
    // ADC clock = APB2 / 6 = 20 MHz (the GD32F30x ADC allows at most 40 MHz).
    reg(RCU_CFG0) = (reg(RCU_CFG0) & ~RCU_CFG0_ADCPSC_MASK) | RCU_CFG0_ADCPSC_DIV6;
    reg(base_ + ADC_CTL1) = ADC_CTL1_ETSRC_SW | ADC_CTL1_ETERC;
    reg(base_ + ADC_SAMPT0) = 0x00FFFFFF;  // long sampling for channels 10..17
    reg(base_ + ADC_SAMPT1) = 0x3FFFFFFF;  // and 0..9
    reg(base_ + ADC_CTL1) |= ADC_CTL1_ADCON;
    delay_us(20);
    reg(base_ + ADC_CTL1) |= ADC_CTL1_RSTCLB;
    while (reg(base_ + ADC_CTL1) & ADC_CTL1_RSTCLB) {
    }
    reg(base_ + ADC_CTL1) |= ADC_CTL1_CLB;
    while (reg(base_ + ADC_CTL1) & ADC_CTL1_CLB) {
    }
}

uint16_t Adc::read(uint8_t channel) {
    reg(base_ + ADC_RSQ0) = 0;  // one conversion
    reg(base_ + ADC_RSQ2) = channel & 0x1Fu;
    reg(base_ + ADC_STAT) = 0;
    reg(base_ + ADC_CTL1) |= ADC_CTL1_SWRCST;
    while (!(reg(base_ + ADC_STAT) & ADC_STAT_EOC)) {
    }
    return static_cast<uint16_t>(reg(base_ + ADC_RDATA) & 0xFFFu);
}

// ---- SPI --------------------------------------------------------------------------
void Spi::init(uint32_t prescaler_bits) {
    reg(RCU_APB1EN) |= APB1_SPI1;
    reg(base_ + SPI_CTL0) = SPI_CTL0_MSTMOD | SPI_CTL0_CKPL | SPI_CTL0_CKPH | SPI_CTL0_SWNSSEN |
                            SPI_CTL0_SWNSS | ((prescaler_bits & 7u) << 3);
    reg(base_ + SPI_CTL0) |= SPI_CTL0_SPIEN;
}

uint8_t Spi::transfer(uint8_t out) {
    while (!(reg(base_ + SPI_STAT) & SPI_STAT_TBE)) {
    }
    reg(base_ + SPI_DATA) = out;
    while (!(reg(base_ + SPI_STAT) & SPI_STAT_RBNE)) {
    }
    return static_cast<uint8_t>(reg(base_ + SPI_DATA));
}

// ---- software I2C -----------------------------------------------------------------
namespace {
constexpr uint32_t kHalfBitUs = 5;  // ~100 kHz
}

void SoftI2c::init() {
    pin_write(scl_, true);
    pin_write(sda_, true);
    pin_mode(scl_, PinMode::OutputOD);
    pin_mode(sda_, PinMode::OutputOD);
}

void SoftI2c::start() {
    pin_write(sda_, true);
    pin_write(scl_, true);
    delay_us(kHalfBitUs);
    pin_write(sda_, false);
    delay_us(kHalfBitUs);
    pin_write(scl_, false);
}

void SoftI2c::stop() {
    pin_write(sda_, false);
    delay_us(kHalfBitUs);
    pin_write(scl_, true);
    delay_us(kHalfBitUs);
    pin_write(sda_, true);
    delay_us(kHalfBitUs);
}

bool SoftI2c::send(uint8_t b) {
    for (int i = 7; i >= 0; --i) {
        pin_write(sda_, (b >> i) & 1u);
        delay_us(kHalfBitUs);
        pin_write(scl_, true);
        delay_us(kHalfBitUs);
        pin_write(scl_, false);
    }
    pin_write(sda_, true);  // release for ACK
    delay_us(kHalfBitUs);
    pin_write(scl_, true);
    delay_us(kHalfBitUs);
    const bool ack = !pin_read(sda_);
    pin_write(scl_, false);
    return ack;
}

uint8_t SoftI2c::recv(bool ack) {
    uint8_t b = 0;
    pin_write(sda_, true);
    for (int i = 0; i < 8; ++i) {
        delay_us(kHalfBitUs);
        pin_write(scl_, true);
        delay_us(kHalfBitUs);
        b = static_cast<uint8_t>((b << 1) | (pin_read(sda_) ? 1 : 0));
        pin_write(scl_, false);
    }
    pin_write(sda_, !ack);
    delay_us(kHalfBitUs);
    pin_write(scl_, true);
    delay_us(kHalfBitUs);
    pin_write(scl_, false);
    pin_write(sda_, true);
    return b;
}

bool SoftI2c::write(uint8_t addr7, const uint8_t* data, size_t len) {
    start();
    bool ok = send(static_cast<uint8_t>(addr7 << 1));
    for (size_t i = 0; ok && i < len; ++i) ok = send(data[i]);
    stop();
    return ok;
}

bool SoftI2c::read(uint8_t addr7, uint8_t* data, size_t len) {
    start();
    const bool ok = send(static_cast<uint8_t>((addr7 << 1) | 1u));
    for (size_t i = 0; ok && i < len; ++i) data[i] = recv(i + 1 < len);
    stop();
    return ok;
}

}  // namespace cfs::board
