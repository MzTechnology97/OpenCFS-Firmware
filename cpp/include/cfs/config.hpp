// SPDX-License-Identifier: Apache-2.0
// Tunables with the stock defaults documented in docs/04-feeder.md.
#pragma once

#include <cstdint>

namespace cfs {

struct Speeds {
    uint8_t feeder_forward = 255;
    uint8_t hub_forward = 100;
    uint8_t hub_transition = 200;
    uint8_t hub_extruder_push = 150;
    uint8_t hub_insert = 155;
    uint8_t feeder_reverse = 255;
    uint8_t hub_reverse = 80;
    // Print refill (buffer empty interrupt). The stock value lives in RAM
    // (BOX_OPER_STATE + 0x14) and is not recovered yet: use hub_forward.
    uint8_t hub_print_refill = 100;
    uint8_t hub_print_assist = 180;
    // RFID margin estimation.
    uint8_t rfid_seek = 160;
    uint8_t rfid_measure = 120;
    uint8_t rfid_reposition = 100;
};

struct Timing {
    uint32_t sample_ms = 20;               // odometer sampling period
    float min_move_mm = 0.01f;             // movement threshold per sample
    uint32_t hub_sensor_timeout_ms = 7000;
    uint32_t hub_outside_timeout_ms = 2000;
    float hub_outside_mm = 20.0f;
    uint32_t hub_transition_wait_ms = 3200;
    uint32_t feed_total_samples = 1250;    // 25 s
    uint32_t stall_samples = 25;           // 500 ms
    float overtravel_mm = 3000.0f;
    uint32_t buffer_full_timeout_ms = 3000;
    uint32_t stage7_samples = 15;          // 300 ms
    uint32_t unload_buffer_timeout_ms = 5000;
    uint32_t pull_back_samples = 2000;     // 40 s
    uint32_t pull_back_stall_samples = 150;
    uint32_t hub_clear_timeout_ms = 7000;
    uint32_t recovery_timeout_ms = 3000;
    uint32_t inlet_check_attempts = 30;
    uint32_t inlet_check_samples = 5;
    // Print refill session.
    uint32_t refill_window_ms = 10000;
    uint32_t refill_assist_after_ms = 5000;
    uint32_t refill_assist_ms = 1000;
    float refill_min_mm = 5.0f;
    float refill_max_mm = 40.0f;
    uint32_t refill_max_retriggers = 2;
};

struct SpoolGeometry {
    float core_diameter_mm = 96.0f;
    float full_diameter_mm = 172.0f;
};

struct Config {
    Speeds speeds;
    Timing timing;
    SpoolGeometry spool;
};

}  // namespace cfs
