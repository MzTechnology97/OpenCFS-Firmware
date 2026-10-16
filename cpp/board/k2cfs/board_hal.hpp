// SPDX-License-Identifier: Apache-2.0
// cfs::Hardware implementation for the K2 CFS controller board (GD32F303).
// Bare-metal: the application calls the tick functions from SysTick / a 1 ms
// loop and the IRQ hooks from the vector table. NOT qualified on hardware.
#pragma once

#include <cstdint>

#include "board_io.hpp"
#include "fm17622.hpp"
#include "cfs/hal.hpp"
#include "cfs/param_store.hpp"

namespace cfs::board {

class SysTickClock : public Clock {
public:
    void init();                       // 1 ms SysTick at HCLK
    void on_systick() { ms_ = ms_ + 1; }
    uint32_t now_ms() override { return ms_; }
    void sleep_ms(uint32_t ms) override;
    // Called while sleeping (lets the board run its periodic tasks).
    void (*idle_hook)() = nullptr;

private:
    volatile uint32_t ms_ = 0;
};

// Four AT8236 feeders driven on/off by GPIO like stock (every motor record has
// no PWM device), with the stock current-ripple stall detector (docs/spec/06 section 1).
class Feeders : public FeederDriver {
public:
    explicit Feeders(Adc& adc) : adc_(adc) {}
    void init();
    void run(int slot, Dir dir, uint8_t speed) override;
    void stop(int slot) override;
    bool blocked(int slot) override { return blocked_[slot]; }
    void tick_20ms();  // current monitor
    float last_volts(int slot) const { return avg_[slot]; }

private:
    Adc& adc_;
    bool running_[4] = {};
    bool blocked_[4] = {};
    float window_[4][5] = {};
    uint8_t fill_[4] = {};
    float avg_[4] = {};
};

// MS3791 hub: TIMER0 CH2 PWM (20 kHz, active low), DIR, BRAKE, FG speed loop.
class Hub : public HubDriver {
public:
    explicit Hub(PwmTimer& tim0) : tim_(tim0) {}
    void init();
    void run(Dir dir, uint8_t speed) override;
    void stop() override;
    bool blocked() override { return speed_ != 0 && stall_count_ == 0; }
    bool overload() override { return overload_; }
    Dir current_dir() override { return dir_; }
    uint8_t current_speed() override { return speed_; }
    void on_fg_edge() { pulses_ = pulses_ + 1; }
    void tick_40ms();  // speed PID, blocked/overload detection
    float rps() const { return rps_; }

private:
    void apply_output(float out);

    PwmTimer& tim_;
    Dir dir_ = Dir::Forward;
    uint8_t speed_ = 0;
    volatile uint32_t pulses_ = 0;
    uint32_t last_pulses_ = 0;
    float rps_ = 0.0f;
    float integral_ = 0.0f;
    uint8_t stall_count_ = 20;
    uint8_t saturated_[56] = {};
    uint8_t sat_pos_ = 0;
    uint8_t sat_count_ = 0;
    bool overload_ = false;
};

class Sensors : public PathSensors {
public:
    void init();
    bool inlet_present(int slot) override { return !pin_read(pins::kInlet[slot]); }
    bool hub_present(int slot) override { return !pin_read(pins::kHubSensor[slot]); }
    BufferState buffer() override;
};

// MT6826 magnetic encoder on SPI1, unwrapped to a multi-turn count.
class Mt6826Odometer : public Odometer {
public:
    explicit Mt6826Odometer(Spi& spi) : spi_(spi) {}
    bool init();          // stock: write user ID 0x7E and read it back
    void poll();          // every 20 ms like stock; needs < half a turn per call
    float position_mm() override { return static_cast<float>(count_) * kMmPerCount; }
    void reset() override { count_ = 0; }

    static constexpr float kMmPerCount = 0.000788641f;  // stock scale

private:
    uint8_t read_reg(uint16_t addr);
    void write_reg(uint16_t addr, uint8_t value);
    uint16_t read_angle();  // 15-bit

