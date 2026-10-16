// SPDX-License-Identifier: Apache-2.0
#include "bench.hpp"

namespace cfs::board {

namespace {

uint16_t get_u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

void put_u16(uint8_t* p, uint32_t v) {
    if (v > 0xFFFF) v = 0xFFFF;
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void put_i16(uint8_t* p, float v) {
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    const auto i = static_cast<int16_t>(v);
    put_u16(p, static_cast<uint16_t>(i));
}

float abs_f(float v) { return v < 0.0f ? -v : v; }

}  // namespace

BenchCommands::BenchCommands(BoardApp& app, void (*raise_fault)()) : app_(app), raise_fault_(raise_fault) {}

void BenchCommands::answer(Status st, const uint8_t* data, size_t len) {
    app_.app().disp.reply_current(to_u8(Cmd::Bench), st, data, len);
}

void BenchCommands::disarm() {
    Board& b = app_.board();
    b.feeders().set_allowed(Feeders::kNoSlot);
    b.feeders().stop_all();
    b.hub().set_allowed(false);
    b.hub().set_output_cap(255);
    level_ = BuildInfo::SensorsOnly;
}

bool BenchCommands::outputs_at_rest() {
    Board& b = app_.board();
    // Both calls also clear their "moved since the last check" flags.
    const bool feeders = b.feeders().outputs_idle();
    const bool hub = b.hub().outputs_idle();
    return feeders && hub && app_.app().box.state() == BoxState::Idle;
}

// Called for every frame to this box from the foreground loop.
bool BenchCommands::handle(const Frame& f) {
    last_frame_ = app_.board().clock.now_ms();
    if (f.cmd != to_u8(Cmd::Bench)) return false;
    if (f.len < 1) {
        answer(Status::InvalidParam);
        return true;
    }
    const uint8_t* p = f.payload;
    const auto need = [&](uint8_t n) {
        if (f.len == n) return true;
        answer(Status::InvalidParam);
        return false;
    };
    switch (p[0]) {
    case SubStatus:
        if (need(1)) {
            const uint32_t now = app_.board().clock.now_ms();
            const uint32_t left = armed() ? (armed_until_ - now) / 1000u : 0;
            const uint8_t v[3] = {level_, last_result_, static_cast<uint8_t>(left > 255 ? 255 : left)};
            answer(Status::Ok, v, sizeof v);
        }
        break;
    case SubArm:
        if (need(8)) arm(p + 1);
        break;
    case SubDisarm:
        if (need(1)) {
            disarm();
            const uint8_t v = level_;
            answer(Status::Ok, &v, 1);
        }
        break;
    case SubFeederJog:
        if (need(5)) feeder_jog(p + 1);
        break;
    case SubHubJog:
        if (need(5)) hub_jog(p + 1);
        break;
    case SubTagRead:
        if (need(2)) tag_read(p[1]);
        break;
    case SubFaultTest:
        if (need(2)) fault_test(p[1]);
        break;
    default:
        answer(Status::InvalidParam);
        break;
    }
    return true;
}

void BenchCommands::arm(const uint8_t* p) {
    uint8_t uid[12];
    app_.board().hardware().system.unique_id(uid);
    bool ok = p[0] == 'A' && p[1] == 'R' && p[2] == 'M';
    for (int i = 0; i < 4; ++i) ok = ok && p[3 + i] == uid[8 + i];
    if (!ok) {
        answer(Status::InvalidParam);
        return;
    }
    if (!outputs_at_rest()) {
        answer(Status::Busy);
        return;
    }
    const uint32_t now = app_.board().clock.now_ms();
    level_ = BuildInfo::ActuatorTest;
    armed_until_ = now + kArmMs;
    last_frame_ = now;
    last_result_ = Done;
    const uint8_t v = level_;
    answer(Status::Ok, &v, 1);
}

// Watchdog: leave ACTUATOR_TEST when the host is gone or the arm time is over.
void BenchCommands::service(uint32_t now_ms) {
    if (!armed()) return;
    if (static_cast<int32_t>(now_ms - armed_until_) >= 0 || now_ms - last_frame_ > kLinkLossMs) {
        disarm();
    }
}

void BenchCommands::feeder_jog(const uint8_t* p) {
    const uint8_t slot = p[0], dir = p[1];
    const uint32_t ms = get_u16(p + 2);
    if (!armed()) {
        answer(Status::MotionLocked);
        return;
    }
    if (slot >= kSlotCount || dir > 1 || ms == 0 || ms > kFeederJogMaxMs) {
        answer(Status::InvalidParam);
        return;
    }
    if (!outputs_at_rest()) {
        answer(Status::Busy);
        return;
    }
    Board& b = app_.board();
    Feeders& fd = b.feeders();
    const float odo0 = b.odometer().position_mm();
    const uint32_t start = b.clock.now_ms();
    Result r = Done;
    fd.set_allowed(slot);  // this slot only; the hub stays locked
    fd.run(slot, dir == 0 ? Dir::Forward : Dir::Reverse, 255);
    while (b.clock.now_ms() - start < ms) {
        b.clock.sleep_ms(kPollMs);  // board ticks keep the current monitor running
        if (fd.blocked(slot)) { r = Stalled; break; }
        if (abs_f(b.odometer().position_mm() - odo0) > kTravelLimitMm) { r = Travel; break; }
    }
    fd.stop(slot);
    fd.set_allowed(Feeders::kNoSlot);
    const uint32_t elapsed = b.clock.now_ms() - start;
    last_result_ = r;
    if (r != Done) disarm();
    uint8_t v[7];
    v[0] = r;
    put_u16(v + 1, elapsed);
    put_u16(v + 3, static_cast<uint32_t>(fd.last_volts(slot) * 1000.0f));
    put_i16(v + 5, (b.odometer().position_mm() - odo0) * 10.0f);
    answer(Status::Ok, v, sizeof v);
}

void BenchCommands::hub_jog(const uint8_t* p) {
    const uint8_t dir = p[0], speed = p[1];
    const uint32_t ms = get_u16(p + 2);
    if (!armed()) {
        answer(Status::MotionLocked);
        return;
    }
    if (dir > 1 || speed == 0 || speed > kHubJogMaxSpeed || ms == 0 || ms > kHubJogMaxMs) {
        answer(Status::InvalidParam);
        return;
    }
    if (!outputs_at_rest()) {
        answer(Status::Busy);
        return;
    }
    Board& b = app_.board();
    Hub& hub = b.hub();
    const float odo0 = b.odometer().position_mm();
    const uint32_t fg0 = hub.fg_pulses();
    const uint32_t start = b.clock.now_ms();
    Result r = Done;
    hub.set_output_cap(kHubJogPwmCap);
    hub.set_allowed(true);  // the feeders stay locked
    hub.run(dir == 0 ? Dir::Forward : Dir::Reverse, speed);
    while (b.clock.now_ms() - start < ms) {
        b.clock.sleep_ms(kPollMs);
        if (hub.overload()) { r = Overload; break; }
        if (hub.blocked()) { r = Stalled; break; }
        if (abs_f(b.odometer().position_mm() - odo0) > kTravelLimitMm) { r = Travel; break; }
    }
    const float rps = hub.rps();
    hub.stop();
    hub.set_allowed(false);
    hub.set_output_cap(255);
    const uint32_t elapsed = b.clock.now_ms() - start;
    last_result_ = r;
    if (r != Done) disarm();
    uint8_t v[9];
    v[0] = r;
    put_u16(v + 1, elapsed);
    put_u16(v + 3, hub.fg_pulses() - fg0);
    put_u16(v + 5, static_cast<uint32_t>(rps * 10.0f));
    put_i16(v + 7, (b.odometer().position_mm() - odo0) * 10.0f);
    answer(Status::Ok, v, sizeof v);
}

// Read-only in either level: the reader sees whatever tag is in its field now.
// The decoded readout is then available through 0x60, as after a normal read.
void BenchCommands::tag_read(uint8_t slot) {
    if (slot >= kSlotCount) {
        answer(Status::InvalidParam);
        return;
    }
    RfidReader& rfid = app_.board().rfid();
    TagUid uid;
    TagRecord rec;
    const bool have_uid = rfid.read_uid(slot, uid);
    const bool have_record = have_uid && rfid.read_record(slot, rec);
    uint8_t v[2 + sizeof uid.bytes] = {};
    v[0] = static_cast<uint8_t>((have_uid ? 1 : 0) | (have_record ? 2 : 0));
    v[1] = have_uid ? uid.len : 0;
    for (uint8_t i = 0; have_uid && i < uid.len && i < sizeof uid.bytes; ++i) v[2 + i] = uid.bytes[i];
    answer(Status::Ok, v, sizeof v);
}

// Stage 3.4: a real CPU fault while a feeder runs must brake everything.
void BenchCommands::fault_test(uint8_t slot) {
    if (!armed()) {
        answer(Status::MotionLocked);
        return;
    }
    if (slot >= kSlotCount) {
        answer(Status::InvalidParam);
        return;
    }
    if (!outputs_at_rest()) {
        answer(Status::Busy);
        return;
    }
    answer(Status::Ok);
    Board& b = app_.board();
    b.feeders().set_allowed(slot);
    b.feeders().run(slot, Dir::Forward, 255);
    b.clock.sleep_ms(kFaultTestRunMs);
    raise_fault_();
    disarm();  // only if raise_fault returned, which it must not
}

}  // namespace cfs::board
