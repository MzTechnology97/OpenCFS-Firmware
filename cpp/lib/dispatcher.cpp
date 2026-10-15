// SPDX-License-Identifier: Apache-2.0
#include "cfs/dispatcher.hpp"

#include <cstring>

namespace cfs {

namespace {

void put_be_float(float v, uint8_t out[4]) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    out[0] = static_cast<uint8_t>(u >> 24);
    out[1] = static_cast<uint8_t>(u >> 16);
    out[2] = static_cast<uint8_t>(u >> 8);
    out[3] = static_cast<uint8_t>(u);
}

}  // namespace

// Stage-4 link: while the feed loop runs it owns the receiver and only
// understands LOAD frames to the own address (stage 5 = progress, stage 6 = stop).
// Bad CRCs, broadcasts and other commands are dropped silently, as in stock.
class Dispatcher::LoadLink : public Stage4Link {
public:
    explicit LoadLink(Dispatcher& d) : d_(d) {}
    Poll poll() override {
        uint8_t b;
        while (d_.hw_.link.read_byte(b)) {
            Frame f;
            if (d_.rx_.push(b, d_.hw_.clock.now_ms(), f) != Receiver::Result::Frame) continue;
            if (f.addr != d_.address_ || f.cmd != to_u8(Cmd::Load) || f.len != 3) continue;
            d_.reply_addr_ = d_.address_;
            d_.silent_ = false;
            if (f.payload[1] == 5) return Poll::Progress;
            if (f.payload[1] == 6) return Poll::Stop;
        }
        return Poll::None;
    }
    void reply_progress(float odom) override {
        uint8_t p[4];
        put_be_float(odom, p);
        d_.reply(to_u8(Cmd::Load), Status::Ok, p, 4);
    }
    void reply_stop(Status s) override { d_.reply(to_u8(Cmd::Load), s); }

private:
    Dispatcher& d_;
};

void Dispatcher::poll() {
    uint8_t b;
    while (hw_.link.read_byte(b)) {
        Frame f;
        if (rx_.push(b, hw_.clock.now_ms(), f) == Receiver::Result::Frame) {
            handle(f);
        }
    }
    rx_.poll_timeout(hw_.clock.now_ms());
    auto_.poll();
}

void Dispatcher::reply_raw(uint8_t cmd, uint8_t status, const uint8_t* data, size_t len) {
    if (silent_) return;
    uint8_t buf[kMaxPayload + 6];
    const size_t n = encode(reply_addr_, status, cmd, data, len, buf, sizeof buf);
    if (n) hw_.link.write(buf, n);
}

void Dispatcher::reply(uint8_t cmd, Status st, const uint8_t* data, size_t len) {
    reply_raw(cmd, to_u8(st), data, len);
}

bool Dispatcher::check_len(const Frame& f, uint8_t payload_len) {
    if (f.len == payload_len) return true;
    reply(f.cmd, Status::InvalidParam);
    return false;
}

bool Dispatcher::slot_from_mask(uint8_t mask, bool allow_zero, int& slot) {
    switch (mask) {
    case 0x01: slot = 0; break;
    case 0x02: slot = 1; break;
    case 0x04: slot = 2; break;
    case 0x08: slot = 3; break;
    case 0x00:
        if (!allow_zero) return false;
        slot = -1;
        return true;
    default:
        return false;
    }
    box_.select_slot(slot);
    return true;
}

bool Dispatcher::busy() const {
    return box_.state() == BoxState::Preload || box_.state() == BoxState::Reload;
}

void Dispatcher::cmd_rfid_records(uint8_t mask) {
    uint8_t out[kMaxPayload];
    size_t n = 0;
    for (int s = 0; s < kSlotCount; ++s) {
        if (!(mask & (1u << s))) continue;
        const SlotRfid& r = box_.rfid[s];
        const char* text = r.has_record ? r.record : "none";
        const size_t len = r.has_record ? kTagRecordLen : 4;
        if (n + len + 3 > sizeof out) break;
        out[n++] = static_cast<uint8_t>('A' + s);
        out[n++] = ':';
        std::memcpy(out + n, text, len);
        n += len;
        out[n++] = ';';
    }
    reply(to_u8(Cmd::RfidRecords), Status::Ok, out, n);
}

void Dispatcher::cmd_rfid_remaining(uint8_t mask) {
    uint8_t out[kSlotCount];
    for (int s = 0; s < kSlotCount; ++s) {
        out[s] = 0xFF;
        if (!(mask & (1u << s)) || (loading_ && loading_slot_ == s)) continue;
        out[s] = box_.rfid[s].remaining_pct;
    }
    reply(to_u8(Cmd::RfidRemaining), Status::Ok, out, kSlotCount);
}

