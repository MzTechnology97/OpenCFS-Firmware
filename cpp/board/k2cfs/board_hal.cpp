// SPDX-License-Identifier: Apache-2.0
#include "board_hal.hpp"

#include <initializer_list>

namespace cfs::board {

using namespace cfs::gd32;

// ---- clock ----------------------------------------------------------------------
void SysTickClock::init() {
    reg(SYST_LOAD) = kHclkHz / 1000u - 1u;
    reg(SYST_VAL) = 0;
    reg(SYST_CTRL) = 0x7;  // HCLK source, interrupt, enable
}

void SysTickClock::sleep_ms(uint32_t ms) {
    const uint32_t end = ms_ + ms;
    while (static_cast<int32_t>(ms_ - end) < 0) {
        if (idle_hook) idle_hook();
    }
}

// ---- feeders --------------------------------------------------------------------
namespace {
constexpr float kRippleBlockedV = 0.05f;
}  // namespace

void Feeders::init() {
    for (int s = 0; s < 4; ++s) {
        pin_write(pins::kFeederIn1[s], true);  // brake
        pin_write(pins::kFeederIn2[s], true);
        pin_mode(pins::kFeederIn1[s], PinMode::OutputPP);
        pin_mode(pins::kFeederIn2[s], PinMode::OutputPP);
        pin_mode(pins::kFeederAdcPin[s], PinMode::Analog);
    }
}

// Stock GPIO drive (0x08010054): direction 0 sets record +4 (IN2) high and +5 (IN1)
// low, direction 1 the opposite, speed 0 sets both high (brake). Any non-zero speed
// is full on. Which way the spool turns for direction 0 is not verified on hardware.
void Feeders::run(int slot, Dir dir, uint8_t speed) {
    if (speed == 0) {
        stop(slot);
        return;
    }
    const bool dir0 = dir == Dir::Forward;
    pin_write(pins::kFeederIn2[slot], dir0);
    pin_write(pins::kFeederIn1[slot], !dir0);
    if (!running_[slot]) {
        running_[slot] = true;
        fill_[slot] = 0;
        blocked_[slot] = false;
    }
}

void Feeders::stop(int slot) {
    pin_write(pins::kFeederIn1[slot], true);
    pin_write(pins::kFeederIn2[slot], true);
    running_[slot] = false;
    blocked_[slot] = false;
    fill_[slot] = 0;
}

void Feeders::tick_20ms() {
    for (int s = 0; s < 4; ++s) {
        if (!running_[s]) continue;
        const float v = Adc::volts(adc_.read(pins::kFeederAdcChannel[s]));
        float* w = window_[s];
        for (int i = 0; i < 4; ++i) w[i] = w[i + 1];
        w[4] = v;
        if (fill_[s] < 5) ++fill_[s];
        if (fill_[s] < 5) continue;
        float lo = w[0], hi = w[0], sum = 0.0f;
        for (int i = 0; i < 5; ++i) {
            lo = w[i] < lo ? w[i] : lo;
            hi = w[i] > hi ? w[i] : hi;
            sum += w[i];
        }
        avg_[s] = sum / 5.0f;
        blocked_[s] = (hi - lo) < kRippleBlockedV;  // no commutation ripple
    }
}

// ---- hub --------------------------------------------------------------------------
namespace {
constexpr float kHubKp = 1.0f, kHubKi = 0.75f;
constexpr float kHubMaxRps = 50.0f;
constexpr uint8_t kStallTicks = 20;   // 20 × 40 ms ≈ 0.8 s
constexpr uint8_t kOverloadOver = 39; // of the last 56 outputs
constexpr int kHubPwmChannel = 2;     // TIMER0 CH2 on PE13
}  // namespace

void Hub::init() {
    pin_write(pins::kHubBrake, true);
    pin_mode(pins::kHubBrake, PinMode::OutputPP);
    pin_mode(pins::kHubDir, PinMode::OutputPP);
    pin_mode(pins::kHubPwm, PinMode::AltPP);
    pin_mode(pins::kHubFg, PinMode::InputPull);
    pin_pull(pins::kHubFg, true);
    tim_.enable_channel(kHubPwmChannel);
    apply_output(0.0f);
}

void Hub::apply_output(float out) {
    if (out < 0.0f) out = 0.0f;
    if (out > 255.0f) out = 255.0f;
    // Active low: the driver sees `out`/255 of the period low.
    const uint32_t steps = tim_.steps();
    tim_.set_compare(kHubPwmChannel, static_cast<uint16_t>(steps - (steps * static_cast<uint32_t>(out)) / 255u));
}

void Hub::run(Dir dir, uint8_t speed) {
    if (speed == 0) {
        stop();
        return;
    }
    dir_ = dir;
    pin_write(pins::kHubDir, dir == Dir::Forward);
    pin_write(pins::kHubBrake, false);
    if (speed_ == 0) {
        integral_ = 0.0f;
        stall_count_ = kStallTicks;
        last_pulses_ = pulses_;
    }
    speed_ = speed;
}

void Hub::stop() {
    pin_write(pins::kHubBrake, true);
    apply_output(0.0f);
    speed_ = 0;
    integral_ = 0.0f;
    stall_count_ = kStallTicks;
    overload_ = false;
    sat_count_ = 0;
    for (auto& s : saturated_) s = 0;
}

void Hub::tick_40ms() {
    const uint32_t p = pulses_;
    const uint32_t n = p - last_pulses_;
    last_pulses_ = p;
    rps_ = static_cast<float>(n) * 25.0f / 6.0f;  // 6 FG pulses per turn, 25 ticks/s
    if (speed_ == 0) return;

    if (n == 0) {
        if (stall_count_) --stall_count_;
    } else {
        stall_count_ = kStallTicks;
    }

    const float target = speed_ * kHubMaxRps / 255.0f;
    const float err = target - rps_;
    integral_ += err;
    float out = kHubKp * err + kHubKi * integral_;
    if (out > 255.0f) {
        out = 255.0f;
        integral_ -= err;  // anti-windup
    } else if (out < 0.0f) {
        out = 0.0f;
        integral_ -= err;
    }
    apply_output(out);

    const uint8_t sat = out >= 255.0f ? 1 : 0;
    sat_count_ = static_cast<uint8_t>(sat_count_ - saturated_[sat_pos_] + sat);
    saturated_[sat_pos_] = sat;
    sat_pos_ = static_cast<uint8_t>((sat_pos_ + 1) % sizeof saturated_);
    if (sat_count_ > kOverloadOver) overload_ = true;
}

// ---- sensors --------------------------------------------------------------------------
void Sensors::init() {
    for (int s = 0; s < 4; ++s) {
        pin_mode(pins::kInlet[s], PinMode::InputPull);
        pin_pull(pins::kInlet[s], true);
        pin_mode(pins::kHubSensor[s], PinMode::InputPull);
        pin_pull(pins::kHubSensor[s], true);
    }
    for (Pin p : {pins::kBufferFull, pins::kBufferEmpty}) {
        pin_mode(p, PinMode::InputPull);
        pin_pull(p, true);
    }
}

BufferState Sensors::buffer() {
    const uint8_t v = static_cast<uint8_t>((pin_read(pins::kBufferFull) ? 0 : 1) |
                                           (pin_read(pins::kBufferEmpty) ? 0 : 2));
    return static_cast<BufferState>(v);
}

// ---- MT6826 ---------------------------------------------------------------------------
// SPI framing assumed from the MagnTek MT68xx family (4-bit command + 12-bit address):
// read 0x3, write 0x6; angle in registers 0x003..0x004; user ID at 0x001.
// Framing matches the stock driver (0x0800CC20 / 0x0800CC66 / 0x0800CCFC) and a
// public logic-analyzer capture of a running CFS (docs/spec/10 section 5): command
// nibble + 12-bit address, data follows in the same CS window, mode 3.
namespace {
constexpr uint8_t kCmdRead = 0x3, kCmdWrite = 0x6;
constexpr uint16_t kRegUserId = 0x001, kRegAngle = 0x003;
}  // namespace

uint8_t Mt6826Odometer::read_reg(uint16_t addr) {
    pin_write(pins::kOdoCs, false);
    spi_.transfer(static_cast<uint8_t>((kCmdRead << 4) | ((addr >> 8) & 0xF)));
    spi_.transfer(static_cast<uint8_t>(addr));
    const uint8_t v = spi_.transfer(0);
    pin_write(pins::kOdoCs, true);
    return v;
}

void Mt6826Odometer::write_reg(uint16_t addr, uint8_t value) {
    pin_write(pins::kOdoCs, false);
    spi_.transfer(static_cast<uint8_t>((kCmdWrite << 4) | ((addr >> 8) & 0xF)));
    spi_.transfer(static_cast<uint8_t>(addr));
    spi_.transfer(value);
    pin_write(pins::kOdoCs, true);
}

// One burst read of 0x003..0x004 (stock: 30 03 00 00), 15-bit angle = raw16 >> 1.
uint16_t Mt6826Odometer::read_angle() {
    pin_write(pins::kOdoCs, false);
    spi_.transfer(static_cast<uint8_t>((kCmdRead << 4) | ((kRegAngle >> 8) & 0xF)));
    spi_.transfer(static_cast<uint8_t>(kRegAngle));
    const uint16_t hi = spi_.transfer(0);
    const uint16_t lo = spi_.transfer(0);
    pin_write(pins::kOdoCs, true);
    return static_cast<uint16_t>(((hi << 8) | lo) >> 1);
}

bool Mt6826Odometer::init() {
    pin_write(pins::kOdoCs, true);
    pin_mode(pins::kOdoCs, PinMode::OutputPP);
    pin_mode(pins::kOdoSck, PinMode::AltPP);
    pin_mode(pins::kOdoMosi, PinMode::AltPP);
    pin_mode(pins::kOdoMiso, PinMode::InputFloat);
    spi_.init(6);  // APB1 / 128 = 469 kHz (stock bit-bangs at ~312 kHz)
    if (read_reg(kRegUserId) != 0x7E) write_reg(kRegUserId, 0x7E);
    return read_reg(kRegUserId) == 0x7E;
}

void Mt6826Odometer::poll() {
    const uint16_t a = read_angle();
    if (primed_) {
        int32_t d = static_cast<int32_t>(a) - static_cast<int32_t>(last_angle_);
        if (d > 16384) d -= 32768;  // unwrap a 15-bit turn
        if (d < -16384) d += 32768;
        count_ += d;
    }
    last_angle_ = a;
    primed_ = true;
}

// ---- AHT30 ---------------------------------------------------------------------------
namespace {
constexpr uint8_t kAhtAddr = 0x38;
}

bool Aht30::init() {
    bus_.init();
    uint8_t status = 0;
    if (!bus_.read(kAhtAddr, &status, 1)) return false;
    if (!(status & 0x08)) {  // not calibrated
        const uint8_t cal[3] = {0xBE, 0x08, 0x00};
        bus_.write(kAhtAddr, cal, 3);
    }
    return true;
}

bool Aht30::update(Clock& clock) {
    const uint8_t trig[3] = {0xAC, 0x33, 0x00};
    if (!bus_.write(kAhtAddr, trig, 3)) return false;
    clock.sleep_ms(80);
    uint8_t d[7] = {};
    if (!bus_.read(kAhtAddr, d, sizeof d) || (d[0] & 0x80)) return false;  // busy
    const uint32_t hum = (static_cast<uint32_t>(d[1]) << 12) | (static_cast<uint32_t>(d[2]) << 4) | (d[3] >> 4);
    const uint32_t tmp = (static_cast<uint32_t>(d[3] & 0x0F) << 16) | (static_cast<uint32_t>(d[4]) << 8) | d[5];
    hum_pct_ = static_cast<uint8_t>((hum * 100u) >> 20);
    temp_c_ = static_cast<int8_t>(static_cast<int32_t>((tmp * 200u) >> 20) - 50);
    return true;
}

// ---- LEDs ------------------------------------------------------------------------------
void Leds::init() {
    for (int s = 0; s < 4; ++s) {
        pin_write(pins::kLedA[s], false);
        pin_write(pins::kLedB[s], false);
        pin_mode(pins::kLedA[s], PinMode::OutputPP);
        pin_mode(pins::kLedB[s], PinMode::OutputPP);
    }
}

// Simplified patterns: breathing is shown as a slow blink.
void Leds::tick_ms(uint32_t now) {
    const bool b500 = (now / 500) % 2;
    const bool b1000 = (now / 1000) % 2;
    const uint32_t ph = now % 1600;
    const bool dbl = ph < 200 || (ph >= 400 && ph < 600);
    for (int s = 0; s < 4; ++s) {
        bool a = false, b = false;
        switch (mode_[s]) {
        case 0: b = (now / 500) % 4 == static_cast<uint32_t>(s); break;
        case 1: a = (now / 500) % 4 == static_cast<uint32_t>(s); break;
        case 2: case 12: a = true; break;
        case 3: a = b1000; break;
        case 4: b = b500; break;
        case 5: case 13: b = true; break;
        case 6: b = dbl; break;
        case 7: case 8: b = b1000; break;
        case 9: a = (now / 1200) % 2; b = !a; break;
        case 10: a = b500; break;
        default: break;
        }
        pin_write(pins::kLedA[s], a);
        pin_write(pins::kLedB[s], b);
    }
}

// ---- system -----------------------------------------------------------------------------
void McuSystem::reset_after_ms(uint32_t ms) {
    clock_.sleep_ms(ms);
    reg(0xE000ED0C) = 0x05FA0004;  // SCB AIRCR: SYSRESETREQ
    for (;;) {
    }
}

void McuSystem::unique_id(uint8_t out[12]) {
    const volatile uint8_t* uid = reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(0x1FFFF7E8u));
    for (int i = 0; i < 12; ++i) out[i] = uid[i];
}

