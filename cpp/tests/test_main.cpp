// SPDX-License-Identifier: Apache-2.0
// Host tests: protocol codec, dispatcher contract and filament-path behavior on
// the simulator. No external framework.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "cfs/dispatcher.hpp"
#include "cfs/feeder.hpp"
#include "cfs/frame.hpp"
#include "cfs/preload.hpp"
#include "cfs/print_tracker.hpp"
#include "cfs/spool.hpp"
#include "sim.hpp"

using namespace cfs;

namespace {

int g_failed = 0;
int g_checks = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failed;                                                          \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                        \
    } while (0)

std::vector<uint8_t> frame(uint8_t addr, uint8_t cmd, std::vector<uint8_t> payload,
                           uint8_t header = kHeaderBoxRequest) {
    std::vector<uint8_t> out(payload.size() + 6);
    out.resize(encode(addr, header, cmd, payload.data(), payload.size(), out.data(), out.size()));
    return out;
}

// One simulated CFS with every component wired like the firmware.
struct Rig {
    sim::World w;
    Hardware hw = w.hardware();
    Config cfg;
    Box box;
    Feeder feeder{hw, box, cfg};
    PrintTracker print{hw, box, cfg};
    RfidScanner scanner{hw, box, feeder, cfg};
    Preload preload{hw, box, feeder, scanner, cfg};
    Dispatcher disp{hw, box, cfg, feeder, print, 1};

    Rig() {
        disp.set_scanner(&scanner);
        preload.init();
    }

    // Sends a request and returns the last reply frame.
    std::vector<uint8_t> request(uint8_t cmd, std::vector<uint8_t> payload, uint8_t addr = 1) {
        w.tx.clear();
        w.inject(w.now_ms(), frame(addr, cmd, payload));
        disp.poll();
        auto frames = w.sent_frames();
        return frames.empty() ? std::vector<uint8_t>{} : frames.back();
    }
};

uint8_t status_of(const std::vector<uint8_t>& f) { return f.size() > 3 ? f[3] : 0xEE; }

void test_crc_vectors() {
    const uint8_t a[] = {0x04, 0x00, 0xF0, 0x00};
    CHECK(crc8(a, 4) == 0x4C);
    const uint8_t b[] = {0x03, 0xFF, 0x14};
    CHECK(crc8(b, 3) == 0x06);
}

void test_receiver() {
    Receiver rx(1);
    Frame f;
    auto bytes = frame(1, 0x14, {});
    CHECK(bytes == std::vector<uint8_t>({0xF7, 0x01, 0x03, 0xFF, 0x14, 0x06}));
    Receiver::Result r = Receiver::Result::None;
    for (uint8_t b : bytes) r = rx.push(b, 0, f);
    CHECK(r == Receiver::Result::Frame && f.cmd == 0x14 && f.len == 0);

    // Other address: ignored.
    auto other = frame(2, 0x14, {});
    for (uint8_t b : other) r = rx.push(b, 10, f);
    CHECK(r == Receiver::Result::None);

    // Corrupted CRC.
    bytes.back() ^= 1;
    for (uint8_t b : bytes) r = rx.push(b, 20, f);
    CHECK(r == Receiver::Result::BadCrc);

    // Inter-byte timeout drops a partial frame.
    auto good = frame(1, 0x05, {});
    for (size_t i = 0; i < 3; ++i) rx.push(good[i], 100, f);
    r = Receiver::Result::None;
    for (size_t i = 0; i < good.size(); ++i) r = rx.push(good[i], 300 + i, f);
    CHECK(r == Receiver::Result::Frame && f.cmd == 0x05);
}

void test_dispatcher_contract() {
    Rig rig;
    // Wrong length → INVALID_PARAM.
    auto r = rig.request(0x0A, {0x00});
    CHECK(status_of(r) == 0x01 && r[4] == 0x0A);
    // Unknown command → INVALID_PARAM.
    CHECK(status_of(rig.request(0x7E, {})) == 0x01);
    // Silent broadcast: no reply at all.
    rig.request(0x55, {7}, kAddrSilentBroadcast);
    CHECK(rig.w.tx.empty());
    // Echo.
    r = rig.request(0x55, {7});
    CHECK(status_of(r) == 0 && r[5] == 8);
    // Version: "153" + serial.
    r = rig.request(0x14, {});
    CHECK(r.size() == 28 && std::memcmp(&r[5], "153", 3) == 0);
    // BOX_STATE: 6 bytes, temp/hum, IDLE.
    r = rig.request(0x0A, {});
    CHECK(r[2] == 9 && r[5] == 29 && r[6] == 38 && r[8] == 0);
    // Bad slot mask.
    CHECK(status_of(rig.request(0x10, {0x03, 0, 0})) == 0x01);
    // 0x52 replies with opcode 0x53.
    r = rig.request(0x52, {});
    CHECK(r[4] == 0x53 && r[5] == 29 && r[6] == 38);
    // Mode unchanged → BUSY; changed → OK and scheduled reset.
    CHECK(status_of(rig.request(0x50, {0})) == 0x03);
    CHECK(status_of(rig.request(0x50, {2})) == 0x00 && rig.w.reset_at > 0);
}

