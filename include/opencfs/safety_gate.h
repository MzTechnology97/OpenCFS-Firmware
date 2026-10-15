/*
 * OpenCFS - source-only motion/RFID arbitration reference model.
 * SPDX-License-Identifier: Apache-2.0
 *
 * WARNING: NOT a hardware safety interlock. All feedback is synthetic
 * until qualified independent GD32 hardware sensing and full task/ISR
 * ownership have been established. This module may be unit tested and
 * cross-compiled but MUST NOT authorize real CFS MCU SET/RESET.
 */
#ifndef OPEN_CFS_SAFETY_GATE_H
#define OPEN_CFS_SAFETY_GATE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OCFS_GATE_UNQUALIFIED = 0,
    OCFS_GATE_VERIFIED_IDLE = 1,
    OCFS_GATE_MOTOR_RESERVED = 2,
    OCFS_GATE_RFID_RESERVED = 3,
    OCFS_GATE_FAULT = 4
} ocfs_gate_state_t;

/*
 * Evidence flags are a design contract, NOT actual GPIO/ADC measurements.
 * A board-qualified implementation must provide genuinely independent
 * stop feedback, fresh timestamps, inhibited outputs and a shared
 * exclusion domain covering ALL motor entry points and interrupts.
 * No board implementation of these flags exists yet.
 */
enum {
    OCFS_EVIDENCE_FRESH = 1u << 0,
    OCFS_EVIDENCE_INDEPENDENT_STOP = 1u << 1,
    OCFS_EVIDENCE_DRIVER_INHIBITED = 1u << 2,
    OCFS_EVIDENCE_EXCLUSIVE_MOTOR_DOMAIN = 1u << 3,
    OCFS_EVIDENCE_REQUIRED = (1u << 4) - 1u
};

typedef struct {
    atomic_uint state;
} ocfs_gate_t;

/* Reset on cold initialization only; not safe to invoke concurrently. */
void ocfs_gate_init(ocfs_gate_t *gate);
ocfs_gate_state_t ocfs_gate_get_state(const ocfs_gate_t *gate);

/* For simulation only until a trusted independent board provider exists. */
bool ocfs_gate_submit_idle_evidence(ocfs_gate_t *gate, uint32_t evidence_flags);

/* Atomically reserve mutually exclusive MOTOR or RFID operations. */
bool ocfs_gate_try_begin_motor(ocfs_gate_t *gate);
bool ocfs_gate_try_begin_rfid(ocfs_gate_t *gate);

/* Completion returns to UNQUALIFIED, never assumes physical idle. */
bool ocfs_gate_end_motor(ocfs_gate_t *gate);
bool ocfs_gate_end_rfid(ocfs_gate_t *gate);

/* FAULT is latched until explicitly reinitialized at cold start. */
void ocfs_gate_latch_fault(ocfs_gate_t *gate);

#ifdef __cplusplus
}
#endif

#endif /* OPEN_CFS_SAFETY_GATE_H */
