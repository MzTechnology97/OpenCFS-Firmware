// SPDX-License-Identifier: Apache-2.0
#include "cfs/runtime_config.hpp"

namespace cfs {

// ---- interlock ----------------------------------------------------------------------
bool MotionInterlock::idle_now() {
    // Evaluate every source each time: outputs_idle() also tracks FG pulses.
    const bool feeders = hw_.feeders.outputs_idle();
    const bool hub = hw_.hub.outputs_idle();
    return box_.state() == BoxState::Idle && !box_.motion_active() && feeders && hub &&
           hw_.hub.current_speed() == 0;
}

void MotionInterlock::poll() {
    const uint32_t now = hw_.clock.now_ms();
    if (!started_ || !idle_now()) busy_at_ = now;  // counted as busy until seen at rest
    started_ = true;
}

bool MotionInterlock::at_rest() {
    poll();
    return hw_.clock.now_ms() - busy_at_ >= kSettleMs;
}

// ---- parameter table ------------------------------------------------------------------
namespace {
using P = RuntimeConfig::Param;
constexpr uint8_t W = RuntimeConfig::kFlagWritable | RuntimeConfig::kFlagIdleOnly;
constexpr uint8_t ADV = RuntimeConfig::kFlagAdvanced;
constexpr uint8_t RF = RuntimeConfig::kFlagRfid;
constexpr uint8_t SAFE = RuntimeConfig::kFlagSafety;
constexpr uint8_t U8 = RuntimeConfig::kTypeU8;
constexpr uint8_t U16 = RuntimeConfig::kTypeU16;
constexpr uint8_t BOOL = RuntimeConfig::kTypeBool;

#define CFS_U8(field)                                                         \
    [](const Config& c) -> uint16_t { return c.field; },                      \
        [](Config& c, uint16_t v) { c.field = static_cast<uint8_t>(v); }
#define CFS_U32(field)                                                        \
    [](const Config& c) -> uint16_t { return static_cast<uint16_t>(c.field); }, \
        [](Config& c, uint16_t v) { c.field = v; }
#define CFS_READ_ONLY(field) [](const Config& c) -> uint16_t { return c.field; }, nullptr
#define CFS_NOT_MODELED nullptr, nullptr
}  // namespace

// IDs, names, defaults and ranges follow the box_cfs_runtime.py catalogue; the
// bindings are OpenCFS's (docs/12-runtime-config.md section 2).
const RuntimeConfig::Param RuntimeConfig::kParams[kCount] = {
    // 0 feeder_forward_speed: feeders are GPIO on/off, a speed has no effect.
    P{U8, 0, 255, 1, 255, CFS_READ_ONLY(speeds.feeder_forward)},
    P{U8, W, 100, 1, 255, CFS_U8(speeds.hub_forward)},       // 1 hub_forward_speed
    P{U8, W, 200, 1, 255, CFS_U8(speeds.hub_transition)},    // 2 hub_transition_speed
    P{U8, W, 155, 1, 255, CFS_U8(speeds.hub_insert)},        // 3 hub_insert_speed
    P{U8, 0, 255, 1, 255, CFS_READ_ONLY(speeds.feeder_reverse)},  // 4 feeder_reverse_speed
    P{U8, W, 80, 1, 255, CFS_U8(speeds.hub_reverse)},        // 5 hub_reverse_speed
    // 6 reverse_detooth_enable
    P{BOOL, W | ADV | SAFE, 1, 0, 1,
      [](const Config& c) -> uint16_t { return c.supervision.reverse_detooth ? 1 : 0; },
      [](Config& c, uint16_t v) { c.supervision.reverse_detooth = v != 0; }},
    P{U16, W | ADV | SAFE, 3200, 100, 10000, CFS_U32(timing.hub_transition_wait_ms)},  // 7
    // 8 insert_sensor_timeout: stock counts 10 ms polls (wait_hub_sensor(slot, 700, 0, 10)).
    P{U16, W | ADV | SAFE, 700, 100, 5000,
      [](const Config& c) -> uint16_t { return static_cast<uint16_t>(c.timing.hub_sensor_timeout_ms / 10); },
      [](Config& c, uint16_t v) { c.timing.hub_sensor_timeout_ms = 10u * v; }},
    P{U16, W | ADV | SAFE, 5000, 500, 15000, CFS_U32(timing.unload_buffer_timeout_ms)},  // 9
    P{U16, ADV | SAFE, 1000, 100, 5000, CFS_NOT_MODELED},     // 10 unload_sensor_timeout (10 ms polls)
    P{U8, W | ADV | RF, 160, 1, 255, CFS_U8(speeds.rfid_seek)},          // 11
    P{U8, W | ADV | RF, 120, 1, 255, CFS_U8(speeds.rfid_measure)},       // 12
    P{U8, W | ADV | RF, 100, 1, 255, CFS_U8(speeds.rfid_reposition)},    // 13
    P{U16, W | ADV | RF, 800, 100, 5000, CFS_U32(timing.rfid_seek_move_ms)},      // 14
    P{U16, W | ADV | RF, 200, 10, 2000, CFS_U32(timing.rfid_seek_settle_ms)},     // 15
    P{U16, W | ADV | RF, 1000, 100, 5000, CFS_U32(timing.rfid_retry_reverse_ms)}, // 16
    P{U16, ADV | RF, 10, 1, 1000, CFS_NOT_MODELED},           // 17 rfid_pre_measure_settle
    P{U16, W | ADV | RF, 400, 50, 2000, CFS_U32(timing.rfid_neighbor_move_ms)},   // 18
    P{U16, W | ADV | RF, 100, 10, 1000, CFS_U32(timing.rfid_neighbor_settle_ms)},  // 19
    P{U16, W | ADV | SAFE, 1250, 250, 3000, CFS_U32(timing.feed_total_samples)},  // 20 (20 ms ticks)
    P{U16, W | ADV | SAFE, 3000, 500, 10000, CFS_U32(timing.recovery_timeout_ms)},  // 21
    P{U16, W | ADV | SAFE, 25, 5, 100, CFS_U32(timing.stall_samples)},           // 22 (20 ms ticks)
    P{U16, W | ADV | RF, 3, 1, 10, CFS_U32(timing.rfid_locate_attempts)},        // 23
    P{U16, W | ADV | SAFE, 1000, 100, 10000, CFS_U32(supervision.detooth_after_ms)},  // 24
    P{U16, ADV | SAFE, 30000, 1000, 60000, CFS_NOT_MODELED},  // 25 buffer_fill_timeout
    P{U16, ADV | SAFE, 300, 50, 5000, CFS_NOT_MODELED},       // 26 transition_detect_timeout
    P{U16, ADV | SAFE, 3, 1, 10, CFS_NOT_MODELED},            // 27 load_retry_count
};

#undef CFS_U8
#undef CFS_U32
#undef CFS_READ_ONLY
#undef CFS_NOT_MODELED

// ---- commands -------------------------------------------------------------------------
namespace {
uint16_t value_of(const RuntimeConfig::Param& p, const Config& cfg) { return p.get ? p.get(cfg) : p.def; }

void put16(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>(v);
    out[1] = static_cast<uint8_t>(v >> 8);
}
}  // namespace

uint8_t RuntimeConfig::handle(const uint8_t* req, size_t len, uint8_t* out, size_t& out_len) {
    out_len = 0;
    if (len < 1) return kStatusBadRequest;
    const uint8_t sub = req[0];
    switch (sub) {
    case kSubInfo:
        if (len != 1) return kStatusBadRequest;
        out[0] = kVersion;
        out[1] = kCount;
        out[2] = kDescriptorRevision;
        out[3] = kFeatures;
        out_len = 4;
        return kStatusOk;
    case kSubGet:
    case kSubDescribe: {
        if (len != 2 || req[1] >= kCount) return kStatusBadRequest;
        const Param& p = kParams[req[1]];
        if (sub == kSubGet) {
            out[0] = req[1];
            put16(out + 1, value_of(p, cfg_));
            out_len = 3;
        } else {
            out[0] = p.type;
            out[1] = p.flags;
            put16(out + 2, p.def);
            put16(out + 4, p.min);
            put16(out + 6, p.max);
            out_len = 8;
        }
        return kStatusOk;
    }
    case kSubSet: {
        if (len != 4 || req[1] >= kCount) return kStatusBadRequest;
        const Param& p = kParams[req[1]];
        const uint16_t v = static_cast<uint16_t>(req[2] | req[3] << 8);
        if (!p.set || v < p.min || v > p.max) return kStatusBadRequest;
        if (!interlock_.at_rest()) return kStatusBusy;
        p.set(cfg_, v);  // same main-loop pass as the check: no motor can start in between
        out[0] = req[1];
        put16(out + 1, value_of(p, cfg_));
        out_len = 3;
        return kStatusOk;
    }
    case kSubReset: {
        if (len != 2 || (req[1] >= kCount && req[1] != 0xFF)) return kStatusBadRequest;
        if (!interlock_.at_rest()) return kStatusBusy;
        for (uint8_t id = 0; id < kCount; ++id) {
            if ((req[1] == 0xFF || req[1] == id) && kParams[id].set) kParams[id].set(cfg_, kParams[id].def);
        }
        return kStatusOk;
    }
    default:
        return kStatusBadRequest;  // other 0x57 sub-commands are K2-OpenHost only
    }
}

}  // namespace cfs