void test_slot_events_in_box_state() {
    Rig rig;
    rig.box.read_on_insert = false;
    rig.w.insert(2);
    for (int i = 0; i < 10; ++i) {
        rig.preload.sample();
        rig.w.sleep_ms(20);
    }
    auto r = rig.request(0x0A, {});
    CHECK(status_of(r) == 0x30 && r[2] == 7 && r[7] == 1);
    // Events are consumed by the read.
    CHECK(status_of(rig.request(0x0A, {})) == 0x00);
}

void test_load_sequence() {
    Rig rig;
    rig.w.insert(0);
    auto r = rig.request(0x10, {0x01, 0, 0});
    CHECK(status_of(r) == 0x00 && r[5] == 0);
    CHECK(rig.w.hub_present(0));
    CHECK(rig.box.state() == BoxState::Idle);

    // Stage 4 with the host polling stage 5 and ending with stage 6.
    const uint32_t t = rig.w.now_ms();
    rig.w.tx.clear();
    rig.w.inject(t, frame(1, 0x10, {0x01, 4, 0}));
    rig.w.inject(t + 500, frame(1, 0x10, {0x01, 5, 0}));
    rig.w.inject(t + 9000, frame(1, 0x10, {0x01, 6, 0}));
    rig.disp.poll();
    auto frames = rig.w.sent_frames();
    CHECK(frames.size() == 3);
    if (frames.size() == 3) {
        CHECK(status_of(frames[0]) == 0);              // immediate OK
        CHECK(status_of(frames[1]) == 0 && frames[1][2] == 7);  // progress with float
        CHECK(status_of(frames[2]) == 0);              // buffer full
    }
    CHECK(rig.w.buffer() == BufferState::Full);

    // Stage 7: 3 mm into the extruder (the sim stops at the toolhead).
    r = rig.request(0x10, {0x01, 7, 3});
    CHECK(status_of(r) == 0x00 || status_of(r) == 0x0E);
}

void test_load_errors() {
    {
        Rig rig;  // nothing inserted
        CHECK(status_of(rig.request(0x10, {0x02, 0, 0})) == to_u8(Status::SlotEmpty));
        CHECK(rig.box.state() == BoxState::Error);
    }
    {
        Rig rig;
        rig.w.insert(1);
        rig.w.jam_before_hub = true;
        CHECK(status_of(rig.request(0x10, {0x02, 0, 0})) == to_u8(Status::Stage0SensorTimeout));
    }
    {
        Rig rig;
        rig.box.enter(BoxState::Print);
        rig.w.insert(1);
        CHECK(status_of(rig.request(0x10, {0x02, 0, 0})) == to_u8(Status::Busy));
        CHECK(rig.box.state() == BoxState::Print);
    }
}

void test_unload() {
    Rig rig;
    rig.w.insert(3);
    CHECK(status_of(rig.request(0x10, {0x08, 0, 0})) == 0);
    CHECK(rig.w.hub_present(3));
    auto r = rig.request(0x11, {0x08, 1});
    CHECK(status_of(r) == 0);
    CHECK(!rig.w.hub_present(3) && rig.w.inlet_present(3));
    // Nothing loaded anywhere.
    Rig empty;
    CHECK(status_of(empty.request(0x11, {0x00, 1})) == to_u8(Status::UnloadAllEmpty));
}

