// SPDX-License-Identifier: Apache-2.0
// Host tests: protocol codec, dispatcher contract and filament-path behavior on
// the simulator. No external framework.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "cfs/aes128.hpp"
#include "cfs/app.hpp"
#include "cfs/creality_tag.hpp"
#include "cfs/dispatcher.hpp"
#include "cfs/feeder.hpp"
#include "cfs/frame.hpp"
#include "cfs/param_store.hpp"
#include "cfs/preload.hpp"
#include "cfs/print_tracker.hpp"
#include "cfs/runtime_config.hpp"
#include "cfs/spool.hpp"
#include "cfs/sha256.hpp"
#include "cfs/supervisor.hpp"
#include "cfs/tags/decoders.hpp"
#include "cfs/usage.hpp"
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
    UsageManager usage{hw, box};
    MotorSupervisor supervisor{hw, box, cfg};
    Dispatcher disp;

    explicit Rig(uint8_t address = 1) : disp(hw, box, cfg, feeder, print, address) {
        disp.set_scanner(&scanner);
        disp.set_preload(&preload);
        disp.set_usage(&usage);
        scanner.set_usage(&usage);
        preload.set_usage(&usage);
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
    // Mode unchanged → BUSY; factory modes refused; back to standard from a mode a
    // stock firmware stored → OK and scheduled reset.
    CHECK(status_of(rig.request(0x50, {0})) == 0x03);
    CHECK(status_of(rig.request(0x50, {2})) == 0x01 && rig.w.reset_at == 0);
    rig.w.set_mode(2);
    CHECK(status_of(rig.request(0x50, {0})) == 0x00 && rig.w.reset_at > 0 && rig.w.mode() == 0);
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
    auto bad = frame(1, 0x10, {0x01, 5, 0});
    bad.back() ^= 0x55;
    rig.w.inject(t + 1000, bad);                            // bad CRC: dropped
    rig.w.inject(t + 1500, frame(kAddrBoxBroadcast, 0x10, {0x01, 5, 0}));  // broadcast: dropped
    rig.w.inject(t + 2000, frame(1, 0x57, {0x00}));         // other command: dropped
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


void test_auto_address() {
    Rig rig(AutoAddress::kUnassigned);
    // Discovery: the unassigned box answers once, after a random 0..1000 ms delay.
    rig.w.tx.clear();
    rig.w.inject(rig.w.now_ms(), frame(0xFE, 0xA1, {0xFE, 0xFE}, 0x00));
    rig.disp.poll();
    for (int i = 0; i < 1100 && rig.w.sent_frames().empty(); ++i) {
        rig.w.sleep_ms(1);
        rig.disp.poll();
    }
    auto fr = rig.w.sent_frames();
    CHECK(fr.size() == 1);
    if (fr.size() == 1) {
        CHECK(fr[0][1] == 0xFE && fr[0][3] == 0x00 && fr[0][4] == 0xA1 && fr[0][2] == 17);
        CHECK(fr[0][5] == 1 && fr[0][6] == 0 && fr[0][7] == 0x31 && fr[0][18] == 0x3C);
    }
    // Assignment with the matching UID: three replies from the new address.
    std::vector<uint8_t> assign = {0x02};
    for (int i = 0; i < 12; ++i) assign.push_back(rig.w.uid[i]);
    rig.w.tx.clear();
    rig.w.inject(rig.w.now_ms(), frame(0xFE, 0xA0, assign, 0x00));
    rig.disp.poll();
    fr = rig.w.sent_frames();
    CHECK(fr.size() == 3 && fr[0][1] == 0x02 && rig.disp.address() == 0x02);
    // A wrong UID changes nothing.
    assign[5] ^= 0xFF;
    assign[0] = 0x03;
    rig.w.tx.clear();
    rig.w.inject(rig.w.now_ms(), frame(0xFE, 0xA0, assign, 0x00));
    rig.disp.poll();
    CHECK(rig.w.tx.empty() && rig.disp.address() == 0x02);
    // Once assigned, discovery is no longer answered; online check is.
    rig.w.inject(rig.w.now_ms(), frame(0xFE, 0xA1, {0xFE, 0xFE}, 0x00));
    rig.disp.poll();
    rig.w.sleep_ms(1100);
    rig.disp.poll();
    CHECK(rig.w.tx.empty());
    auto r = rig.request(0xA2, {}, 0x02);
    CHECK(r.size() == 20 && r[1] == 0x02 && r[4] == 0xA2);
    // Box commands now use the new address.
    CHECK(status_of(rig.request(0x55, {1}, 0x02)) == 0);
}

void test_status_commands() {
    Rig rig;
    rig.box.module_ok[2] = false;  // odometer failed to init
    auto r = rig.request(0x15, {});
    CHECK(r.size() == 22 && r[5] == 0x11 && r[6] == 0x21 && r[7] == 0x30 && r[10] == 0x61 && r[11] == 0);
    CHECK(status_of(rig.request(0x30, {1})) == 0 && rig.w.aux_output);
    CHECK(status_of(rig.request(0x30, {0})) == 0 && !rig.w.aux_output);
    r = rig.request(0x5B, {});
    CHECK(r.size() == 14 && r[5] == 0x31 && r[9] == (0x35 & 0x39));
}

void test_usage_accounting() {
    Rig rig;
    rig.w.insert(0);
    std::memcpy(rig.box.rfid[0].record, rig.w.slots[0].record, 40);
    rig.box.rfid[0].has_record = true;
    rig.usage.begin(0, 50, rig.box.rfid[0].record);  // 50 % of a 330 m spool
    CHECK(rig.usage.total_mm(0) == 330000);
    CHECK(status_of(rig.request(0x10, {0x01, 0, 0})) == 0);
    CHECK(rig.feeder.stage1_hub_until_stop() == Status::Ok);
    CHECK(status_of(rig.request(0x04, {0x01, 0})) == 0);
    CHECK(rig.usage.mode(0) == UsageManager::Mode::Printing);
    // Print with the refill loop running: 66 s at 50 mm/s ≈ 3.3 m = 1 % of the spool.
    rig.w.consume(50.0f);
    BufferState last = rig.w.buffer();
    rig.w.tick_hook = [&] {
        const BufferState now = rig.w.buffer();
        if (now == last) return;
        if (now == BufferState::Empty) rig.print.on_buffer_empty();
        if (now == BufferState::Full) rig.print.on_buffer_full();
        last = now;
    };
    // 80 s of simulated time at 50 mm/s = 4 m (about 1.2 % of the spool).
    const uint32_t t_end = rig.w.now_ms() + 80000;
    while (rig.w.now_ms() < t_end) {
        rig.print.service();
        rig.usage.tick();
        rig.w.sleep_ms(10);
    }
    rig.w.tick_hook = nullptr;
    CHECK(rig.usage.used_mm(0) > 3800 && rig.usage.used_mm(0) < 4300);
    CHECK(rig.box.rfid[0].remaining_pct == 49);
    auto rem = rig.request(0x03, {0x01});
    CHECK(rem[5] == 49);
    CHECK(status_of(rig.request(0x04, {0x00, 1})) == 0);
    CHECK(rig.usage.mode(0) == UsageManager::Mode::Idle);
}

void test_supervisor_detooth_and_stop() {
    Rig rig;
    rig.w.insert(1);
    CHECK(status_of(rig.request(0x10, {0x02, 0, 0})) == 0);
    rig.w.run(Dir::Forward, 100);  // hub running
    rig.w.hub_blocked = true;
    for (int i = 0; i < 12; ++i) {  // 1.2 s blocked
        rig.supervisor.tick();
        rig.w.sleep_ms(100);
    }
    CHECK(rig.supervisor.detooth_count() == 1);
    CHECK(rig.w.current_speed() == 100);  // hub command restored
    for (int i = 0; i < 100; ++i) {  // keep blocked past 10 s
        rig.supervisor.tick();
        rig.w.sleep_ms(100);
    }
    CHECK(rig.supervisor.stopped_mask() == 0x10);
    CHECK(rig.w.current_speed() == 0);
    CHECK(rig.supervisor.detooth_count() == 1);  // once per 11.1 s window
}


void test_usage_reset_on_removal_and_runout() {
    Rig rig;
    rig.w.insert(2);
    for (int i = 0; i < 10; ++i) { rig.preload.sample(); rig.w.sleep_ms(20); }
    std::memcpy(rig.box.rfid[2].record, rig.w.slots[2].record, 40);
    rig.box.rfid[2].has_record = true;
    rig.usage.begin(2, 70, rig.box.rfid[2].record);
    rig.box.rfid[2].remaining_pct = 70;
    // Runout while printing reports 0 %.
    CHECK(status_of(rig.request(0x0A, {})) == to_u8(Status::SlotEvent));  // insert event first
    CHECK(status_of(rig.request(0x04, {0x04, 0})) == 0);
    rig.w.remove(2);
    CHECK(status_of(rig.request(0x0A, {})) == to_u8(Status::Runout));
    auto rem = rig.request(0x03, {0x04});
    CHECK(rem[7] == 0);
    // Taking the spool out forgets it: 0xFF and no record.
    for (int i = 0; i < 10; ++i) { rig.preload.sample(); rig.w.sleep_ms(20); }
    rem = rig.request(0x03, {0x04});
    CHECK(rem[7] == 0xFF && !rig.box.rfid[2].has_record && rig.usage.total_mm(2) == 0);
}

struct Test {
    const char* name;
    void (*fn)();
};

}  // namespace