void McuSystem::serial_number(char out[19]) {
    // The stock serial comes from production data; use the UID until that is known.
    static const char hex[] = "0123456789ABCDEF";
    uint8_t uid[12];
    unique_id(uid);
    out[0] = 'C';
    out[1] = 'F';
    out[2] = 'S';
    for (int i = 0; i < 8; ++i) {
        out[3 + 2 * i] = hex[uid[i] >> 4];
        out[4 + 2 * i] = hex[uid[i] & 0xF];
    }
}

// ---- parameter page -------------------------------------------------------------
namespace {
bool fmc_wait() {
    while (reg(FMC_STAT0) & FMC_STAT_BUSY) {
    }
    const uint32_t st = reg(FMC_STAT0);
    reg(FMC_STAT0) = FMC_STAT_ENDF | FMC_STAT_PGERR | FMC_STAT_WPERR;  // write 1 to clear
    return !(st & (FMC_STAT_PGERR | FMC_STAT_WPERR));
}

void fmc_unlock() {
    if (reg(FMC_CTL0) & FMC_CTL_LK) {
        reg(FMC_KEY0) = FMC_UNLOCK_KEY0;
        reg(FMC_KEY0) = FMC_UNLOCK_KEY1;
    }
}
}  // namespace

void FlashSettings::load() {
    uint8_t raw[params::kRecordSize];
    const volatile uint8_t* page = reinterpret_cast<const volatile uint8_t*>(
        static_cast<uintptr_t>(kPageAddr));
    for (size_t i = 0; i < sizeof raw; ++i) raw[i] = page[i];
    if (params::load(raw, kVersion, rec_)) save();
}