void test_spool_estimate() {
    SpoolGeometry g;
    auto e = estimate_remaining(3.14f * 96.0f, g);
    CHECK(e.linear_pct == 1 && e.area_pct == 1 && e.reliable);
    e = estimate_remaining(3.14f * 172.0f, g);
    CHECK(e.linear_pct == 100 && e.area_pct == 100);
    e = estimate_remaining(3.14f * 134.0f, g);  // half way in diameter
    CHECK(e.linear_pct == 50 && e.area_pct == 43);
    e = estimate_remaining(200.0f, g);  // below the core
    CHECK(!e.reliable && e.area_pct == 1);
    CHECK(estimate_remaining(-1.0f, g).area_pct == 0xFF);
}

void test_rfid_scan() {
    Rig rig;
    rig.w.insert(1);
    rig.w.slots[1].spool_circ_mm = 3.14f * 134.0f;
    auto r = rig.scanner.scan(1);
    CHECK(r.record_read);
    CHECK(std::fabs(r.per_rev_mm - 3.14f * 134.0f) < 20.0f);
    CHECK(rig.box.rfid[1].remaining_pct >= 38 && rig.box.rfid[1].remaining_pct <= 48);
    CHECK(!rig.w.hub_present(1));  // parked
    auto rem = rig.request(0x03, {0x0F});
    CHECK(rem[5] == 0xFF && rem[6] == rig.box.rfid[1].remaining_pct);
}

void test_print_refill() {
    Rig rig;
    rig.w.insert(0);
    CHECK(status_of(rig.request(0x10, {0x01, 0, 0})) == 0);
    CHECK(rig.feeder.stage1_hub_until_stop() == Status::Ok);
    CHECK(status_of(rig.request(0x04, {0x01, 0})) == 0);
    CHECK(rig.box.state() == BoxState::Print);
    rig.w.consume(5.0f);
    // Buffer switch interrupts, as the GD32 EXTI lines would deliver them.
    BufferState last = rig.w.buffer();
    int sessions = 0;
    rig.w.tick_hook = [&] {
        const BufferState now = rig.w.buffer();
        if (now == last) return;
        if (now == BufferState::Empty) {
            ++sessions;
            rig.print.on_buffer_empty();
        }
        if (now == BufferState::Full) rig.print.on_buffer_full();
        last = now;
    };
    for (int i = 0; i < 3000; ++i) {  // 30 s of printing; print_check thread in 10 ms steps
        rig.print.service();
        rig.w.sleep_ms(10);
    }
    rig.w.tick_hook = nullptr;
    CHECK(sessions > 0);
    CHECK(rig.box.state() == BoxState::Print);
    auto r = rig.request(0x0A, {});
    CHECK(status_of(r) == 0 && r[8] == 2);
    CHECK(status_of(rig.request(0x04, {0x00, 1})) == 0);
    CHECK(rig.box.state() == BoxState::Idle);
}

void test_refill_no_motion() {
    Rig rig;
    rig.w.insert(0);
    CHECK(status_of(rig.request(0x10, {0x01, 0, 0})) == 0);
    CHECK(status_of(rig.request(0x04, {0x01, 0})) == 0);
    rig.w.hub_blocked = true;
    rig.print.on_buffer_empty();  // buffer emptied, but the hub cannot move
    rig.print.service();
    CHECK(rig.box.state() == BoxState::Error);
    CHECK(rig.box.error() == to_u8(Status::BufferRefillNoMotion));
    CHECK(status_of(rig.request(0x0A, {})) == to_u8(Status::BufferRefillNoMotion));
}

void test_runout_status() {
    Rig rig;
    rig.w.insert(2);
    CHECK(status_of(rig.request(0x04, {0x04, 0})) == 0);
    rig.w.remove(2);
    auto r = rig.request(0x0A, {});
    CHECK(status_of(r) == to_u8(Status::Runout));
}

struct Test {
    const char* name;
    void (*fn)();
};

}  // namespace

int main() {
    const Test tests[] = {
        {"crc_vectors", test_crc_vectors},
        {"receiver", test_receiver},
        {"dispatcher_contract", test_dispatcher_contract},
        {"slot_events_in_box_state", test_slot_events_in_box_state},
        {"load_sequence", test_load_sequence},
        {"load_errors", test_load_errors},
        {"unload", test_unload},
        {"spool_estimate", test_spool_estimate},
        {"rfid_scan", test_rfid_scan},
        {"print_refill", test_print_refill},
        {"refill_no_motion", test_refill_no_motion},
        {"runout_status", test_runout_status},
    };
    for (const Test& t : tests) {
        const int before = g_failed;
        t.fn();
        std::printf("%-28s %s\n", t.name, g_failed == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