// Parameter page: the record found at 0x0807F800 in a public live flash dump.
void test_param_store() {
    using namespace cfs::params;
    const uint8_t real[kRecordSize] = {0x16, 0x89, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
                                       0x08, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF};
    Record r;
    CHECK(decode(real, r));
    CHECK(r.version == 0x00010008 && r.mode == 0);
    uint8_t again[kRecordSize] = {};
    encode(r, again);
    CHECK(std::memcmp(again, real, kRecordSize) == 0);  // padding kept, same CRC

    // Another firmware version: keep the mode, rewrite the version.
    uint8_t moded[kRecordSize];
    encode(Record{0x00010008, 2, {0xFF, 0xFF, 0xFF}}, moded);
    Record l;
    CHECK(load(moded, kStockVersion, l));
    CHECK(l.version == kStockVersion && l.mode == 2);
    CHECK(!load(real, 0x00010008, l));  // same version: nothing to write

    // Erased or corrupted page: defaults.
    uint8_t erased[kRecordSize];
    std::memset(erased, 0xFF, sizeof erased);
    CHECK(!decode(erased, r));
    CHECK(load(erased, kStockVersion, l));
    CHECK(l.version == kStockVersion && l.mode == 0);
    uint8_t bad[kRecordSize];
    std::memcpy(bad, real, kRecordSize);
    bad[12] ^= 1;
    CHECK(!decode(bad, r));
}

