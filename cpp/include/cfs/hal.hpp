// SPDX-License-Identifier: Apache-2.0
// Hardware abstraction used by the CFS logic. A board port (GD32 + RTOS) or the
// host simulator (src/sim) implements these interfaces.
#pragma once

#include <cstddef>
#include <cstdint>

#include "cfs/protocol.hpp"

namespace cfs {

enum class Dir : uint8_t { Forward = 0, Reverse = 1 };

class Clock {
public:
    virtual ~Clock() = default;
    virtual uint32_t now_ms() = 0;
    // Blocking delay; the simulator advances its physics model here.
    virtual void sleep_ms(uint32_t ms) = 0;
};

// Four brushed slot feeders (AT8236). speed 0..255.
class FeederDriver {
public:
    virtual ~FeederDriver() = default;
    virtual void run(int slot, Dir dir, uint8_t speed) = 0;
    virtual void stop(int slot) = 0;
    virtual bool blocked(int slot) = 0;
    // True when every feeder's commanded outputs are at brake/off (read back from
    // the output registers where the board can) and no run() happened since the
    // previous call. Sticky on purpose: the runtime-config interlock may not be
    // polled while a blocking motion routine runs.
    virtual bool outputs_idle() = 0;
    void stop_all() {
        for (int s = 0; s < kSlotCount; ++s) {
            stop(s);
        }
    }
};

// Hub BLDC (MS3791) with brake.
class HubDriver {
public:
    virtual ~HubDriver() = default;
    virtual void run(Dir dir, uint8_t speed) = 0;
    virtual void stop() = 0;
    virtual bool blocked() = 0;
    virtual bool overload() = 0;
    // Last command (speed 0 = stopped), needed to resume after a detooth.
    virtual Dir current_dir() = 0;
    virtual uint8_t current_speed() = 0;
    // True when the hub is commanded off and braked, with no run() and no FG pulse
    // since the previous call (runtime-config interlock).
    virtual bool outputs_idle() = 0;
};

// Hub odometer in mm. Feeding toward the toolhead makes the value decrease.
class Odometer {
public:
    virtual ~Odometer() = default;
    virtual float position_mm() = 0;
    virtual void reset() = 0;
};

// Raw (unfiltered) filament path sensors.
class PathSensors {
public:
    virtual ~PathSensors() = default;
    virtual bool inlet_present(int slot) = 0;
    virtual bool hub_present(int slot) = 0;
    virtual BufferState buffer() = 0;
};

class Environment {
public:
    virtual ~Environment() = default;
    virtual int8_t temperature_c() = 0;
    virtual uint8_t humidity_pct() = 0;
};

enum class IndicatorMode : uint8_t { Off = 0, Idle = 1, Loaded = 2, Error = 3 };

class Indicators {
public:
    virtual ~Indicators() = default;
    virtual void set(int slot, uint8_t mode) = 0;
};

struct TagUid {
    uint8_t len = 0;
    uint8_t bytes[10] = {};
    bool operator==(const TagUid& o) const {
        if (len != o.len) return false;
        for (uint8_t i = 0; i < len; ++i) {
            if (bytes[i] != o.bytes[i]) return false;
        }
        return true;
    }
};

constexpr size_t kTagRecordLen = 40;

struct TagRecord {
    char text[kTagRecordLen] = {};  // Creality 40-character ASCII record
};

namespace tags {
struct TagReadout;
}

// RFID front-end for the antenna that faces a slot's spool.
class RfidReader {
public:
    enum class Detect : uint8_t { None, One, Multiple };
    virtual ~RfidReader() = default;
    virtual Detect detect(int slot) = 0;
    virtual bool read_uid(int slot, TagUid& uid) = 0;
    virtual bool read_record(int slot, TagRecord& rec) = 0;
    // Multi-vendor readout of the last read_record() (docs/11); nullptr if none.
    virtual const tags::TagReadout* last_tag(int slot) {
        (void)slot;
        return nullptr;
    }
};

// Persistent settings (stock: key/value database in internal flash).
class Settings {
public:
    virtual ~Settings() = default;
    virtual uint8_t mode() = 0;
    virtual void set_mode(uint8_t mode) = 0;
};

// Link to the host: raw bytes in and out.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void write(const uint8_t* data, size_t len) = 0;
    // Returns false when no byte is available right now.
    virtual bool read_byte(uint8_t& b) = 0;
};

class System {
public:
    virtual ~System() = default;
    virtual void reset_after_ms(uint32_t ms) = 0;
    // 19-character serial number, not NUL terminated.
    virtual void serial_number(char out[19]) = 0;
    // 12-byte MCU unique ID (stock: 0x1FFFF7E8), used as the RS-485 UniID.
    virtual void unique_id(uint8_t out[12]) = 0;
    // Command 0x30: front LCD backlight (stock: PB0, RT pin 16).
    virtual void set_aux_output(bool on) = 0;
};

struct Hardware {
    Clock& clock;
    FeederDriver& feeders;
    HubDriver& hub;
    Odometer& odometer;
    PathSensors& sensors;
    Environment& env;
    Indicators& indicators;
    RfidReader& rfid;
    Settings& settings;
    Transport& link;
    System& system;
};

}  // namespace cfs
