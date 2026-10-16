// SPDX-License-Identifier: Apache-2.0
// Host tests: protocol codec, dispatcher contract and filament-path behavior on
// the simulator. No external framework.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "cfs/aes128.hpp"
#include "cfs/creality_tag.hpp"
#include "cfs/dispatcher.hpp"
#include "cfs/feeder.hpp"
#include "cfs/frame.hpp"
#include "cfs/param_store.hpp"
#include "cfs/preload.hpp"
#include "cfs/print_tracker.hpp"
#include "cfs/spool.hpp"
#include "cfs/supervisor.hpp"
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
    MotorSupervisor supervisor{hw, box};
    Dispatcher disp;

    explicit Rig(uint8_t address = 1) : disp(hw, box, cfg, feeder, print, address) {
        disp.set_scanner(&scanner);
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
    };
    for (const Test& t : tests) {
        const int before = g_failed;
        t.fn();
        std::printf("%-28s %s\n", t.name, g_failed == before ? "ok" : "FAILED");
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
