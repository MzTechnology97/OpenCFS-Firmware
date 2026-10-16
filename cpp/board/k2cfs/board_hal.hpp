// SPDX-License-Identifier: Apache-2.0
// cfs::Hardware implementation for the K2 CFS controller board (GD32F303).
// Bare-metal: the application calls the tick functions from SysTick / a 1 ms
// loop and the IRQ hooks from the vector table. NOT qualified on hardware.
#pragma once

#include <cstdint>

#include "board_io.hpp"
#include "fm17622.hpp"
#include "tm1621.hpp"
#include "vendor_decoders.hpp"
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
    // Both inputs of every driver commanded high (brake), read back from GPIOD OCTL.
    bool outputs_idle() override;
    void tick_20ms();  // current monitor
    float last_volts(int slot) const { return avg_[slot]; }

private:
    Adc& adc_;
    bool running_[4] = {};
    bool blocked_[4] = {};
    bool moved_since_idle_check_ = false;
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
    // Speed 0, BRAKE commanded high, PWM compare at the inactive level and no FG
    // pulse since the previous call.
    bool outputs_idle() override;
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
    uint32_t idle_check_pulses_ = 0;
    bool moved_since_idle_check_ = false;
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

// 24C16 (2 KiB) on software I2C PE5/PE6, read-only here. Stock map: serial number
// 0x000..0x012 (read at boot, 0x080032E8), secure-backend key 0x060..0x06F,
// "motor block info" record 0x080..0x0AB (written by is_reverse_detooth),
// self-test word 0x7F0. OpenCFS reads the serial and writes nothing.
class Eeprom24c16 {
public:
    explicit Eeprom24c16(SoftI2c& bus) : bus_(bus) {}
    void init() { bus_.init(); }
    bool read(uint16_t addr, uint8_t* out, size_t len);

private:
    SoftI2c& bus_;
};

class McuSystem : public System {
public:
    explicit McuSystem(Clock& clock) : clock_(clock) {}
    // Reads the production serial number from the EEPROM (stock 0x080032E8).
    void load_serial(Eeprom24c16& eeprom);
    void reset_after_ms(uint32_t ms) override;
    // The EEPROM serial when it is valid ASCII, else one derived from the MCU UID.
    void serial_number(char out[19]) override;
    void unique_id(uint8_t out[12]) override;
    void set_aux_output(bool on) override { pin_write(pins::kLcdBacklight, on); }

private:
    Clock& clock_;
    char serial_[19] = {};
    bool serial_valid_ = false;
};

// Everything wired together.
class Board {
public:
    Board();
    void init();
    Hardware hardware();
    // Stock hw_flag: true when a strap reads high (FM15L013 secure element fitted).
    // OpenCFS reads tags through the FM17622 path either way; secure-element boards
    // are untested.
    bool secure_element_fitted() const { return secure_element_; }
    // 1 ms housekeeping for millisecond `now`: monitors, speed loop, odometer, LEDs.
    void tick_1ms(uint32_t now);
    // Stock `lcd` thread, 1 s: refresh temperature/humidity (sleeps ~80 ms) and
    // show them with the box index on the front display.
    void update_environment(uint8_t box_index) {
        env_.update(clock);
        lcd_.show(env_.temperature_c(), env_.humidity_pct(), box_index);
    }
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
    SoftI2c eeprom_bus_{pins::kEepromScl, pins::kEepromSda};
    Eeprom24c16 eeprom_{eeprom_bus_};
    SoftI2c rfid_bus0_{pins::kRfid0Scl, pins::kRfid0Sda};
    SoftI2c rfid_bus1_{pins::kRfid1Scl, pins::kRfid1Sda};

    Feeders feeders_{adc_};
    Hub hub_{tim0_};
    Sensors sensors_;
    Mt6826Odometer odometer_{spi_};
    Aht30 env_{aht_bus_};
    Leds leds_;
    Tm1621 lcd_;
    Rs485 link_{usart_};
    VendorDecoders decoders_;
    tags::TagPipeline pipeline_{decoders_.list(), VendorDecoders::kCount};
    Fm17622Rfid rfid_{rfid_bus0_, rfid_bus1_, clock, pipeline_};
    FlashSettings settings_;
    McuSystem system_{clock};
    bool secure_element_ = false;
};

}  // namespace cfs::board
