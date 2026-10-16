// SPDX-License-Identifier: Apache-2.0
// Test commands of the BENCH build (docs/13-application.md section 5), command
// 0x7B. Compiled into the BENCH image only: the release and INERT images do not
// contain this code, so no host setting can enable it there.
//
// Two levels. SENSORS_ONLY at every start: sensor and tag reads, every actuator
// locked. ACTUATOR_TEST only after an explicit ARM addressed to this box with its
// unique ID: one motor at a time, bounded jogs, back to SENSORS_ONLY on a stall,
// an overload, a travel limit, DISARM, 5 s without a frame to this box, or 120 s
// after ARM. Nothing is stored: a reset always starts at SENSORS_ONLY.
#pragma once

#include <cstddef>
#include <cstdint>

#include "board_app.hpp"
#include "cfs/build_info.hpp"
#include "cfs/dispatcher.hpp"

namespace cfs::board {

class BenchCommands : public Dispatcher::Extension, public BoardApp::Service {
public:
    // Sub-commands of 0x7B (payload[0]).
    enum Sub : uint8_t {
        SubStatus = 0x00,     // -> level, last result, seconds left armed
        SubArm = 0x01,        // 'A' 'R' 'M' uid[8..11] -> level
        SubDisarm = 0x02,     // -> level
        SubFeederJog = 0x10,  // slot, dir (0 fwd, 1 rev), ms u16 LE
        SubHubJog = 0x11,     // dir, speed, ms u16 LE
        SubTagRead = 0x20,    // slot: read the tag in place, no motion
        SubFaultTest = 0x30,  // slot: run that feeder, then raise a real fault
    };
    enum Result : uint8_t { Done = 0, Stalled = 1, Overload = 2, Travel = 3 };

    static constexpr uint32_t kFeederJogMaxMs = 500;
    static constexpr uint32_t kHubJogMaxMs = 1000;
    static constexpr uint8_t kHubJogMaxSpeed = 64;  // of 255
    static constexpr uint8_t kHubJogPwmCap = 96;    // of 255
    static constexpr float kTravelLimitMm = 30.0f;  // odometer, per jog
    static constexpr uint32_t kArmMs = 120000;
    static constexpr uint32_t kLinkLossMs = 5000;
    static constexpr uint32_t kFaultTestRunMs = 50;
    static constexpr uint32_t kPollMs = 10;

    // `raise_fault` must not return (the image raises a real CPU fault).
    BenchCommands(BoardApp& app, void (*raise_fault)());

    bool handle(const Frame& f) override;
    void service(uint32_t now_ms) override;

    const volatile uint8_t* level_ptr() const { return &level_; }
    uint8_t level() const { return level_; }

private:
    void answer(Status st, const uint8_t* data = nullptr, size_t len = 0);
    void disarm();
    bool armed() const { return level_ == BuildInfo::ActuatorTest; }
    bool outputs_at_rest();
    void feeder_jog(const uint8_t* p);
    void hub_jog(const uint8_t* p);
    void tag_read(uint8_t slot);
    void fault_test(uint8_t slot);
    void arm(const uint8_t* p);

    BoardApp& app_;
    void (*raise_fault_)();
    volatile uint8_t level_ = BuildInfo::SensorsOnly;
    uint8_t last_result_ = Done;
    uint32_t armed_until_ = 0;
    uint32_t last_frame_ = 0;
};

}  // namespace cfs::board
