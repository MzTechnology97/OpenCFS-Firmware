/*
 * OpenCFS - deterministic host-only reference tests.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "opencfs/safety_gate.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>

enum { CONTENDERS = 16 };

typedef struct {
    ocfs_gate_t *gate;
    atomic_bool *go;
    atomic_uint *ready;
    atomic_uint *won;
    bool motor;
} contender_t;

static void *contender_main(void *context)
{
    contender_t *thread = (contender_t *)context;
    atomic_fetch_add_explicit(thread->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(thread->go, memory_order_acquire)) {
        sched_yield();
    }
    bool success = thread->motor
        ? ocfs_gate_try_begin_motor(thread->gate)
        : ocfs_gate_try_begin_rfid(thread->gate);
    if (success) {
        atomic_fetch_add_explicit(thread->won, 1u, memory_order_relaxed);
    }
    return NULL;
}

static void test_fail_closed_and_requalification(void)
{
    ocfs_gate_t gate;
    ocfs_gate_init(&gate);
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_UNQUALIFIED);
    assert(!ocfs_gate_try_begin_motor(&gate));
    assert(!ocfs_gate_try_begin_rfid(&gate));
    assert(!ocfs_gate_submit_idle_evidence(&gate, 0u));
    assert(!ocfs_gate_submit_idle_evidence(&gate,
                                           OCFS_EVIDENCE_REQUIRED & ~OCFS_EVIDENCE_FRESH));
    assert(!ocfs_gate_submit_idle_evidence(&gate,
                                           OCFS_EVIDENCE_REQUIRED | (1u << 8)));

    /* These are SYNTHETIC host-only flags, not physical measurements. */
    assert(ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_VERIFIED_IDLE);
    assert(!ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));

    assert(ocfs_gate_try_begin_motor(&gate));
    assert(!ocfs_gate_try_begin_motor(&gate));
    assert(!ocfs_gate_try_begin_rfid(&gate));
    assert(!ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    assert(ocfs_gate_end_motor(&gate));
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_UNQUALIFIED);
    assert(!ocfs_gate_end_motor(&gate));
    assert(!ocfs_gate_try_begin_rfid(&gate));

    assert(ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    assert(ocfs_gate_try_begin_rfid(&gate));
    assert(!ocfs_gate_try_begin_motor(&gate));
    assert(!ocfs_gate_try_begin_rfid(&gate));
    assert(ocfs_gate_end_rfid(&gate));
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_UNQUALIFIED);
    assert(!ocfs_gate_end_rfid(&gate));
}

static void test_fault_is_latched(void)
{
    ocfs_gate_t gate;
    ocfs_gate_init(&gate);
    assert(ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    assert(ocfs_gate_try_begin_rfid(&gate));
    ocfs_gate_latch_fault(&gate);
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_FAULT);
    assert(!ocfs_gate_end_rfid(&gate));
    assert(!ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    assert(!ocfs_gate_try_begin_motor(&gate));
    assert(!ocfs_gate_try_begin_rfid(&gate));

    /* Cold initialization is the sole reset operation in the model. */
    ocfs_gate_init(&gate);
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_UNQUALIFIED);
}

static void test_simultaneous_motor_and_rfid_claims(void)
{
    ocfs_gate_t gate;
    pthread_t handles[CONTENDERS];
    contender_t threads[CONTENDERS];
    atomic_bool go;
    atomic_uint ready, won;

    ocfs_gate_init(&gate);
    assert(ocfs_gate_submit_idle_evidence(&gate, OCFS_EVIDENCE_REQUIRED));
    atomic_init(&go, false);
    atomic_init(&ready, 0u);
    atomic_init(&won, 0u);

    for (unsigned i = 0; i < CONTENDERS; ++i) {
        threads[i] = (contender_t){
            .gate = &gate,
            .go = &go,
            .ready = &ready,
            .won = &won,
            .motor = (i % 2u) == 0u,
        };
        assert(pthread_create(&handles[i], NULL, contender_main, &threads[i]) == 0);
    }
    while (atomic_load_explicit(&ready, memory_order_acquire) != CONTENDERS) {
        sched_yield();
    }
    atomic_store_explicit(&go, true, memory_order_release);
    for (unsigned i = 0; i < CONTENDERS; ++i) {
        assert(pthread_join(handles[i], NULL) == 0);
    }
    assert(atomic_load_explicit(&won, memory_order_acquire) == 1u);

    ocfs_gate_state_t state = ocfs_gate_get_state(&gate);
    assert(state == OCFS_GATE_MOTOR_RESERVED || state == OCFS_GATE_RFID_RESERVED);
    assert(state == OCFS_GATE_MOTOR_RESERVED
           ? ocfs_gate_end_motor(&gate) : ocfs_gate_end_rfid(&gate));
    assert(ocfs_gate_get_state(&gate) == OCFS_GATE_UNQUALIFIED);
}

static void test_null_is_never_permissive(void)
{
    assert(ocfs_gate_get_state(NULL) == OCFS_GATE_FAULT);
    assert(!ocfs_gate_submit_idle_evidence(NULL, OCFS_EVIDENCE_REQUIRED));
    assert(!ocfs_gate_try_begin_motor(NULL));
    assert(!ocfs_gate_try_begin_rfid(NULL));
    assert(!ocfs_gate_end_motor(NULL));
    assert(!ocfs_gate_end_rfid(NULL));
    ocfs_gate_init(NULL);
    ocfs_gate_latch_fault(NULL);
}

int main(void)
{
    test_fail_closed_and_requalification();
    test_fault_is_latched();
    test_simultaneous_motor_and_rfid_claims();
    test_null_is_never_permissive();
    puts("OpenCFS safety model: PASS (simulation only; no MCU guarantee)");
    return 0;
}