void FlashSettings::set_mode(uint8_t m) {
    if (m == rec_.mode) return;
    rec_.mode = m;
    save();
}

// Same sequence as stock (0x08011DF0): erase the page, then program the record
// word by word. The rest of the page stays erased.
bool FlashSettings::save() {
    uint8_t raw[params::kRecordSize];
    params::encode(rec_, raw);
    fmc_unlock();
    bool ok = fmc_wait();
    reg(FMC_CTL0) |= FMC_CTL_PER;
    reg(FMC_ADDR0) = kPageAddr;
    reg(FMC_CTL0) |= FMC_CTL_START;
    ok = fmc_wait() && ok;
    reg(FMC_CTL0) &= ~FMC_CTL_PER;
    for (size_t i = 0; ok && i < sizeof raw; i += 4) {
        const uint32_t word = static_cast<uint32_t>(raw[i]) | static_cast<uint32_t>(raw[i + 1]) << 8 |
                              static_cast<uint32_t>(raw[i + 2]) << 16 |
                              static_cast<uint32_t>(raw[i + 3]) << 24;
        reg(FMC_CTL0) |= FMC_CTL_PG;
        reg(kPageAddr + static_cast<uint32_t>(i)) = word;
        ok = fmc_wait();
        reg(FMC_CTL0) &= ~FMC_CTL_PG;
    }
    reg(FMC_CTL0) |= FMC_CTL_LK;
    write_ok_ = ok;
    return ok;
}

