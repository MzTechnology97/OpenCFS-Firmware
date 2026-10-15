/*
 * OpenCFS - portable, testable lock-state reference model.
 * SPDX-License-Identifier: Apache-2.0
 * NOT a validated CFS physical safety interlock or flashable firmware.
 */
#include "opencfs/safety_gate.h"
#include <stddef.h>

static bool ocfs_gate_change(ocfs_gate_t *gate,
                             unsigned from,
                             unsigned to)
{
    unsigned expected = from;
    if (gate == NULL) {
        return false;
    }
    return atomic_compare_exchange_strong_explicit(
        &gate->state, &expected, to,
        memory_order_acq_rel, memory_order_acquire);
}

void ocfs_gate_init(ocfs_gate_t *gate)
{
    if (gate != NULL) {
        atomic_init(&gate->state, (unsigned)OCFS_GATE_UNQUALIFIED);
    }
}

ocfs_gate_state_t ocfs_gate_get_state(const ocfs_gate_t *gate)
{
    unsigned state;
    if (gate == NULL) {
        return OCFS_GATE_FAULT;
    }
    state = atomic_load_explicit(&gate->state, memory_order_acquire);
    if (state > (unsigned)OCFS_GATE_FAULT) {
        return OCFS_GATE_FAULT;
    }
    return (ocfs_gate_state_t)state;
}

bool ocfs_gate_submit_idle_evidence(ocfs_gate_t *gate, uint32_t evidence_flags)
{
    /* Fail closed for absent or unexpected evidence bits. */
    if (evidence_flags != OCFS_EVIDENCE_REQUIRED) {
        return false;
    }
    return ocfs_gate_change(gate, (unsigned)OCFS_GATE_UNQUALIFIED,
                            (unsigned)OCFS_GATE_VERIFIED_IDLE);
}

bool ocfs_gate_try_begin_motor(ocfs_gate_t *gate)
{
    return ocfs_gate_change(gate, (unsigned)OCFS_GATE_VERIFIED_IDLE,
                            (unsigned)OCFS_GATE_MOTOR_RESERVED);
}

bool ocfs_gate_try_begin_rfid(ocfs_gate_t *gate)
{
    return ocfs_gate_change(gate, (unsigned)OCFS_GATE_VERIFIED_IDLE,
                            (unsigned)OCFS_GATE_RFID_RESERVED);
}

bool ocfs_gate_end_motor(ocfs_gate_t *gate)
{
    return ocfs_gate_change(gate, (unsigned)OCFS_GATE_MOTOR_RESERVED,
                            (unsigned)OCFS_GATE_UNQUALIFIED);
}

bool ocfs_gate_end_rfid(ocfs_gate_t *gate)
{
    return ocfs_gate_change(gate, (unsigned)OCFS_GATE_RFID_RESERVED,
                            (unsigned)OCFS_GATE_UNQUALIFIED);
}

void ocfs_gate_latch_fault(ocfs_gate_t *gate)
{
    if (gate != NULL) {
        atomic_store_explicit(&gate->state, (unsigned)OCFS_GATE_FAULT,
                              memory_order_release);
    }
}