void Dispatcher::cmd_box_state() {
    uint8_t events[kSlotCount];
    if (box_.take_events(events)) {
        reply(to_u8(Cmd::BoxState), Status::SlotEvent, events, kSlotCount);
        return;
    }
    uint8_t p[6];
    p[0] = static_cast<uint8_t>(hw_.env.temperature_c());
    p[1] = hw_.env.humidity_pct();
    p[2] = box_.one_shot_event;
    if (box_.one_shot_event & 0x80) box_.one_shot_event &= 0x7F;
    p[4] = 0;
    p[5] = 0;
    for (int s = 0; s < kSlotCount; ++s) {
        if (hw_.sensors.hub_present(s)) p[4] |= static_cast<uint8_t>(1u << s);
        if (box_.rfid[s].remaining_pct != 0xFF) p[5] |= static_cast<uint8_t>(1u << s);
    }
    uint8_t status = 0;
    if (box_.state() == BoxState::Error) {
        status = box_.error();
    } else if (box_.state() == BoxState::Print) {
        status = to_u8(print_.runout_check());
    }
    p[3] = static_cast<uint8_t>(box_.state());
    reply_raw(to_u8(Cmd::BoxState), status, p, 6);
}

void Dispatcher::cmd_load(int slot, uint8_t stage, uint8_t arg) {
    loading_ = true;
    loading_slot_ = slot;
    Status st = Status::InvalidParam;
    switch (stage) {
    case 0: {
        st = feeder_.stage0_slot_to_hub(slot);
        const uint8_t s = static_cast<uint8_t>(slot);
        reply(to_u8(Cmd::Load), st, &s, 1);
        break;
    }
    case 1:
        box_.enter(BoxState::Reload);
        st = feeder_.stage1_hub_until_stop();
        box_.finish(st);
        reply(to_u8(Cmd::Load), st);
        break;
    case 2:
        box_.enter(BoxState::Reload);
        st = feeder_.stage2_hub_start_checked();
        box_.finish(st);
        reply(to_u8(Cmd::Load), st);
        break;
    case 3:
        box_.set_idle();
        reply(to_u8(Cmd::Load), Status::Ok);
        break;
    case 4: {
        reply(to_u8(Cmd::Load), Status::Ok);
        LoadLink link(*this);
        st = feeder_.stage4_feed_to_buffer(link);
        stage4_result_ = st;
        if (st != Status::Ok) box_.fail(st);
        break;
    }
    case 5: {
        uint8_t p[4];
        put_be_float(hw_.odometer.position_mm(), p);
        reply(to_u8(Cmd::Load), stage4_result_, p, 4);
        stage4_result_ = Status::Ok;
        break;
    }
    case 6:
        box_.enter(BoxState::Reload);
        st = feeder_.stage6_buffer_full();
        box_.finish(st);
        reply(to_u8(Cmd::Load), st);
        break;
    case 7:
        box_.enter(BoxState::Reload);
        st = feeder_.stage7_extruder_push(static_cast<float>(arg));
        box_.finish(st);
        reply(to_u8(Cmd::Load), st);
        break;
    default:
        break;  // stock: "illegal data", no reply
    }
    loading_ = false;
}