// ---- board -------------------------------------------------------------------------------
Board::Board() = default;

void Board::init() {
    clock.init();
    gpio_clocks_on();
    reg(RCU_APB2EN) |= APB2_TIMER0;
    reg(RCU_APB1EN) |= APB1_USART1;
    reg(AFIO_PCF0) |= PCF0_TIMER0_FULL_REMAP;

    tim0_.init(20000, static_cast<uint16_t>(kTimerClkHz / 20000));  // hub 20 kHz
    adc_.init();

    pin_mode(pins::kRs485Tx, PinMode::AltPP);
    pin_mode(pins::kRs485Rx, PinMode::InputFloat);
    usart_.init(230400, kApb1Hz);
    nvic_enable(IRQ_USART1);

    // FG on PE11: EXTI line 11, port E (4), rising edge.
    reg(AFIO_EXTISS0 + 8) = (reg(AFIO_EXTISS0 + 8) & ~(0xFu << 12)) | (4u << 12);
    reg(EXTI_RTEN) |= 1u << 11;
    reg(EXTI_INTEN) |= 1u << 11;
    nvic_enable(IRQ_EXTI10_15);

    settings_.load();
    feeders_.init();
    hub_.init();
    sensors_.init();
    odometer_.init();
    env_.init();
    leds_.init();
    pin_write(pins::kLcdBacklight, false);
    pin_mode(pins::kLcdBacklight, PinMode::OutputPP);
}

Hardware Board::hardware() {
    return Hardware{clock, feeders_, hub_, odometer_, sensors_, env_, leds_, rfid_,
                    settings_, link_, system_};
}

void Board::tick_1ms() {
    const uint32_t now = clock.now_ms();
    if (now % 20 == 0) {
        odometer_.poll();  // stock samples the encoder every 20 ms
        feeders_.tick_20ms();
    }
    if (now % 40 == 0) hub_.tick_40ms();
    leds_.tick_ms(now);
}

}  // namespace cfs::board