// AES-128 (FIPS-197 appendix C.1) and the Creality tag chain with dummy keys;
// expected values computed with an independent AES library.
void test_aes_and_creality_tag() {
    const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                             0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t cipher[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30,
                                0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    uint8_t b[16];
    std::memcpy(b, plain, 16);
    cfs::aes128::encrypt_block(key, b);
    CHECK(std::memcmp(b, cipher, 16) == 0);
    cfs::aes128::decrypt_block(key, b);
    CHECK(std::memcmp(b, plain, 16) == 0);

    uint8_t kdf[16], rec_key[16];
    for (int i = 0; i < 16; ++i) {
        kdf[i] = static_cast<uint8_t>(15 - i);
        rec_key[i] = static_cast<uint8_t>(0x10 + 7 * i);
    }
    const uint8_t uid[4] = {0x37, 0x10, 0x15, 0x73};
    const uint8_t key_a[6] = {0x37, 0x7D, 0xCD, 0xE7, 0x92, 0x51};
    uint8_t got[6];
    cfs::creality::derive_key_a(kdf, uid, got);
    CHECK(std::memcmp(got, key_a, 6) == 0);
    uint8_t block[16] = {0x72, 0xD3, 0x2C, 0xAF, 0x83, 0x56, 0x21, 0x84,
                         0xB6, 0x27, 0x24, 0xB9, 0x65, 0x13, 0x4F, 0x52};
    cfs::creality::decrypt_block(rec_key, block);
    CHECK(std::memcmp(block, "AB1240276A210100", 16) == 0);
}

// SHA-256 (FIPS 180-2 "abc") and HKDF-SHA256 (RFC 5869 test case 1).
void test_sha256_hkdf() {
    const uint8_t abc[] = {'a', 'b', 'c'};
    const uint8_t abc_digest[32] = {0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA, 0x41, 0x41, 0x40,
                                    0xDE, 0x5D, 0xAE, 0x22, 0x23, 0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17,
                                    0x7A, 0x9C, 0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD};
    uint8_t d[32];
    cfs::sha256::digest(abc, sizeof abc, d);
    CHECK(std::memcmp(d, abc_digest, 32) == 0);

    uint8_t ikm[22], salt[13], info[10];
    std::memset(ikm, 0x0B, sizeof ikm);
    for (int i = 0; i < 13; ++i) salt[i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 10; ++i) info[i] = static_cast<uint8_t>(0xF0 + i);
    const uint8_t okm_ref[42] = {0x3C, 0xB2, 0x5F, 0x25, 0xFA, 0xAC, 0xD5, 0x7A, 0x90, 0x43, 0x4F,
                                 0x64, 0xD0, 0x36, 0x2F, 0x2A, 0x2D, 0x2D, 0x0A, 0x90, 0xCF, 0x1A,
                                 0x5A, 0x4C, 0x5D, 0xB0, 0x2D, 0x56, 0xEC, 0xC4, 0xC5, 0xBF, 0x34,
                                 0x00, 0x72, 0x08, 0xD5, 0xB8, 0x87, 0x18, 0x58, 0x65};
    uint8_t okm[42];
    CHECK(cfs::sha256::hkdf(salt, sizeof salt, ikm, sizeof ikm, info, sizeof info, okm, sizeof okm));
    CHECK(std::memcmp(okm, okm_ref, 42) == 0);
}

// ---- multi-vendor tags (docs/11) with simulated tags and dummy keys ----
struct FakeClassic : cfs::tags::TagSession {
    cfs::tags::CardInfo c;
    uint8_t blocks[64][16] = {};
    uint8_t keys[16][6] = {};
    int authed = -1;
    bool halted = false;
    int auth_calls = 0, reactivations = 0;
    explicit FakeClassic(const uint8_t uid[4]) {
        c.atqa[0] = 0x04;
        c.sak = 0x08;
        c.uid_len = 4;
        std::memcpy(c.uid, uid, 4);
        for (auto& k : keys) std::memset(k, 0x5A, 6);  // nothing matches by default
    }
    const cfs::tags::CardInfo& card() const override { return c; }
    bool reactivate() override {
        ++reactivations;
        halted = false;
        authed = -1;
        return true;
    }
    bool auth_a(uint8_t block, const uint8_t key[6]) override {
        ++auth_calls;
        if (halted) return false;
        if (std::memcmp(key, keys[block / 4], 6) == 0) {
            authed = block / 4;
            return true;
        }
        halted = true;  // a failed authentication drops the tag out of the session
        authed = -1;
        return false;
    }
    bool read16(uint8_t index, uint8_t out[16]) override {
        if (halted || authed != index / 4) return false;
        std::memcpy(out, blocks[index], 16);
        return true;
    }
};

struct FakeType2 : cfs::tags::TagSession {
    cfs::tags::CardInfo c;
    uint8_t mem[144] = {};  // NTAG213 user memory, from page 4
    FakeType2() {
        c.atqa[0] = 0x44;
        c.sak = 0x00;
        c.uid_len = 7;
        for (int i = 0; i < 7; ++i) c.uid[i] = static_cast<uint8_t>(0x04 + i);
    }
    const cfs::tags::CardInfo& card() const override { return c; }
    bool reactivate() override { return true; }
    bool auth_a(uint8_t, const uint8_t*) override { return false; }
    bool read16(uint8_t page, uint8_t out[16]) override {
        const size_t off = static_cast<size_t>(page - 4) * 4;
        if (page < 4 || off + 16 > sizeof mem) return false;  // past the end of user memory
        std::memcpy(out, mem + off, 16);
        return true;
    }
};

void test_multi_vendor_tags() {
    using namespace cfs::tags;
    const uint8_t uid[4] = {0x37, 0x10, 0x15, 0x73};
    uint8_t kdf[16], rec_key[16], salt[16];
    for (int i = 0; i < 16; ++i) {
        kdf[i] = static_cast<uint8_t>(15 - i);
        rec_key[i] = static_cast<uint8_t>(0x10 + 7 * i);
        salt[i] = static_cast<uint8_t>(0xA0 + i);
    }
    const KeyEntry key_list[] = {{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, KeyListDecoder::kHintQidi}};
    CrealityDecoder creality(kdf, rec_key);
    BambuDecoder bambu(salt, sizeof salt);
    KeyListDecoder keys(key_list, 1);
    Type2Decoder type2;
    TagDecoder* const list[] = {&creality, &bambu, &keys, &type2};
    TagPipeline pipeline(list, 4);

    // Bambu HKDF: sector 0 key computed independently (Python hmac + hashlib).
    uint8_t bk[16][6];
    CHECK(bambu.derive_keys(uid, bk));
    const uint8_t bambu_s0[6] = {0xEF, 0xE4, 0x4B, 0xB7, 0x8E, 0x36};
    CHECK(std::memcmp(bk[0], bambu_s0, 6) == 0);

    // Creality: Key A from the UID (value checked in aes_and_creality_tag), the
    // 48-character record encrypted into blocks 4..6.
    {
        FakeClassic t(uid);
        const uint8_t key_a[6] = {0x37, 0x7D, 0xCD, 0xE7, 0x92, 0x51};
        std::memcpy(t.keys[1], key_a, 6);
        const char* rec = "AB1240276A21010010FF88000330123456000000"
                          "00000000";
        for (int blk = 0; blk < 3; ++blk) {
            std::memcpy(t.blocks[4 + blk], rec + 16 * blk, 16);
            cfs::aes128::encrypt_block(rec_key, t.blocks[4 + blk]);
        }
        TagReadout r;
        CHECK(pipeline.read(t, r));
        CHECK(r.decoder == DecoderId::Creality && r.has_creality);
        CHECK(std::memcmp(r.creality.text, rec, 40) == 0);
        CHECK(std::strncmp(r.material, "101001", 6) == 0);
        CHECK(r.rgba == 0xFF8800FFu && r.length_m == 330);
    }

    // Bambu: the Creality attempt fails, the tag is reselected, Bambu reads it.
    FakeClassic b(uid);
    std::memcpy(b.keys, bk, sizeof bk);
    std::memcpy(b.blocks[2], "PLA", 3);
    std::memcpy(b.blocks[4], "PLA Basic", 9);
    const uint8_t b5[16] = {0xFF, 0x00, 0x00, 0xFF, 0xE8, 0x03, 0, 0, 0x00, 0x00, 0xE0, 0x3F};  // 1000 g, 1.75 mm
    const uint8_t b6[16] = {55, 0, 8, 0, 1, 0, 35, 0, 230, 0, 190, 0};
    std::memcpy(b.blocks[5], b5, 16);
    std::memcpy(b.blocks[6], b6, 16);
    b.blocks[14][4] = 0x4A;  // 330 m
    b.blocks[14][5] = 0x01;
    TagReadout r;
    CHECK(pipeline.read(b, r));
    CHECK(r.decoder == DecoderId::Bambu && b.reactivations == 1);
    CHECK(std::strncmp(r.material, "PLA", 4) == 0);
    CHECK(r.rgba == 0xFF0000FFu && r.weight_g == 1000 && r.diameter_centi_mm == 175);
    CHECK(r.nozzle_min_c == 190 && r.nozzle_max_c == 230 && r.length_m == 330);
    CHECK(r.raw_count == 6 && !r.has_creality);
    // Same UID again: the routing cache goes straight to Bambu.
    b.reactivations = 0;
    CHECK(pipeline.read(b, r) && r.decoder == DecoderId::Bambu && b.reactivations == 0);

    // Command 0x60 payload of that readout.
    uint8_t v[cfs::kMaxPayload];
    const size_t n = encode_tag_info(2, r, v, sizeof v);
    CHECK(n == kTagInfoHeader + 6 * 17);
    CHECK(v[0] == 2 && v[1] == 2 && v[3] == 4 && v[4] == 0x37 && v[17] == 'P');
    CHECK(v[33] == 0xFF && v[36] == 0xFF && v[37] == 0xE8 && v[38] == 0x03 && v[48] == 6);
    CHECK(v[kTagInfoHeader] == 1);  // first raw entry: block 1

    // Factory key (QIDI): raw block 4 and the vendor hint.
    {
        const uint8_t quid[4] = {0x11, 0x22, 0x33, 0x44};
        FakeClassic q(quid);
        std::memset(q.keys[1], 0xFF, 6);
        q.blocks[4][0] = 0x25;
        q.blocks[4][1] = 0x02;
        q.blocks[4][2] = 0x01;
        TagReadout qr;
        CHECK(pipeline.read(q, qr));
        CHECK(qr.decoder == DecoderId::KeyList && qr.vendor_hint == KeyListDecoder::kHintQidi);
        CHECK(qr.raw_count == 3 && qr.raw[0].index == 4 && qr.raw[0].data[0] == 0x25);
    }

    // Unknown MIFARE Classic: nothing reads it, the card identity is kept.
    {
        const uint8_t uuid[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        FakeClassic u(uuid);
        TagReadout ur;
        CHECK(!pipeline.read(u, ur));
        CHECK(ur.decoder == DecoderId::None && ur.card.uid[0] == 0xDE && !ur.has_creality);
    }

    // NTAG213 with an OpenSpool NDEF JSON record (143 bytes, past 128).
    {
        FakeType2 t;
        const char json[] =
            "{\"protocol\":\"openspool\",\"version\":\"1.0\",\"type\":\"PETG\","
            "\"color_hex\":\"1A2B3C\",\"brand\":\"X\",\"min_temp\":\"230\",\"max_temp\":\"250\"}";
        const char mime[] = "application/json";
        const size_t pl = sizeof json - 1, tl = sizeof mime - 1;
        size_t o = 0;
        t.mem[o++] = 0x03;
        t.mem[o++] = static_cast<uint8_t>(3 + tl + pl);
        t.mem[o++] = 0xD2;  // MB | ME | SR, TNF = MIME media
        t.mem[o++] = static_cast<uint8_t>(tl);
        t.mem[o++] = static_cast<uint8_t>(pl);
        std::memcpy(t.mem + o, mime, tl);
        o += tl;
        std::memcpy(t.mem + o, json, pl);
        o += pl;
        t.mem[o++] = 0xFE;
        TagReadout tr;
        CHECK(pipeline.read(t, tr));
        CHECK(tr.decoder == DecoderId::Type2 && (tr.flags & kFlagNdef));
        CHECK(std::strncmp(tr.material, "PETG", 5) == 0 && tr.rgba == 0x1A2B3CFFu);
        CHECK(tr.nozzle_min_c == 230 && tr.nozzle_max_c == 250);
        CHECK(tr.has_creality && std::strncmp(tr.creality.text, "038CD21079", 10) == 0);
    }
}

void test_tag_info_command() {
    Rig rig;
    CHECK(status_of(rig.request(0x60, {1})) == 0x08);  // nothing read yet
    CHECK(status_of(rig.request(0x60, {4})) == 0x01);  // bad slot
    cfs::tags::TagReadout r;
    r.decoder = cfs::tags::DecoderId::Type2;
    r.card.uid_len = 7;
    r.set_material("PETG", 4);
    rig.w.tag_readout[1] = &r;
    const auto reply = rig.request(0x60, {1});
    CHECK(status_of(reply) == 0 && reply.size() == 6 + cfs::tags::kTagInfoHeader);
    CHECK(reply[5] == 1 && reply[6] == 4 && reply[5 + 17] == 'P');
}

// ---- Runtime Config API v2 (box_cfs_runtime.py wire format) and its interlock ----
std::vector<uint8_t> rt_payload(const std::vector<uint8_t>& frame_bytes) {
    // F7 addr len status cmd payload... crc
    if (frame_bytes.size() < 6) return {};
    return std::vector<uint8_t>(frame_bytes.begin() + 5, frame_bytes.end() - 1);
}

void test_runtime_config_protocol() {
    Rig rig;
    auto r = rig.request(0x57, {0x10});
    CHECK(status_of(r) == 0 && rt_payload(r) == std::vector<uint8_t>({2, 28, 1, 0x2B}));

    // Every parameter: GET equals the catalogue default with a default Config,
    // DESCRIBE carries type, flags, default, min and max.
    for (uint8_t id = 0; id < cfs::RuntimeConfig::kCount; ++id) {
        const auto& p = cfs::RuntimeConfig::kParams[id];
        r = rig.request(0x57, {0x11, id});
        auto v = rt_payload(r);
        CHECK(status_of(r) == 0 && v.size() == 3 && v[0] == id && (v[1] | v[2] << 8) == p.def);
        r = rig.request(0x57, {0x14, id});
        v = rt_payload(r);
        CHECK(status_of(r) == 0 && v.size() == 8 && (v[2] | v[3] << 8) == p.def &&
              (v[4] | v[5] << 8) == p.min && (v[6] | v[7] << 8) == p.max);
    }
    CHECK(status_of(rig.request(0x57, {0x11, 28})) == 1);  // unknown id
    CHECK(status_of(rig.request(0x57, {0x09})) == 1);      // K2-OpenHost-only sub-command
}

void test_runtime_config_interlock() {
    Rig rig;
    auto set = [&](uint8_t id, uint16_t v) {
        return rig.request(0x57, {0x12, id, static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)});
    };
    // Fresh boot: not yet seen at rest for 500 ms.
    CHECK(status_of(set(1, 120)) == 7 && rig.cfg.speeds.hub_forward == 100);
    rig.w.sleep_ms(600);
    auto r = set(1, 120);
    CHECK(status_of(r) == 0 && rt_payload(r) == std::vector<uint8_t>({1, 120, 0}));
    CHECK(rig.cfg.speeds.hub_forward == 120);

    // Bounds and read-only parameters are refused without touching the value.
    CHECK(status_of(set(1, 0)) == 1 && rig.cfg.speeds.hub_forward == 120);
    CHECK(status_of(set(0, 200)) == 1 && rig.cfg.speeds.feeder_forward == 255);  // GPIO feeders
    CHECK(status_of(set(25, 20000)) == 1);                                         // not modeled

    // ID 8 counts 10 ms polls like stock.
    CHECK(status_of(set(8, 900)) == 0 && rig.cfg.timing.hub_sensor_timeout_ms == 9000);

    // A feeder running: busy. Stopped again: still busy until 500 ms at rest.
    rig.hw.feeders.run(2, Dir::Forward, 255);
    CHECK(status_of(set(5, 90)) == 7);
    rig.hw.feeders.stop(2);
    CHECK(status_of(set(5, 90)) == 7);
    rig.w.sleep_ms(300);
    CHECK(status_of(set(5, 90)) == 7);
    rig.w.sleep_ms(300);
    CHECK(status_of(set(5, 90)) == 0 && rig.cfg.speeds.hub_reverse == 90);

    // A short hub run between two polls is still seen (sticky idle check).
    rig.w.sleep_ms(600);
    rig.hw.hub.run(Dir::Forward, 100);
    rig.hw.hub.stop();
    CHECK(status_of(set(5, 95)) == 7);

    // Not IDLE, or a motion scope open: busy even with every output off.
    rig.w.sleep_ms(600);
    rig.box.enter(BoxState::Print);
    CHECK(status_of(set(2, 210)) == 7);
    rig.box.set_idle();
    rig.w.sleep_ms(600);
    {
        MotionScope m(rig.box);
        CHECK(status_of(set(2, 210)) == 7);
    }
    rig.w.sleep_ms(600);
    CHECK(status_of(set(2, 210)) == 0);

    // RESET follows the same interlock, then restores every default.
    rig.hw.hub.run(Dir::Forward, 100);
    CHECK(status_of(rig.request(0x57, {0x13, 0xFF})) == 7 && rig.cfg.speeds.hub_forward == 120);
    rig.hw.hub.stop();
    rig.w.sleep_ms(600);
    CHECK(status_of(rig.request(0x57, {0x13, 0xFF})) == 0);
    CHECK(rig.cfg.speeds.hub_forward == 100 && rig.cfg.speeds.hub_reverse == 80 &&
          rig.cfg.speeds.hub_transition == 200 && rig.cfg.timing.hub_sensor_timeout_ms == 7000);
}

// ---- the cooperative application loop (docs/13) ----
struct AppRig {
    sim::World w;
    Hardware hw = w.hardware();
    cfs::App app{hw, 1};
    std::function<void()> extra;  // test observer, runs before background()
    AppRig() {
        app.init();
        w.tick_hook = [this] {
            if (extra) extra();
            app.background();  // what the board's clock idle hook does
        };
    }
    // Runs main-loop passes for `ms` of simulated time.
    void run(uint32_t ms) {
        const uint32_t end = w.now_ms() + ms;
        while (static_cast<int32_t>(w.now_ms() - end) < 0) {
            app.loop_once();
            w.sleep_ms(1);
        }
    }
    std::vector<uint8_t> request(uint8_t cmd, std::vector<uint8_t> payload) {
        w.tx.clear();
        w.inject(w.now_ms(), frame(1, cmd, payload));
        app.loop_once();
        auto frames = w.sent_frames();
        return frames.empty() ? std::vector<uint8_t>{} : frames.back();
    }
};

void test_app_print_refill_and_background() {
    AppRig r;
    r.w.insert(0);
    CHECK(status_of(r.request(0x10, {0x01, 0, 0})) == 0);
    CHECK(r.app.feeder.stage1_hub_until_stop() == Status::Ok);
    CHECK(status_of(r.request(0x04, {0x01, 0})) == 0);
    CHECK(r.app.box.state() == BoxState::Print);

    // Printing: the buffer edges reach the print tracker from the loop itself.
    r.w.consume(5.0f);
    int empties = 0;
    bool injected = false;
    BufferState last = r.w.buffer();
    r.extra = [&] {
        const BufferState b = r.w.buffer();
        if (b != last && b == BufferState::Empty) {
            ++empties;
            if (!injected) {  // host traffic while a refill is starting
                r.w.tx.clear();
                r.w.inject(r.w.now_ms(), frame(1, 0x0A, {}));
                r.w.inject(r.w.now_ms(), frame(1, 0x06, {0x01, 0x01, 0x00}));
                injected = true;
            }
        }
        last = b;
    };
    r.run(30000);
    r.extra = nullptr;
    CHECK(empties > 0 && r.app.box.state() == BoxState::Print);
    // Both injected commands were answered by the background path: the status query
    // normally, the motor command BUSY.
    bool state_ok = false, feeder_busy = false;
    for (const auto& f : r.w.sent_frames()) {
        if (f.size() > 4 && f[4] == 0x0A && f[3] == 0) state_ok = true;
        if (f.size() > 4 && f[4] == 0x06 && f[3] == 0x03) feeder_busy = true;
    }
    CHECK(state_ok && feeder_busy);
    CHECK(status_of(r.request(0x04, {0x00, 1})) == 0 && r.app.box.state() == BoxState::Idle);
}

void test_app_supervision_during_blocking_routine() {
    AppRig r;
    r.w.insert(0);
    CHECK(status_of(r.request(0x10, {0x01, 0, 0})) == 0);
    CHECK(r.app.feeder.stage1_hub_until_stop() == Status::Ok);  // filament at the hub sensor
    CHECK(status_of(r.request(0x04, {0x01, 0})) == 0);
    r.w.hub_blocked = true;
    r.app.print.on_buffer_empty();
    r.app.loop_once();  // the refill session blocks here; supervision runs in background()
    CHECK(r.app.supervisor.detooth_count() > 0);
    CHECK(r.app.box.state() == BoxState::Error);
}

void test_app_insert_event() {
    AppRig r;
    BoxState seen = BoxState::Idle;
    r.extra = [&] {
        if (r.app.box.state() != BoxState::Idle) seen = r.app.box.state();
    };
    r.w.insert(2);
    r.run(300);  // 6 samples of 20 ms debounce, then the preload picks the slot
    CHECK(r.app.preload.inlet(2));
    CHECK(seen == BoxState::Preload);
}

// Stock pretension (0x080055B0): in/out cycles counted on the hub sensor; ID 27
// sets the count through the runtime config.
void test_pretension_cycles() {
    for (uint16_t cycles : {static_cast<uint16_t>(3), static_cast<uint16_t>(2)}) {
        AppRig r;
        r.app.box.read_on_insert = false;  // pretension only, no RFID scan
        if (cycles != 3) {
            r.w.sleep_ms(600);  // interlock: at rest for 500 ms
            CHECK(status_of(r.request(0x57, {0x12, 27, static_cast<uint8_t>(cycles), 0})) == 0);
        }
        int rises = 0;
        bool last = false;
        r.extra = [&] {
            const bool now = r.hw.sensors.hub_present(1);
            if (now && !last) ++rises;
            last = now;
        };
        r.w.insert(1);
        r.run(2000);
        CHECK(rises == cycles);
        CHECK(r.app.box.state() == BoxState::Idle);
    }
}

// 0x0F pauses insertion handling (stock 0x080053B0); 0x0D sub 3 = forced
// pretension only, sub 2 = pretension + RFID read (stock 0x08004EC4/0x08004E80).
void test_preload_pause_and_forced_preload() {
    {
        AppRig r;
        CHECK(status_of(r.request(0x0F, {1})) == 0 && r.app.box.preload_paused);
        int rises = 0;
        bool last = false;
        r.extra = [&] {
            const bool now = r.hw.sensors.hub_present(0);
            if (now && !last) ++rises;
            last = now;
        };
        r.w.insert(0);
        r.run(1500);
        CHECK(rises == 0);  // paused: no pretension
        CHECK(status_of(r.request(0x0F, {0})) == 0);
        r.run(1500);
        CHECK(rises == 0);  // an insertion seen while paused is dropped, as stock
        r.w.remove(0);
        r.run(500);
        r.w.insert(0);
        r.run(3000);
        CHECK(rises == 3);  // resumed: a new insertion is handled
        CHECK(status_of(r.request(0x0F, {2})) == 1);
    }
    for (uint8_t sub : {static_cast<uint8_t>(3), static_cast<uint8_t>(2)}) {
        AppRig r;
        r.app.box.read_on_insert = false;  // inserted silently: only pretension now
        r.w.insert(1);
        r.run(3000);
        int rises = 0;
        bool last = false, preload_seen = false;
        r.extra = [&] {
            const bool now = r.hw.sensors.hub_present(1);
            if (now && !last) ++rises;
            last = now;
            if (r.app.box.state() == BoxState::Preload) preload_seen = true;
        };
        // Slots 0 and 1 masked; slot 0 is empty and skipped without error.
        const auto reply = r.request(0x0D, {0x03, sub});
        CHECK(status_of(reply) == 0);
        CHECK(rises >= 3);                     // the pretension cycles ran
        CHECK(preload_seen == (sub == 2));     // only sub 2 reads the tag
        CHECK(r.app.box.read_on_insert == false);  // restored afterwards
        CHECK(r.app.box.state() == BoxState::Idle);
    }
}

// Factory/aging modes are refused: never stored, never started (docs/03).
void test_factory_modes_refused() {
    Rig rig;
    CHECK(status_of(rig.request(0x50, {2})) == 1 && rig.w.mode() == 0);
    CHECK(status_of(rig.request(0x50, {0})) == 3);  // already standard, as stock
    CHECK(status_of(rig.request(0x5D, {})) == 1);
}

// ---- NFC Type 4 (ISO-DEP) with a simulated tag ----
struct FakeType4 : cfs::tags::TagSession {
    cfs::tags::CardInfo c;
    std::vector<uint8_t> cc, ndef;  // files E103 and E104
    const std::vector<uint8_t>* selected = nullptr;
    bool app_selected = false, wtx_once = true;
    int expect_block = 0;  // block number the next I-block must carry
    bool block_error = false;
    FakeType4() {
        c.atqa[0] = 0x44;
        c.sak = 0x20;
        c.uid_len = 7;
        for (int i = 0; i < 7; ++i) c.uid[i] = static_cast<uint8_t>(0x10 + i);
    }
    const cfs::tags::CardInfo& card() const override { return c; }
    bool reactivate() override { return true; }
    bool auth_a(uint8_t, const uint8_t*) override { return false; }
    bool read16(uint8_t, uint8_t*) override { return false; }
    bool transceive(const uint8_t* tx, size_t n, uint8_t* rx, size_t cap, size_t& rn) override {
        std::vector<uint8_t> out;
        if (n == 2 && tx[0] == 0xE0) {
            out = {0x05, 0x78, 0x80, 0x70, 0x02};  // ATS
        } else if (n == 2 && (tx[0] & 0xF7) == 0xF2) {
            out = {static_cast<uint8_t>(0x02 | expect_block), 0x90, 0x00};  // answer after WTX
            pending_ok_ = false;
            expect_block ^= 1;
        } else if (n == 1 && tx[0] == 0xC2) {
            out = {0xC2};
        } else if (n >= 5 && (tx[0] & 0xE2) == 0x02) {
            if ((tx[0] & 1) != expect_block) block_error = true;
            const uint8_t* a = tx + 1;
            std::vector<uint8_t> data;
            bool ok = false;
            if (a[1] == 0xA4 && a[2] == 0x04) {
                app_selected = true;
                ok = true;
                if (wtx_once) {  // ask for more time once, then answer
                    wtx_once = false;
                    out = {0xF2, 0x01};
                    pending_ok_ = true;
                    rn = out.size();
                    std::memcpy(rx, out.data(), rn);
                    return true;
                }
            } else if (a[1] == 0xA4 && a[2] == 0x00 && app_selected) {
                selected = (a[5] == 0xE1 && a[6] == 0x03) ? &cc : (a[5] == 0xE1 && a[6] == 0x04) ? &ndef : nullptr;
                ok = selected != nullptr;
            } else if (a[1] == 0xB0 && selected) {
                const size_t off = static_cast<size_t>(a[2] << 8 | a[3]);
                const size_t len = a[4];
                ok = off + len <= selected->size();
                if (ok) data.assign(selected->begin() + off, selected->begin() + off + len);
            }
            out.push_back(static_cast<uint8_t>(0x02 | expect_block));
            out.insert(out.end(), data.begin(), data.end());
            out.push_back(ok ? 0x90 : 0x6A);
            out.push_back(ok ? 0x00 : 0x82);
            expect_block ^= 1;
        } else {
            return false;
        }
        if (out.size() > cap) return false;
        std::memcpy(rx, out.data(), out.size());
        rn = out.size();
        return true;
    }
    bool pending_ok_ = false;
};

void test_type4_ndef() {
    using namespace cfs::tags;
    FakeType4 t;
    t.cc = {0x00, 0x0F, 0x20, 0x00, 0x3B, 0x00, 0x34, 0x04, 0x06, 0xE1, 0x04, 0x00, 0xFF, 0x00, 0xFF};
    const char json[] = "{\"protocol\":\"openspool\",\"type\":\"PLA\",\"color_hex\":\"112233\",\"min_temp\":\"200\",\"max_temp\":\"220\"}";
    const char mime[] = "application/json";
    const size_t pl = sizeof json - 1, tl = sizeof mime - 1;
    std::vector<uint8_t> msg = {0xD2, static_cast<uint8_t>(tl), static_cast<uint8_t>(pl)};
    msg.insert(msg.end(), mime, mime + tl);
    msg.insert(msg.end(), json, json + pl);
    t.ndef = {static_cast<uint8_t>(msg.size() >> 8), static_cast<uint8_t>(msg.size())};
    t.ndef.insert(t.ndef.end(), msg.begin(), msg.end());

    Type4Decoder type4;
    TagDecoder* const list[] = {&type4};
    TagPipeline pipeline(list, 1);
    TagReadout r;
    CHECK(pipeline.read(t, r));
    CHECK(r.decoder == DecoderId::Type4 && (r.flags & kFlagNdef) && !t.block_error);
    CHECK(std::strncmp(r.material, "PLA", 4) == 0 && r.rgba == 0x112233FFu);
    CHECK(r.nozzle_min_c == 200 && r.nozzle_max_c == 220 && !r.has_creality);
}

// Deterministic fuzzing (run under ASan/UBSan in CI): random bytes into the frame
// receiver, the dispatcher (commands that cannot start motion, plus unknown
// codes), the NDEF/JSON parsers, the parameter page codec and the 0x60 encoder.
struct XorShift {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    uint8_t byte() { return static_cast<uint8_t>(next()); }
};

void test_fuzz() {
    XorShift rng;
    // Raw byte noise into a receiver: never a crash, frames only with a valid CRC.
    cfs::Receiver rx(1);
    cfs::Frame f;
    int frames = 0;
    for (int i = 0; i < 200000; ++i) {
        if (rx.push(rng.byte(), static_cast<uint32_t>(i / 10), f) == cfs::Receiver::Result::Frame) ++frames;
    }
    CHECK(frames >= 0);

    // Valid frames with random payloads through the dispatcher.
    static const uint8_t kCmds[] = {0x01, 0x02, 0x03, 0x05, 0x0A, 0x0C, 0x0F, 0x14, 0x15, 0x30, 0x51,
                                    0x52, 0x53, 0x55, 0x57, 0x5B, 0x5D, 0x60, 0x20, 0x2F, 0x61, 0x99};
    Rig rig;
    int replies = 0;
    for (int i = 0; i < 4000; ++i) {
        const uint8_t cmd = kCmds[rng.next() % sizeof kCmds];
        std::vector<uint8_t> payload(rng.next() % 24);
        for (auto& b : payload) b = rng.byte();
        if (cmd == 0x57 && !payload.empty() && (payload[0] == 0x12 || payload[0] == 0x13)) payload[0] = 0x11;
        if (cmd == 0x0F && !payload.empty()) payload[0] &= 1;  // keep the preload enabled
        replies += rig.request(cmd, payload).empty() ? 0 : 1;
    }
    CHECK(replies > 0 && rig.box.state() != BoxState::Error);

    // Parsers with random memory.
    bool encode_ok = true;
    for (int i = 0; i < 20000; ++i) {
        uint8_t mem[160];
        const size_t n = rng.next() % sizeof mem;
        for (size_t k = 0; k < n; ++k) mem[k] = rng.byte();
        if (n > 2 && (rng.next() & 1)) mem[0] = 0x03;  // often an NDEF TLV
        if (n > 4 && (rng.next() & 1)) mem[2] = 0xD2;  // often a MIME record
        cfs::tags::TagReadout r;
        cfs::tags::parse_ndef_json(mem, n, r);
        cfs::tags::parse_ndef_message(mem, n, r);
        char out[24];
        cfs::tags::json_string(reinterpret_cast<const char*>(mem), n, "type", out, sizeof out);
        uint8_t page[cfs::params::kRecordSize];
        for (auto& b : page) b = rng.byte();
        cfs::params::Record rec;
        cfs::params::load(page, cfs::params::kStockVersion, rec);
        r.raw_count = static_cast<uint8_t>(rng.next() % (cfs::tags::TagReadout::kMaxRaw + 1));
        r.card.uid_len = static_cast<uint8_t>(rng.next() % 11);
        uint8_t v[cfs::kMaxPayload];
        encode_ok = encode_ok && cfs::tags::encode_tag_info(1, r, v, sizeof v) <= sizeof v;
    }
    CHECK(encode_ok);
}

// ---- vendor tag formats from public specifications (docs/11) ----
std::vector<uint8_t> pages(std::initializer_list<uint32_t> words) {  // big-endian page words
    std::vector<uint8_t> m;
    for (uint32_t w : words) {
        m.push_back(static_cast<uint8_t>(w >> 24));
        m.push_back(static_cast<uint8_t>(w >> 16));
        m.push_back(static_cast<uint8_t>(w >> 8));
        m.push_back(static_cast<uint8_t>(w));
    }
    return m;
}

void test_vendor_formats() {
    using namespace cfs::tags;
    // Anycubic v1, Pantone Spring Leaf PLA: pages 0x04..0x27 of the published dump.
    {
        const auto m = pages({0x7b006400, 0x48504c31, 0x392d3130, 0x32000000, 0, 0, 0, 0, 0, 0, 0,
                              0x504c4100, 0, 0, 0, 0, 0xff4fa889, 0, 0, 0, 0xc800d200, 0, 0, 0, 0,
                              0x32003c00, 0xaf004a01, 0xe8030000, 0, 0, 0, 0, 0, 0, 0, 0});
        TagReadout r;
        CHECK(parse_anycubic(m.data(), m.size(), r) && r.vendor_hint == kHintAnycubic);
        CHECK(std::strncmp(r.material, "PLA", 4) == 0 && r.rgba == 0x89A84FFFu);
        CHECK(r.nozzle_min_c == 200 && r.nozzle_max_c == 210);
        CHECK(r.diameter_centi_mm == 175 && r.length_m == 330 && r.weight_g == 1000);
    }
    // Anycubic v2 HS PLA: three speed/temperature ranges, 190..240 °C overall.
    {
        const auto m = pages({0x7b006500, 0x4148504c, 0x5042572d, 0x31303200, 0, 0, 0x41430000, 0, 0, 0, 0,
                              0x504c413f, 0x48696768, 0x3f537065, 0x65640000, 0, 0xff0006e1, 0, 0,
                              0x32009600, 0xbe00d200, 0x96002c01, 0xd200e600, 0x2c015802, 0xe6000401,
                              0x32003c00, 0xaf004a01, 0xe8030000, 0, 0, 0, 0, 0, 0, 0, 0});
        TagReadout r;
        CHECK(parse_anycubic(m.data(), m.size(), r));
        CHECK(std::strncmp(r.material, "PLA?High?Speed", 15) == 0 && r.rgba == 0xE10600FFu);
        CHECK(r.nozzle_min_c == 190 && r.nozzle_max_c == 260);  // 0x0104 = 260 in the dump's third range
    }
    // TigerTag example: Rosa3D red PLA (TigerTag-RFID-Guide section 4).
    {
        auto m = pages({0x5BF59264, 0xFFFFFFFF, 0x954B6800, 0x8E384E19, 0xFF0000FF, 0x0003E815,
                        0x00C300E6, 0x3205323C, 0x2F7A0A5C, 0, 0, 0x00E60000});
        m.resize(144, 0);
        TagReadout r;
        CHECK(parse_tigertag(m.data(), m.size(), r) && r.vendor_hint == kHintTigerTag);
        CHECK(std::strncmp(r.material, "TT38219", 8) == 0 && r.rgba == 0xFF0000FFu);
        CHECK(r.weight_g == 1000 && r.nozzle_min_c == 195 && r.nozzle_max_c == 230 && r.diameter_centi_mm == 175);
        CHECK(!parse_anycubic(m.data(), m.size(), r) && !parse_elegoo(m.data(), m.size(), r));
    }
    // ELEGOO guide example: PLA-CF, red, 1.75 mm, 1000 g, February 2025.
    {
        auto m = std::vector<uint8_t>{0x36, 0xEE, 0xEE, 0xEE, 0xEE, 0x00, 0x01, 'P', 'L', 'A', ' ',
                                      'C', 'F', '2', '0', 0xFF, 0x37, 0x00, 0x00, 0xAF, 0x03, 0xE8, 0x09, 0xC6};
        m.resize(32, 0);
        TagReadout r;
        CHECK(parse_elegoo(m.data(), m.size(), r) && r.vendor_hint == kHintElegoo);
        CHECK(std::strncmp(r.material, "PLA-CF20", 9) == 0 && r.rgba == 0xFF3700FFu);
        CHECK(r.diameter_centi_mm == 175 && r.weight_g == 1000);
    }
    // OpenTag3D record after a URI record (the reader takes the first opentag3d one).
    {
        std::vector<uint8_t> payload(0xB8, 0);
        payload[0] = 0x07;
        payload[1] = 0xD3;  // version 2.003
        std::memcpy(&payload[0x02], "PLA", 3);
        std::memcpy(&payload[0x07], "Silk", 4);
        payload[0x3C] = 0x11; payload[0x3D] = 0x22; payload[0x3E] = 0x33; payload[0x3F] = 0xFF;
        payload[0x8C] = 0x06; payload[0x8D] = 0xD6;  // 1750 um
        payload[0x91] = 38; payload[0x92] = 46;      // 190 / 230 °C
        payload[0x9E] = 0x03; payload[0x9F] = 0xE8;  // 1000 g
        payload[0xA2] = 0x01; payload[0xA3] = 0x4A;  // 330 m
        const char mime[] = "application/opentag3d";
        std::vector<uint8_t> msg = {0x91, 0x01, 0x04, 'U', 0x04, 'x', '.', 'y'};  // MB, SR, TNF 1, "U"
        msg.push_back(0x52);  // ME, SR, TNF 2
        msg.push_back(sizeof mime - 1);
        msg.push_back(static_cast<uint8_t>(payload.size()));
        msg.insert(msg.end(), mime, mime + sizeof mime - 1);
        msg.insert(msg.end(), payload.begin(), payload.end());
        TagReadout r;
        CHECK(parse_ndef_message(msg.data(), msg.size(), r) && r.vendor_hint == kHintOpenTag3D);
        CHECK(std::strncmp(r.material, "PLA Silk", 9) == 0 && r.rgba == 0x112233FFu);
        CHECK(r.diameter_centi_mm == 175 && r.nozzle_min_c == 190 && r.nozzle_max_c == 230);
        CHECK(r.weight_g == 1000 && r.length_m == 330);
    }
    // Snapmaker U1 with a dummy salt; sector keys checked with Python hmac/hashlib.
    {
        static const uint8_t salt[25] = {'T', 'e', 's', 't', 'S', 'a', 'l', 't', 'F', 'o', 'r', 'U', 'n',
                                         'i', 't', 'T', 'e', 's', 't', 's', '_', '0', '1', '2', '3'};
        const uint8_t uid[4] = {0x37, 0x10, 0x15, 0x73};
        SnapmakerDecoder snap(salt, sizeof salt);
        uint8_t k1[6], k2[6];
        CHECK(snap.derive_key(uid, 1, k1) && snap.derive_key(uid, 2, k2));
        const uint8_t k1_ref[6] = {0xAF, 0x94, 0x18, 0xA9, 0xE5, 0xBA};
        const uint8_t k2_ref[6] = {0x8D, 0xD0, 0x8A, 0x77, 0xD7, 0xEC};
        CHECK(std::memcmp(k1, k1_ref, 6) == 0 && std::memcmp(k2, k2_ref, 6) == 0);
        FakeClassic t(uid);
        std::memcpy(t.keys[1], k1, 6);
        std::memcpy(t.keys[2], k2, 6);
        const uint8_t b4[16] = {1, 0, 1, 0, 2, 0, 0, 0, 1, 0};  // version 1, PLA, Matte, tray 0, 1 color, alpha 0
        const uint8_t b5[16] = {0xAA, 0xBB, 0xCC};
        const uint8_t b8[16] = {0xAF, 0x00, 0xE8, 0x03, 0x4A, 0x01};  // 175, 1000 g, 330 m
        const uint8_t b9[16] = {55, 0, 8, 0, 220, 0, 190, 0};
        std::memcpy(t.blocks[4], b4, 16);
        std::memcpy(t.blocks[5], b5, 16);
        std::memcpy(t.blocks[8], b8, 16);
        std::memcpy(t.blocks[9], b9, 16);
        TagDecoder* const list[] = {&snap};
        TagPipeline pipeline(list, 1);
        TagReadout r;
        CHECK(pipeline.read(t, r) && r.decoder == DecoderId::Snapmaker && r.vendor_hint == kHintSnapmaker);
        CHECK(std::strncmp(r.material, "PLA Matte", 10) == 0 && r.rgba == 0xAABBCCFFu);
        CHECK(r.diameter_centi_mm == 175 && r.weight_g == 1000 && r.length_m == 330);
        CHECK(r.nozzle_min_c == 190 && r.nozzle_max_c == 220 && r.raw_count == 6);
    }
}

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
        {"auto_address", test_auto_address},
        {"status_commands", test_status_commands},
        {"usage_accounting", test_usage_accounting},
        {"supervisor_detooth_and_stop", test_supervisor_detooth_and_stop},
        {"usage_reset_on_removal_and_runout", test_usage_reset_on_removal_and_runout},
        {"param_store", test_param_store},
        {"aes_and_creality_tag", test_aes_and_creality_tag},
        {"sha256_hkdf", test_sha256_hkdf},
        {"multi_vendor_tags", test_multi_vendor_tags},
        {"tag_info_command", test_tag_info_command},
        {"runtime_config_protocol", test_runtime_config_protocol},
        {"runtime_config_interlock", test_runtime_config_interlock},
        {"app_print_refill_and_background", test_app_print_refill_and_background},
        {"app_supervision_during_blocking", test_app_supervision_during_blocking_routine},
        {"app_insert_event", test_app_insert_event},
        {"pretension_cycles", test_pretension_cycles},
        {"preload_pause_and_forced", test_preload_pause_and_forced_preload},
        {"factory_modes_refused", test_factory_modes_refused},
        {"type4_ndef", test_type4_ndef},
        {"vendor_formats", test_vendor_formats},
        {"fuzz", test_fuzz},
    };
    for (const Test& t : tests) {
        const int before = g_failed;
        t.fn();
        std::printf("%-28s %s\n", t.name, g_failed == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