    Spi& spi_;
    int32_t count_ = 0;
    uint16_t last_angle_ = 0;
    bool primed_ = false;
};

class Aht30 : public Environment {
public:
    explicit Aht30(SoftI2c& bus) : bus_(bus) {}
    bool init();
    // Starts a measurement, waits ~80 ms (clock.sleep_ms) and caches the result.
    bool update(Clock& clock);
    int8_t temperature_c() override { return temp_c_; }
    uint8_t humidity_pct() override { return hum_pct_; }

private:
    SoftI2c& bus_;
    int8_t temp_c_ = 25;
    uint8_t hum_pct_ = 0;
};

// Two LEDs per slot; modes from docs/spec/06 section 5 (simplified patterns).
class Leds : public Indicators {
public:
    void init();
    void set(int slot, uint8_t mode) override { mode_[slot] = mode; }
    void tick_ms(uint32_t now_ms);

private:
    uint8_t mode_[4] = {};
};

class Rs485 : public Transport {
public:
    explicit Rs485(Usart& u) : usart_(u) {}
    void write(const uint8_t* data, size_t len) override { usart_.write_blocking(data, len); }
    bool read_byte(uint8_t& b) override { return usart_.read(b); }

private:
    Usart& usart_;
};

// Stock-compatible parameter page at 0x0807F800 (cfs/param_store.hpp). Erasing
// and programming stall the CPU (and so RS-485 reception) for the page erase time,
// as in stock, which resets 100 ms after a mode change anyway.
class FlashSettings : public Settings {
public:
    static constexpr uint32_t kPageAddr = 0x0807F800;
    static constexpr uint32_t kPageSize = 0x800;
    // Version marker written to key 5. The stock 1.5.3 value, so a stock image
    // finds the page unchanged.
    static constexpr uint32_t kVersion = params::kStockVersion;

    void load();  // stock policy: defaults on a bad CRC, rewrite on another version
    uint8_t mode() override { return rec_.mode; }
    void set_mode(uint8_t m) override;
    bool last_write_ok() const { return write_ok_; }

private:
    bool save();

    params::Record rec_;
    bool write_ok_ = true;
};

class McuSystem : public System {
public:
    explicit McuSystem(Clock& clock) : clock_(clock) {}
    void reset_after_ms(uint32_t ms) override;
    void serial_number(char out[19]) override;
    void unique_id(uint8_t out[12]) override;
    void set_aux_output(bool on) override { pin_write(pins::kLcdBacklight, on); }

private:
    Clock& clock_;
};

// Everything wired together.
class Board {
public:
    Board();
    void init();
    Hardware hardware();
    // Creality tag keys come from the integrator (see TagCrypto); call before init.
    void set_tag_crypto(TagCrypto* crypto) { rfid_.set_crypto(crypto); }
    // 1 ms housekeeping: monitors, speed loop, odometer and LEDs.
    void tick_1ms();
    // IRQ hooks.
    void on_usart1_irq() { usart_.on_irq(); }
    void on_exti_fg() {
        gd32::reg(gd32::EXTI_PD) = 1u << 11;  // clear line 11 (PE11)
        hub_.on_fg_edge();
    }

    SysTickClock clock;

private:
    PwmTimer tim0_{gd32::TIMER0, true};
    Adc adc_{gd32::ADC1, gd32::APB2_ADC1};  // stock device "adc1" = ADC1
    Spi spi_{gd32::SPI1};
    Usart usart_{gd32::USART1};
    SoftI2c aht_bus_{pins::kAhtScl, pins::kAhtSda};
    SoftI2c rfid_bus0_{pins::kRfid0Scl, pins::kRfid0Sda};
    SoftI2c rfid_bus1_{pins::kRfid1Scl, pins::kRfid1Sda};

    Feeders feeders_{adc_};
    Hub hub_{tim0_};
    Sensors sensors_;
    Mt6826Odometer odometer_{spi_};
    Aht30 env_{aht_bus_};
    Leds leds_;
    Rs485 link_{usart_};
    Fm17622Rfid rfid_{rfid_bus0_, rfid_bus1_, clock};
    FlashSettings settings_;
    McuSystem system_{clock};
};

}  // namespace cfs::board