void Dispatcher::handle(const Frame& f) {
    if (AutoAddress::is_auto_command(f.cmd)) {
        auto_.handle(f);
        if (auto_.address() != address_) {
            address_ = auto_.address();
            rx_.set_address(address_);
        }
        auto_.poll();
        return;
    }
    if (f.addr == AutoAddress::kUnassigned || f.addr == AutoAddress::kBroadcast) {
        return;  // only auto-addressing uses these addresses
    }
    silent_ = f.addr == kAddrSilentBroadcast;
    reply_addr_ = f.addr == kAddrBoxBroadcast ? kAddrBoxBroadcast : address_;
    const uint8_t* p = f.payload;
    int slot = 0;
    auto bad_mask = [&] { reply(f.cmd, Status::InvalidParam); };

    switch (static_cast<Cmd>(f.cmd)) {
    case Cmd::KeepAlive:
        if (check_len(f, 0)) reply(f.cmd, Status::Ok);
        break;
    case Cmd::RfidRecords:
        if (check_len(f, 1)) cmd_rfid_records(p[0]);
        break;
    case Cmd::RfidRemaining:
        if (check_len(f, 1)) cmd_rfid_remaining(p[0]);
        break;
    case Cmd::Tracking:
        if (!check_len(f, 2)) break;
        if (!slot_from_mask(p[0], true, slot)) { bad_mask(); break; }
        if (p[1] == 0) {
            const Status st = slot < 0 ? Status::InvalidParam : print_.start(slot);
            if (st == Status::Ok && usage_) usage_->print_start(slot);
            reply(f.cmd, st);
        } else if (p[1] == 1) {
            if (usage_ && box_.state() == BoxState::Print) usage_->print_end(box_.current_slot());
            print_.stop();
            reply(f.cmd, Status::Ok);
        } else {
            reply(f.cmd, Status::InvalidParam);
        }
        break;
    case Cmd::Buffer:
        if (check_len(f, 0)) {
            const uint8_t b = static_cast<uint8_t>(hw_.sensors.buffer());
            reply(f.cmd, Status::Ok, &b, 1);
        }
        break;
    case Cmd::ManualFeeder:
        if (!check_len(f, 2)) break;
        if (!slot_from_mask(p[0], false, slot)) { bad_mask(); break; }
        if (busy()) { reply(f.cmd, Status::Busy); break; }
        if (p[1] == 1) hw_.feeders.run(slot, Dir::Forward, cfg_.speeds.feeder_forward);
        else if (p[1] == 2) hw_.feeders.run(slot, Dir::Reverse, cfg_.speeds.feeder_reverse);
        else if (p[1] == 0) hw_.feeders.stop_all();
        else { reply(f.cmd, Status::InvalidParam); break; }
        reply(f.cmd, Status::Ok);
        break;
    case Cmd::ManualHub:
        if (!check_len(f, 1)) break;
        if (busy()) { reply(f.cmd, Status::Busy); break; }
        if (p[0] == 1) hw_.hub.run(Dir::Forward, cfg_.speeds.hub_forward);
        else if (p[0] == 2) hw_.hub.run(Dir::Reverse, cfg_.speeds.hub_reverse);
        else if (p[0] == 0) hw_.hub.stop();
        else { reply(f.cmd, Status::InvalidParam); break; }
        reply(f.cmd, Status::Ok);
        break;
    case Cmd::SensorMask: {
        if (!check_len(f, 1)) break;
        if (p[0] > 1) { reply(f.cmd, Status::InvalidParam); break; }
        uint8_t m = 0;
        for (int s = 0; s < kSlotCount; ++s) {
            const bool present = p[0] == 0 ? hw_.sensors.inlet_present(s) : hw_.sensors.hub_present(s);
            if (present) m |= static_cast<uint8_t>(1u << s);
        }
        reply(f.cmd, Status::Ok, &m, 1);
        break;
    }
    case Cmd::SetManualSpeed: {
        if (!check_len(f, 3)) break;
        uint8_t* target = nullptr;
        if (p[0] == 0) target = p[1] == 0 ? &cfg_.speeds.feeder_forward : p[1] == 1 ? &cfg_.speeds.feeder_reverse : nullptr;
        if (p[0] == 1) target = p[1] == 0 ? &cfg_.speeds.hub_forward : p[1] == 1 ? &cfg_.speeds.hub_reverse : nullptr;
        if (!target) { reply(f.cmd, Status::InvalidParam); break; }
        *target = p[2];
        const uint8_t zero = 0;
        reply(f.cmd, Status::Ok, &zero, 1);
        break;
    }
    case Cmd::BoxState:
        if (check_len(f, 0)) cmd_box_state();
        break;
    case Cmd::SetIndicator:
        if (!check_len(f, 2)) break;
        if (!slot_from_mask(p[0], false, slot)) { bad_mask(); break; }
        hw_.indicators.set(slot, p[1]);
        reply(f.cmd, Status::Ok);
        break;
    case Cmd::GetMode:
        if (check_len(f, 0)) {
            const uint8_t m = hw_.settings.mode();
            reply(f.cmd, Status::Ok, &m, 1);
        }
        break;
    case Cmd::RfidControl: {
        if (!check_len(f, 2)) break;
        Status st = Status::Ok;
        if (p[1] == 0 || p[1] == 1) {
            box_.read_on_insert = p[1] == 1;
        } else if ((p[1] == 2 || p[1] == 3) && scanner_ && p[0] && p[0] <= 0x0F) {
            // Forced read of every masked slot, one at a time, while the box is idle.
            if (box_.state() != BoxState::Idle) {
                st = Status::Busy;
            } else {
                for (int s = 0; s < kSlotCount && st == Status::Ok; ++s) {
                    if (!(p[0] & (1u << s))) continue;
                    if (!hw_.sensors.inlet_present(s)) {
                        st = Status::UnloadNoFilament;
                        break;
                    }
                    loading_ = true;
                    loading_slot_ = s;
                    st = scanner_->scan(s).status;
                    loading_ = false;
                }
                box_.finish(st);
            }
        } else {
            st = Status::InvalidParam;
        }
        reply(f.cmd, st);
        break;
    }
    case Cmd::Odometer:
        if (!check_len(f, 1)) break;
        if (p[0] == 0) {
            hw_.odometer.reset();
            reply(f.cmd, Status::Ok);
        } else if (p[0] == 1) {
            uint8_t v[4];
            put_be_float(hw_.odometer.position_mm(), v);
            reply(f.cmd, Status::Ok, v, 4);
        } else {
            reply(f.cmd, Status::InvalidParam);
        }
        break;
    case Cmd::Option0F:
        if (!check_len(f, 1)) break;
        if (p[0] > 1) { reply(f.cmd, Status::InvalidParam); break; }
        box_.option_0f = p[0];
        reply(f.cmd, Status::Ok);
        break;
    case Cmd::Load:
        if (!check_len(f, 3)) break;
        if (!slot_from_mask(p[0], true, slot)) { bad_mask(); break; }
        cmd_load(slot, p[1], p[2]);
        break;
    case Cmd::Unload: {
        if (!check_len(f, 2)) break;
        if (!slot_from_mask(p[0], true, slot)) { bad_mask(); break; }
        loading_ = true;
        loading_slot_ = slot;
        if (usage_ && p[1] == 1 && slot >= 0) usage_->retract(slot);
        reply(f.cmd, feeder_.unload(slot, p[1]));
        loading_ = false;
        break;
    }
    case Cmd::FeedToHub:
        if (!check_len(f, 1)) break;
        if (!slot_from_mask(p[0], false, slot)) { bad_mask(); break; }
        reply(f.cmd, feeder_.feed_to_hub_and_settle(slot));
        break;
    case Cmd::Version: {
        if (!check_len(f, 0)) break;
        uint8_t v[22] = {'1', '5', '3'};
        hw_.system.serial_number(reinterpret_cast<char*>(v + 3));
        reply(f.cmd, Status::Ok, v, sizeof v);
        break;
    }
    case Cmd::ModuleStatus:
        if (check_len(f, 0)) {
            uint8_t v[16] = {};
            for (int i = 0; i < 6; ++i) {
                v[i] = static_cast<uint8_t>(((i + 1) << 4) | (box_.module_ok[i] ? 1 : 0));
            }
            reply(f.cmd, Status::Ok, v, sizeof v);
        }
        break;
    case Cmd::Output30:
        if (check_len(f, 1)) {
            hw_.system.set_aux_output(p[0] == 1);
            reply(f.cmd, Status::Ok);
        }
        break;
    case Cmd::UniqueId:
        if (check_len(f, 0)) {
            uint8_t uid[12];
            hw_.system.unique_id(uid);
            // Stock layout: word0, then (word1 & word2), little-endian.
            uint8_t v[8];
            for (int i = 0; i < 4; ++i) {
                v[i] = uid[i];
                v[4 + i] = static_cast<uint8_t>(uid[4 + i] & uid[8 + i]);
            }
            reply(f.cmd, Status::Ok, v, sizeof v);
        }
        break;
    case Cmd::SetMode:
        if (!check_len(f, 1)) break;
        if (p[0] == hw_.settings.mode()) {
            reply(f.cmd, Status::Busy);
            break;
        }
        hw_.settings.set_mode(p[0]);
        reply(f.cmd, Status::Ok);
        hw_.system.reset_after_ms(100);
        break;
    case Cmd::BoxIndex:
        if (f.len == 1 && f.addr == kAddrBoxBroadcast) box_.box_index = p[0];
        break;  // never answered
    case Cmd::GetEnvironment:
        if (check_len(f, 0)) {
            const uint8_t v[2] = {static_cast<uint8_t>(hw_.env.temperature_c()), hw_.env.humidity_pct()};
            reply(to_u8(Cmd::EnvironmentReply), Status::Ok, v, 2);
        }
        break;
    case Cmd::EnvironmentReply:
        if (f.len == 2) {
            box_.display_values[0] = p[0];
            box_.display_values[1] = p[1];
        }
        break;  // never answered
    case Cmd::Echo:
        if (check_len(f, 1)) {
            const uint8_t v = p[0] == 0xFF ? 0 : static_cast<uint8_t>(p[0] + 1);
            reply(f.cmd, Status::Ok, &v, 1);
        }
        break;
    case Cmd::Reboot:
        if (check_len(f, 0)) {
            reply(f.cmd, Status::Ok);
            hw_.system.reset_after_ms(100);
        }
        break;
    default:
        reply(f.cmd, Status::InvalidParam);
        break;
    }
}

}  // namespace cfs
